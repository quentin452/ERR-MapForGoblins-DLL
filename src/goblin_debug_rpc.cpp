#include "goblin_debug_rpc.hpp"

#include "goblin_config.hpp"
#include "goblin_overlay.hpp"
#include "goblin_overlay_render_loader.hpp"

#include <spdlog/spdlog.h>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
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
                       std::to_string(goblin::overlay_render_loader::reload_pending() ? 1 : 0);
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

                auto p = std::make_shared<Pending>();
                p->request = line;
                p->done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
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
                send(c, reply.c_str(), static_cast<int>(reply.size()), 0);
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
