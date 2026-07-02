#include "goblin_debug_rpc.hpp"

#include "goblin_config.hpp"
#include "goblin_inject.hpp"   // world_map_open() — status field for the driver's boot/nav loop
#include "goblin_pause.hpp"    // pause command + paused= status (unfocused-window pause escape)
#include "goblin_overlay.hpp"
#include "goblin_overlay_render_loader.hpp"

#include <spdlog/spdlog.h>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace goblin::debug_rpc
{
    namespace
    {
        struct Pending
        {
            std::string request;  // one line, no trailing \n
            std::string reply;    // filled by pump() on the present thread
            HANDLE done = nullptr;
            ~Pending() { if (done) CloseHandle(done); }
        };

        std::mutex g_queue_mutex;
        // shared_ptr on purpose: on a listener timeout, pump() may STILL be executing the command —
        // its own reference keeps the Pending (and the event handle) alive until SetEvent returns,
        // no use-after-free however late the present thread runs.
        std::deque<std::shared_ptr<Pending>> g_queue;
        std::atomic<bool> g_has_work{false};    // cheap per-frame gate so pump() stays ~free when idle

        std::string next_token(std::string &s)
        {
            size_t b = s.find_first_not_of(" \t");
            if (b == std::string::npos) { s.clear(); return {}; }
            size_t e = s.find_first_of(" \t", b);
            std::string tok = s.substr(b, e == std::string::npos ? std::string::npos : e - b);
            s.erase(0, e == std::string::npos ? s.size() : e + 1);
            return tok;
        }

        // ── Input-injection commands (Phase 4 unblocker: drive menus / load a save / hover the
        // map from the driver). These run on the LISTENER thread, not the present-thread pump:
        // SendInput is thread-agnostic OS-queue injection, touches no overlay/game state, and a
        // key-hold Sleep() on the present thread would hitch frames. Scancodes included because
        // the game reads keyboard via raw input (legacy-message-only synthesis would be invisible
        // to it); mouse moves via MOUSEEVENTF_ABSOLUTE rather than SetCursorPos so we don't feed
        // our own hk_set_cursor_pos hook (which swallows recenter calls while the panel is open).

        bool ensure_game_foreground()
        {
            HWND hwnd = static_cast<HWND>(goblin::overlay::game_hwnd());
            if (!hwnd) return false;
            if (GetForegroundWindow() != hwnd) SetForegroundWindow(hwnd);
            return true;
        }

        void send_vk(uint16_t vk, bool up)
        {
            INPUT in{};
            in.type = INPUT_KEYBOARD;
            in.ki.wVk = vk;
            in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
            // Character keys go VK-only: with KEYEVENTF_SCANCODE set, letters never reached the
            // game under Wine + a non-QWERTY host layout (Enter/Escape worked, E/G/Q didn't —
            // observed live 2026-07-02); the scan→layout remap in Wine's injection path drops or
            // mistranslates them. Non-char keys keep the scancode (that's what raw-input readers
            // want, and their scancodes are layout-independent).
            const bool char_key = (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9');
            in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0u) |
                            ((!char_key && in.ki.wScan) ? KEYEVENTF_SCANCODE : 0u);
            // Extended keys (arrows, Ins/Del/Home/End/PgUp/PgDn) need the flag or they alias the
            // numpad scancodes.
            switch (vk)
            {
            case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
            case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
            case VK_PRIOR: case VK_NEXT:
                in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
                break;
            default: break;
            }
            SendInput(1, &in, sizeof(in));
        }

        bool client_to_abs(int cx, int cy, LONG &ax, LONG &ay)
        {
            HWND hwnd = static_cast<HWND>(goblin::overlay::game_hwnd());
            if (!hwnd) return false;
            POINT p{cx, cy};
            ClientToScreen(hwnd, &p);
            int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            if (vw <= 0 || vh <= 0) return false;
            ax = static_cast<LONG>((p.x - vx) * 65535.0 / (vw - 1));
            ay = static_cast<LONG>((p.y - vy) * 65535.0 / (vh - 1));
            return true;
        }

        void send_mouse_abs(LONG ax, LONG ay, DWORD extra_flags)
        {
            INPUT in{};
            in.type = INPUT_MOUSE;
            in.mi.dx = ax;
            in.mi.dy = ay;
            in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | extra_flags;
            SendInput(1, &in, sizeof(in));
        }

        // key <name> [hold_ms] | mouse_move <cx> <cy> | mouse_click [left|right] [<cx> <cy>]
        // Coordinates are CLIENT pixels of the game window (same space as screenshots).
        std::string execute_input(const std::string &cmd, std::string rest)
        {
            if (!ensure_game_foreground()) return "err game window not resolved yet";
            if (cmd == "key")
            {
                std::string name = next_token(rest), hold = next_token(rest);
                if (name.empty()) return "err usage: key <name> [hold_ms]";
                uint32_t vk = goblin::parse_vk_code(name);
                if (!vk) return "err unknown key name: " + name;
                int hold_ms = 60;
                if (!hold.empty())
                {
                    try { hold_ms = std::stoi(hold); } catch (...) { return "err bad hold_ms"; }
                    if (hold_ms < 1 || hold_ms > 5000) return "err hold_ms out of range (1-5000)";
                }
                send_vk(static_cast<uint16_t>(vk), false);
                Sleep(static_cast<DWORD>(hold_ms));
                send_vk(static_cast<uint16_t>(vk), true);
                return "ok key " + name;
            }
            if (cmd == "mouse_move")
            {
                std::string xs = next_token(rest), ys = next_token(rest);
                int cx = 0, cy = 0;
                try { cx = std::stoi(xs); cy = std::stoi(ys); }
                catch (...) { return "err usage: mouse_move <client_x> <client_y>"; }
                LONG ax = 0, ay = 0;
                if (!client_to_abs(cx, cy, ax, ay)) return "err coordinate mapping failed";
                send_mouse_abs(ax, ay, 0);
                return "ok mouse_move";
            }
            if (cmd == "mouse_click")
            {
                std::string btn = next_token(rest);
                bool right = btn == "right";
                if (!btn.empty() && btn != "left" && btn != "right")
                {
                    // No button given → first token was the x coordinate.
                    rest = btn + " " + rest;
                    right = false;
                }
                std::string xs = next_token(rest), ys = next_token(rest);
                if (!xs.empty())
                {
                    int cx = 0, cy = 0;
                    try { cx = std::stoi(xs); cy = std::stoi(ys); }
                    catch (...) { return "err usage: mouse_click [left|right] [<x> <y>]"; }
                    LONG ax = 0, ay = 0;
                    if (!client_to_abs(cx, cy, ax, ay)) return "err coordinate mapping failed";
                    send_mouse_abs(ax, ay, 0);
                    Sleep(30);
                }
                INPUT in{};
                in.type = INPUT_MOUSE;
                in.mi.dwFlags = right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
                SendInput(1, &in, sizeof(in));
                Sleep(40);
                in.mi.dwFlags = right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
                SendInput(1, &in, sizeof(in));
                return "ok mouse_click";
            }
            return "err not an input command";  // unreachable via dispatch below
        }

        bool is_input_command(const std::string &cmd)
        {
            return cmd == "key" || cmd == "mouse_move" || cmd == "mouse_click";
        }

        // Present thread. Every handler here may touch overlay/config state freely — pump() is
        // called from hk_present, the same thread that owns that state.
        std::string execute(const std::string &line, IDXGISwapChain3 *swapchain)
        {
            std::string rest = line;
            std::string cmd = next_token(rest);
            if (cmd == "ping") return "ok pong";
            if (cmd == "status")
            {
                bool hot =
#if defined(GOBLIN_OVERLAY_HOTRELOAD_BUILD)
                    true;
#else
                    false;
#endif
                return "ok panel=" + std::to_string(goblin::overlay::panel_open() ? 1 : 0) +
                       " hotreload=" + std::to_string(hot ? 1 : 0) +
                       " gen=" + std::to_string(goblin::overlay_render_loader::render_generation()) +
                       " reload_pending=" +
                       std::to_string(goblin::overlay_render_loader::reload_pending() ? 1 : 0) +
                       " map_open=" + std::to_string(goblin::world_map_open() ? 1 : 0) +
                       " paused=" + (goblin::pause::available()
                                         ? std::to_string(goblin::pause::paused() ? 1 : 0)
                                         : std::string("na"));
            }
            if (cmd == "open_f1")
            {
                std::string arg = next_token(rest);
                if (arg == "0")
                    goblin::overlay::set_panel_open(false);
                else if (arg == "1")
                    goblin::overlay::set_panel_open(true);
                else if (arg == "toggle" || arg.empty())
                    goblin::overlay::set_panel_open(!goblin::overlay::panel_open());
                else
                    return "err open_f1 takes 0|1|toggle";
                return "ok panel=" + std::to_string(goblin::overlay::panel_open() ? 1 : 0);
            }
            if (cmd == "set")
            {
                std::string key = next_token(rest);
                // rest (trimmed) is the value — may contain spaces (e.g. show_all_except lists)
                size_t b = rest.find_first_not_of(" \t");
                std::string value = b == std::string::npos ? std::string{} : rest.substr(b);
                if (key.empty()) return "err usage: set <ini_key> <value>";
                if (!goblin::config_set_by_key(key, value)) return "err unknown key (or ERR-only off-ERR): " + key;
                return "ok " + key + "=" + value;
            }
            if (cmd == "screenshot")
            {
                size_t b = rest.find_first_not_of(" \t");
                std::string path = b == std::string::npos ? std::string{} : rest.substr(b);
                if (path.empty()) return "err usage: screenshot <path.bmp>";
                std::string err;
                if (!goblin::overlay::screenshot_to_file(swapchain, path, err)) return "err " + err;
                return "ok " + path;
            }
            if (cmd == "pause")
            {
                if (!goblin::pause::available()) return "err pause branch not resolved (game update?)";
                std::string arg = next_token(rest);
                if (arg == "0")
                    goblin::pause::set_paused(false);
                else if (arg == "1")
                    goblin::pause::set_paused(true);
                else if (arg == "toggle" || arg.empty())
                    goblin::pause::set_paused(!goblin::pause::paused());
                else
                    return "err pause takes 0|1|toggle";
                return "ok paused=" + std::to_string(goblin::pause::paused() ? 1 : 0);
            }
            if (cmd == "reload_overlay")
            {
                if (!goblin::overlay_render_loader::request_reload())
                    return "err not a hotreload build (GOBLIN_OVERLAY_HOTRELOAD=OFF)";
                return "ok reload flagged, poll status for gen bump";
            }
            return "err unknown command: " + cmd;
        }

        void serve_client(SOCKET c)
        {
            std::string buf;
            char chunk[512];
            for (;;)
            {
                size_t nl;
                while ((nl = buf.find('\n')) == std::string::npos)
                {
                    int n = recv(c, chunk, sizeof(chunk), 0);
                    if (n <= 0) return;
                    buf.append(chunk, n);
                    if (buf.size() > 8192) return;  // garbage client
                }
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;

                // Input-injection commands execute right here on the listener thread (see
                // execute_input's rationale) — everything else marshals to the present thread.
                {
                    std::string peek = line, cmd = next_token(peek);
                    if (is_input_command(cmd))
                    {
                        std::string reply = execute_input(cmd, peek) + "\n";
                        size_t off = 0;
                        while (off < reply.size())
                        {
                            int n = send(c, reply.c_str() + off, static_cast<int>(reply.size() - off), 0);
                            if (n <= 0) return;
                            off += n;
                        }
                        continue;
                    }
                }

                auto p = std::make_shared<Pending>();
                p->request = line;
                p->done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (!p->done)
                {
                    const char *msg = "err event creation failed\n";
                    send(c, msg, static_cast<int>(strlen(msg)), 0);
                    continue;
                }
                {
                    std::lock_guard<std::mutex> lk(g_queue_mutex);
                    g_queue.push_back(p);
                    g_has_work.store(true, std::memory_order_release);
                }
                std::string reply;
                if (WaitForSingleObject(p->done, 10000) == WAIT_OBJECT_0)
                    reply = p->reply;
                else
                {
                    // Un-queue if pump never took it (game not presenting); if pump ALREADY holds
                    // it, it's off the queue — wait again briefly for the in-flight execute.
                    bool still_queued = false;
                    {
                        std::lock_guard<std::mutex> lk(g_queue_mutex);
                        for (auto it = g_queue.begin(); it != g_queue.end(); ++it)
                            if (it->get() == p.get())
                            {
                                g_queue.erase(it);
                                still_queued = true;
                                break;
                            }
                    }
                    if (!still_queued && WaitForSingleObject(p->done, 5000) == WAIT_OBJECT_0)
                        reply = p->reply;
                    else
                        reply = "err timeout (no frames presenting?)";
                }
                reply += "\n";
                // send() may transmit partially — loop; a failed send means the client is gone.
                size_t off = 0;
                while (off < reply.size())
                {
                    int n = send(c, reply.c_str() + off, static_cast<int>(reply.size() - off), 0);
                    if (n <= 0) return;
                    off += n;
                }
            }
        }

        void listener_main(unsigned short port)
        {
            WSADATA wsa{};
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            {
                spdlog::error("[RPC] WSAStartup failed → debug RPC disabled");
                return;
            }
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET)
            {
                spdlog::error("[RPC] socket() failed, wsa={} → debug RPC disabled", WSAGetLastError());
                return;
            }
            BOOL yes = TRUE;
            setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&yes), sizeof(yes));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);  // loopback ONLY — never a real interface
            if (bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR ||
                listen(s, 1) == SOCKET_ERROR)
            {
                spdlog::error("[RPC] bind/listen 127.0.0.1:{} failed, wsa={} → debug RPC disabled", port,
                              WSAGetLastError());
                closesocket(s);
                return;
            }
            spdlog::info("[RPC] debug RPC listening on 127.0.0.1:{} (dev-only; tools/mfg_rpc.py)", port);
            for (;;)
            {
                SOCKET c = accept(s, nullptr, nullptr);
                if (c == INVALID_SOCKET) continue;
                // Generous idle timeout so a hung/leaked client can't starve accept() forever
                // (one client at a time); an interactive driver just reconnects after.
                DWORD rcv_to = 10 * 60 * 1000;
                setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&rcv_to), sizeof(rcv_to));
                serve_client(c);  // one client at a time — a dev driver, not a server
                closesocket(c);
            }
        }
    }

    void initialize()
    {
        const std::string &p = goblin::config::debugRpcPort;
        if (p.empty()) return;
        int port = 0;
        try { port = std::stoi(p); } catch (...) { port = 0; }
        if (port <= 0 || port > 65535)
        {
            spdlog::warn("[RPC] debug_rpc_port '{}' invalid (1-65535) → debug RPC disabled", p);
            return;
        }
        std::thread(listener_main, static_cast<unsigned short>(port)).detach();
    }

    void pump(IDXGISwapChain3 *swapchain)
    {
        if (!g_has_work.load(std::memory_order_acquire)) return;
        for (;;)
        {
            std::shared_ptr<Pending> p;
            {
                std::lock_guard<std::mutex> lk(g_queue_mutex);
                if (g_queue.empty())
                {
                    g_has_work.store(false, std::memory_order_release);
                    return;
                }
                p = g_queue.front();
                g_queue.pop_front();
            }
            p->reply = execute(p->request, swapchain);
            SetEvent(p->done);  // our shared_ptr keeps p alive even if the listener timed out
        }
    }
}
