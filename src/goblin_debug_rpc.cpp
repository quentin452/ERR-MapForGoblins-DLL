#include "goblin_debug_rpc.hpp"

#include "goblin_config.hpp"
#include "input/input_shared.hpp"
#include "input/input_wndproc.hpp"  // wm_keydown_total — RPC key-delivery verify
#include "goblin_inject.hpp"   // world_map_open() — status field for the driver's boot/nav loop
#include "input/input_cursor.hpp"  // set_cursor_pos_real — pixel-exact mouse_move via the trampoline
#include "goblin_pause.hpp"    // pause command + paused= status (unfocused-window pause escape)
#include "goblin_overlay.hpp"
#include "goblin_worldmap_probe.hpp"  // dump_menu_state (dumpmenu cmd)
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

        // Command-feed history for the on-screen HUD (recent_commands below). Listener thread
        // writes, present thread reads — tiny, mutexed.
        struct HistEntry { std::string line; ULONGLONG tick; };
        std::mutex g_hist_mutex;
        std::deque<HistEntry> g_hist;
        constexpr ULONGLONG kHistMaxAgeMs = 6000;
        constexpr size_t kHistMax = 8;

        // RPC auto-idle: the user counts as "active" (and RPC input injection is suspended) while
        // real kb/mouse activity is younger than this. Hardcoded calibration, not a preference —
        // the on/off switch is the `rpc_auto_idle` ini key. ~1.5s covers the gap between keystrokes
        // without holding the RPC off for long after the human stops.
        constexpr unsigned long long kUserIdleWindowMs = 1500;
        // How long each SendInput's WM echo is discounted from user-activity (see mark_rpc_injection).
        constexpr unsigned kInjectionGuardMs = 300;

        void note_command(const std::string &request, const std::string &reply)
        {
            std::string line = request + "  ->  " + (reply.size() > 48 ? reply.substr(0, 45) + "..." : reply);
            std::lock_guard<std::mutex> lk(g_hist_mutex);
            g_hist.push_back({std::move(line), GetTickCount64()});
            while (g_hist.size() > kHistMax) g_hist.pop_front();
        }

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
        // key-hold Sleep() on the present thread would hitch frames. Scancodes included for
        // non-char keys (raw-input readers want them); mouse moves go through the SetCursorPos
        // TRAMPOLINE (see move_cursor_client) — pixel-exact and doesn't feed our swallow hook.

        bool ensure_game_foreground()
        {
            HWND hwnd = static_cast<HWND>(goblin::overlay::game_hwnd());
            if (!hwnd) return false;
            if (GetForegroundWindow() == hwnd && goblin::input::has_focus()) return true;
            SetForegroundWindow(hwnd);
            // X11/Wine focus is ASYNC: firing SendInput right after SetForegroundWindow loses
            // the FIRST command — validated live 2026-07-02 (xterm steals focus -> `key M`
            // returns ok but the map never opens; the log shows WM_SETFOCUS landing DURING the
            // send; the immediate retry works). Wait until BOTH the OS reports us foreground
            // AND our own WM_SETFOCUS-driven gate (g_has_focus) has processed — that second
            // condition is what actually opens the input paths — then a short settle frame.
            for (int i = 0; i < 60; ++i) // <= ~1.2s
            {
                if (GetForegroundWindow() == hwnd && goblin::input::has_focus())
                {
                    Sleep(40); // one-two frames of settle so the game's own focus handling runs
                    return true;
                }
                Sleep(20);
            }
            // Focus never confirmed (the known Wine "can't steal X focus back" case) — proceed
            // best-effort; the caller's command may still land if focus arrives late.
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
            goblin::input::mark_rpc_injection(kInjectionGuardMs);
            SendInput(1, &in, sizeof(in));
        }

        // Pixel-exact cursor placement via the SetCursorPos TRAMPOLINE (set_cursor_pos_real —
        // bypasses our own swallow hook; the game's map cursor tracks the OS cursor, so this is
        // all a hover needs). SendInput MOUSEEVENTF_ABSOLUTE was tried first and LANDED OFF
        // TARGET under Wine (sent client 476,536 → arrived 552,371 — the absolute→X11 mapping
        // doesn't match the virtual-desktop metrics we computed against).
        bool move_cursor_client(int cx, int cy)
        {
            HWND hwnd = static_cast<HWND>(goblin::overlay::game_hwnd());
            if (!hwnd) return false;
            POINT p{cx, cy};
            ClientToScreen(hwnd, &p);
            goblin::input::mark_rpc_injection(kInjectionGuardMs);  // SetCursorPos + jiggle = our echo
            if (!goblin::input::set_cursor_pos_real(p.x, p.y)) return false;
            // SetCursorPos generates no input EVENT, so with the world map open the game keeps
            // re-warping the OS cursor back onto its own (raw-input-driven) reticle — the
            // placement held for at most a frame (observed live: sent 483,540, read back
            // 814,901). A ±1px relative jiggle is a REAL mouse event: the game switches to
            // mouse-cursor mode and adopts the OS cursor position we just set.
            INPUT in[2]{};
            in[0].type = INPUT_MOUSE;
            in[0].mi.dx = 1;
            in[0].mi.dwFlags = MOUSEEVENTF_MOVE;
            in[1].type = INPUT_MOUSE;
            in[1].mi.dx = -1;
            in[1].mi.dwFlags = MOUSEEVENTF_MOVE;
            SendInput(2, in, sizeof(INPUT));
            return true;
        }

        // key <name> [hold_ms] | mouse_move <cx> <cy> | mouse_click [left|right] [<cx> <cy>]
        // Coordinates are CLIENT pixels of the game window (same space as screenshots).
        std::string execute_input(const std::string &cmd, std::string rest)
        {
            // Auto-idle: if the human is actively driving kb/mouse, don't inject — scripted and
            // manual input must not fight over the OS cursor/keystrokes. No-op with a clear reply
            // (BEFORE ensure_game_foreground so we don't even steal focus). Non-input RPC bypasses
            // this entirely (it never reaches execute_input). Off via ini rpc_auto_idle=false.
            if (goblin::config::rpcAutoIdle &&
                goblin::input::ms_since_user_input() < kUserIdleWindowMs)
                return "idle user active (rpc input suspended; poll status rpc_input_idle=)";
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
                // Closed-loop delivery verify (first-command-after-refocus loss, 2026-07-02):
                // a key can be silently eaten when X focus is mid-transition even though Wine
                // reports us foreground (no WM_KILLFOCUS ever fires — invisible from in here).
                // The game's wndproc DOES see every delivered injected key (WM_KEYDOWN,
                // validated live via the kbseen counter), so poll it: no arrival within ~240ms
                // → re-assert foreground and resend once.
                const unsigned kb_before = goblin::input::wm_keydown_total();
                send_vk(static_cast<uint16_t>(vk), false);
                bool retried = false;
                for (int i = 0; i < 12 && goblin::input::wm_keydown_total() == kb_before; ++i)
                    Sleep(20);
                if (goblin::input::wm_keydown_total() == kb_before)
                {
                    if (HWND hw = static_cast<HWND>(goblin::overlay::game_hwnd()))
                        SetForegroundWindow(hw);
                    Sleep(150);
                    send_vk(static_cast<uint16_t>(vk), false);
                    retried = true;
                }
                Sleep(static_cast<DWORD>(hold_ms));
                send_vk(static_cast<uint16_t>(vk), true);
                return retried ? "ok key " + name + " (retried after lost first send)"
                               : "ok key " + name;
            }
            if (cmd == "mouse_move")
            {
                std::string xs = next_token(rest), ys = next_token(rest);
                int cx = 0, cy = 0;
                try { cx = std::stoi(xs); cy = std::stoi(ys); }
                catch (...) { return "err usage: mouse_move <client_x> <client_y>"; }
                if (!move_cursor_client(cx, cy)) return "err cursor placement failed";
                return "ok mouse_move";
            }
            if (cmd == "mouse_drag")
            {
                // mouse_drag <x0> <y0> <x1> <y1> — left-button drag in client px (the UI
                // exclusion-zone editor's create gesture; also map panning).
                std::string a=next_token(rest),b=next_token(rest),c=next_token(rest),d=next_token(rest);
                int x0=0,y0=0,x1=0,y1=0;
                try { x0=std::stoi(a); y0=std::stoi(b); x1=std::stoi(c); y1=std::stoi(d); }
                catch (...) { return "err usage: mouse_drag <x0> <y0> <x1> <y1>"; }
                if (!move_cursor_client(x0, y0)) return "err cursor placement failed";
                Sleep(80);
                INPUT in{};
                in.type = INPUT_MOUSE;
                in.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                goblin::input::mark_rpc_injection(kInjectionGuardMs);
                SendInput(1, &in, sizeof(in));
                Sleep(80);
                for (int i = 1; i <= 8; ++i)
                {
                    move_cursor_client(x0 + (x1 - x0) * i / 8, y0 + (y1 - y0) * i / 8);
                    Sleep(30);
                }
                Sleep(80);
                in.mi.dwFlags = MOUSEEVENTF_LEFTUP;
                goblin::input::mark_rpc_injection(kInjectionGuardMs);
                SendInput(1, &in, sizeof(in));
                return "ok mouse_drag";
            }
            if (cmd == "type")
            {
                // Type literal TEXT into a focused (ImGui) field. `key <letter>` is the wrong tool
                // for text on a non-QWERTY layout: Wine converts a VK-only SendInput to a scancode
                // via the US layout, then the game-side layout translates it back — so under
                // AZERTY, sending VK_A lands as 'q' (typed "larval", got "lqrvql", live
                // 2026-07-02). Inversion: char → VK in the GAME's layout (VkKeyScanW) → that
                // key's PHYSICAL scancode → the VK sitting at that position in QWERTY (static
                // scancode→US-VK table) → send THAT; Wine's US mapping then lands on the right
                // physical key and the game layout produces the wanted char. Layout-agnostic.
                size_t b = rest.find_first_not_of(" \t");
                std::string text = b == std::string::npos ? std::string{} : rest.substr(b);
                if (text.empty()) return "err usage: type <text>";
                static const uint16_t kScanToUsVk[0x40] = {
                    // 0x00-0x0F: esc 1..0 - = bksp tab
                    0, VK_ESCAPE, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
                    VK_OEM_MINUS, VK_OEM_PLUS, VK_BACK, VK_TAB,
                    // 0x10-0x1F: qwertyuiop [ ] enter ctrl a s
                    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P',
                    VK_OEM_4, VK_OEM_6, VK_RETURN, VK_CONTROL, 'A', 'S',
                    // 0x20-0x2F: d f g h j k l ; ' ` lshift \ z x c v
                    'D', 'F', 'G', 'H', 'J', 'K', 'L', VK_OEM_1, VK_OEM_7, VK_OEM_3,
                    VK_SHIFT, VK_OEM_5, 'Z', 'X', 'C', 'V',
                    // 0x30-0x39: b n m , . / rshift * alt space
                    'B', 'N', 'M', VK_OEM_COMMA, VK_OEM_PERIOD, VK_OEM_2,
                    VK_SHIFT, VK_MULTIPLY, VK_MENU, VK_SPACE, 0, 0, 0, 0, 0, 0};
                int typed = 0;
                for (char ch : text)
                {
                    const SHORT vks = VkKeyScanW(static_cast<WCHAR>(ch));
                    if (vks == -1) continue;  // no key produces this char in the game's layout
                    const UINT vk_game = vks & 0xFF;
                    const bool shift = (vks & 0x100) != 0;
                    const UINT scan = MapVirtualKeyW(vk_game, MAPVK_VK_TO_VSC);
                    if (scan >= 0x40 || !kScanToUsVk[scan]) continue;
                    const uint16_t vk_send = kScanToUsVk[scan];
                    if (shift) send_vk(VK_SHIFT, false);
                    send_vk(vk_send, false);
                    Sleep(60);  // the poll samples per frame — 35ms dropped/doubled chars live
                    send_vk(vk_send, true);
                    if (shift) send_vk(VK_SHIFT, true);
                    Sleep(60);
                    ++typed;
                }
                return "ok typed " + std::to_string(typed) + "/" + std::to_string(text.size()) + " chars";
            }
            if (cmd == "mouse_wheel")
            {
                std::string ds = next_token(rest);
                int notches = 0;
                try { notches = std::stoi(ds); } catch (...) { return "err usage: mouse_wheel <notches> (±)"; }
                if (notches < -20 || notches > 20 || notches == 0) return "err notches out of range (±1..20)";
                INPUT in{};
                in.type = INPUT_MOUSE;
                in.mi.dwFlags = MOUSEEVENTF_WHEEL;
                const int step = notches > 0 ? 1 : -1;
                for (int k = 0; k != notches; k += step)
                {
                    in.mi.mouseData = static_cast<DWORD>(step * WHEEL_DELTA);
                    goblin::input::mark_rpc_injection(kInjectionGuardMs);
                    SendInput(1, &in, sizeof(in));
                    Sleep(30);  // one notch per game frame-ish; a single big delta gets clamped
                }
                return "ok mouse_wheel";
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
                    if (!move_cursor_client(cx, cy)) return "err cursor placement failed";
                    Sleep(30);
                }
                INPUT in{};
                in.type = INPUT_MOUSE;
                in.mi.dwFlags = right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
                goblin::input::mark_rpc_injection(kInjectionGuardMs);
                SendInput(1, &in, sizeof(in));
                Sleep(40);
                in.mi.dwFlags = right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
                goblin::input::mark_rpc_injection(kInjectionGuardMs);
                SendInput(1, &in, sizeof(in));
                return "ok mouse_click";
            }
            return "err not an input command";  // unreachable via dispatch below
        }

        bool is_input_command(const std::string &cmd)
        {
            return cmd == "key" || cmd == "type" || cmd == "mouse_move" || cmd == "mouse_click" ||
                   cmd == "mouse_drag" ||
                   cmd == "mouse_wheel";
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
                       " menucover=" +
                       std::to_string(goblin::worldmap_probe::menu_covers_map() ? 1 : 0) +
                       " paused=" + (goblin::pause::available()
                                         ? std::to_string(goblin::pause::paused() ? 1 : 0)
                                         : std::string("na")) +
                       " kbseen=" + std::to_string(goblin::input::wm_keydown_total()) +
                       " fg=" + std::to_string(goblin::input::has_focus() ? 1 : 0) +
                       [] {
                           const unsigned long long idle = goblin::input::ms_since_user_input();
                           const bool suspended =
                               goblin::config::rpcAutoIdle && idle < kUserIdleWindowMs;
                           // Cap the reported age so a fresh session (no input yet = ~0ull) prints
                           // a sane number, not 18446744073709551615.
                           return " user_idle_ms=" +
                                  std::to_string(idle > 99999ull ? 99999ull : idle) +
                                  " rpc_input_idle=" + std::to_string(suspended ? 1 : 0);
                       }();
            }
            if (cmd == "dumpmenu")
            {
                std::string tag = next_token(rest);
                goblin::worldmap_probe::dump_menu_state(tag.empty() ? "x" : tag.c_str());
                return "ok dumpmenu -> wmprobe log";
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
                        std::string reply = execute_input(cmd, peek);
                        note_command(line, reply);
                        reply += "\n";
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
                note_command(line, reply);
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

    std::vector<std::string> recent_commands()
    {
        std::vector<std::string> out;
        const ULONGLONG now = GetTickCount64();
        std::lock_guard<std::mutex> lk(g_hist_mutex);
        while (!g_hist.empty() && now - g_hist.front().tick > kHistMaxAgeMs)
            g_hist.pop_front();
        out.reserve(g_hist.size());
        for (const auto &h : g_hist) out.push_back(h.line);
        return out;
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
