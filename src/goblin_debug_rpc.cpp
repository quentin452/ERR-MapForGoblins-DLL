#include "goblin_debug_rpc.hpp"

#include "goblin_config.hpp"
#include "input/input_shared.hpp"
#include "input/input_wndproc.hpp"  // wm_keydown_total — RPC key-delivery verify
#include "goblin_inject.hpp"   // world_map_open() — status field for the driver's boot/nav loop
#include "goblin_overlay_render_api.hpp"  // overlay_api::rebuild_markers (refresh_markers cmd)
#include "input/input_cursor.hpp"  // set_cursor_pos_real — pixel-exact mouse_move via the trampoline
#include "goblin_pause.hpp"    // pause command + paused= status (unfocused-window pause escape)
#include "goblin_overlay.hpp"
#include "goblin_worldmap_probe.hpp"  // dump_menu_state (dumpmenu cmd)
#include "goblin_overlay_render_loader.hpp"
#include "goblin_param_edit.hpp"  // param_get/param_set commands — Slice 1 in-game smoke test
#include "goblin_messages.hpp"  // inject_fmg_entries / raw_message_utf8 — fmg_set cmd (Gap D)
#include "goblin_debug_events.hpp"  // last_inventory_accessor — inv_probe cmd (Gap C grant RE)
#include "goblin_sidecar.hpp"  // sidecar cmd — Phase 1 state store drive/verify
#include "goblin_inventory.hpp"  // give_item cmd — Gap C grant / Phase-2 strip RE
#include "goblin_warp.hpp"  // warp cmd — grace fast-travel (dev-world nav)
#include "goblin_world_editor.hpp"  // we_scan cmd — World Editor picker enumeration
#include "goblin_world_bundle.hpp"  // bundle cmd — World Editor save/apply persistence
#include "goblin_geom_move.hpp"     // move_asset cmd — live geom transform-setter test (MSB-write RE)
#include "goblin_field_probe.hpp"  // arm_raw — serialize find-what-accesses (Phase 2)

#include <spdlog/spdlog.h>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
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

        // Parse a param-field type token (u8/s8/u16/s16/u32/s32/f32/f64/u64/s64) for the
        // param_get/param_set RPC commands. false = unknown token.
        bool parse_field_type(const std::string &t, goblin::paramedit::FieldType &out)
        {
            using FT = goblin::paramedit::FieldType;
            if (t == "u8")  { out = FT::U8;  return true; }
            if (t == "s8")  { out = FT::S8;  return true; }
            if (t == "u16") { out = FT::U16; return true; }
            if (t == "s16") { out = FT::S16; return true; }
            if (t == "u32") { out = FT::U32; return true; }
            if (t == "s32") { out = FT::S32; return true; }
            if (t == "f32") { out = FT::F32; return true; }
            if (t == "f64") { out = FT::F64; return true; }
            if (t == "u64") { out = FT::U64; return true; }
            if (t == "s64") { out = FT::S64; return true; }
            return false;
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
            // help — one-line verb list (the client reads a single reply line, so no embedded \n).
            // Full usages + caveats: docs/memory/tooling/rpc-commands.md. Keep in sync when adding a cmd.
            if (cmd == "help" || cmd == "?")
                return "ok commands: help ping status open_f1 pause set screenshot dumpmenu reload_overlay"
                       " | param_get param_set param_getf param_setf param_clone"
                       " | loot_at refresh_markers warp we_scan"
                       " | give_item goods_count strip_test inv_probe fmg_set sidecar bundle"
                       " | mem_dump mem_fwa equip_dump equip_fwa move_asset move_hold move_read move_near move_restore move_all move_aeg geom_stats geom_dump spawn_probe spawn_clone"
                       " | key type mouse_move mouse_click mouse_drag mouse_wheel"
                       "  (usage+caveats: docs/memory/tooling/rpc-commands.md)";
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
            // param_get <ParamName> <rowId> <offset(0x..)> <type> — read a live param field.
            // param_set <ParamName> <rowId> <offset(0x..)> <type> <value> — write it, return read-back.
            // Slice 1 in-game smoke test for goblin::paramedit (docs/plans/param_override_loader_plan.md).
            // Offset-addressed on purpose (a DEV command); a shipped override FILE will be name-addressed.
            if (cmd == "param_get" || cmd == "param_set")
            {
                const bool is_set = cmd == "param_set";
                std::string pname = next_token(rest), row_s = next_token(rest),
                            off_s = next_token(rest), type_s = next_token(rest),
                            val_s = is_set ? next_token(rest) : std::string{};
                if (pname.empty() || row_s.empty() || off_s.empty() || type_s.empty() ||
                    (is_set && val_s.empty()))
                    return std::string("err usage: ") + cmd +
                           " <ParamName> <rowId> <offset(0x..)> <type>" +
                           (is_set ? " <value>" : "") + " (type=u8|s8|u16|s16|u32|s32|f32|f64|u64|s64)";

                goblin::paramedit::FieldType ft;
                if (!parse_field_type(type_s, ft)) return "err bad type: " + type_s;

                uint64_t row_id = 0;
                ptrdiff_t offset = 0;
                double value = 0;
                try
                {
                    row_id = std::stoull(row_s, nullptr, 0);
                    offset = static_cast<ptrdiff_t>(std::stoll(off_s, nullptr, 0));  // 0x.. auto-detected
                    if (is_set) value = std::stod(val_s);
                }
                catch (...)
                {
                    return "err bad number (row/offset/value)";
                }

                std::wstring wname(pname.begin(), pname.end());  // ASCII param names
                if (is_set)
                {
                    if (!goblin::paramedit::param_set_field(wname.c_str(), row_id, offset, ft, value))
                        return "err write failed (param/row missing, offset OOR, or fault)";
                }
                auto rb = goblin::paramedit::param_get_field(wname.c_str(), row_id, offset, ft);
                if (!rb) return "err read failed (param/row missing or offset OOR)";
                return "ok " + pname + "[" + row_s + "]+" + off_s + " " + type_s + "=" +
                       std::to_string(*rb);
            }
            // param_getf <ParamName> <rowId> <fieldName> — read by field NAME (offset resolved from
            // the live exe via the registry). param_setf <ParamName> <rowId> <fieldName> <value>.
            // Slice 2: name-addressed. Unknown field → err (see goblin::paramedit registry).
            if (cmd == "param_getf" || cmd == "param_setf")
            {
                const bool is_set = cmd == "param_setf";
                std::string pname = next_token(rest), row_s = next_token(rest),
                            field = next_token(rest),
                            val_s = is_set ? next_token(rest) : std::string{};
                if (pname.empty() || row_s.empty() || field.empty() || (is_set && val_s.empty()))
                    return std::string("err usage: ") + cmd + " <ParamName> <rowId> <fieldName>" +
                           (is_set ? " <value>" : "");
                uint64_t row_id = 0;
                double value = 0;
                try
                {
                    row_id = std::stoull(row_s, nullptr, 0);
                    if (is_set) value = std::stod(val_s);
                }
                catch (...)
                {
                    return "err bad number (row/value)";
                }
                std::wstring wname(pname.begin(), pname.end());
                if (!goblin::paramedit::field_is_known(wname.c_str(), field.c_str()))
                    return "err unknown field (not in registry): " + pname + "." + field;
                if (is_set &&
                    !goblin::paramedit::param_set_field_by_name(wname.c_str(), row_id, field.c_str(), value))
                    return "err write failed (row missing, offset OOR, or fault)";
                auto rb = goblin::paramedit::param_get_field_by_name(wname.c_str(), row_id, field.c_str());
                if (!rb) return "err read failed (row missing)";
                return "ok " + pname + "[" + row_s + "]." + field + "=" + std::to_string(*rb);
            }
            // param_clone <ParamName> <srcRowId> <newRowId> — add a row by cloning an existing one
            // (Gap B: param_add_rows table-expand). Read-back proves the new row is findable.
            if (cmd == "param_clone")
            {
                std::string pname = next_token(rest), src_s = next_token(rest), new_s = next_token(rest);
                if (pname.empty() || src_s.empty() || new_s.empty())
                    return "err usage: param_clone <ParamName> <srcRowId> <newRowId>";
                uint64_t src = 0;
                int32_t nid = 0;
                try
                {
                    src = std::stoull(src_s, nullptr, 0);
                    nid = static_cast<int32_t>(std::stol(new_s, nullptr, 0));
                }
                catch (...)
                {
                    return "err bad id";
                }
                std::wstring wname(pname.begin(), pname.end());
                if (!goblin::paramedit::param_clone_row(wname.c_str(), src, nid))
                    return "err clone failed (src missing / id collision / alloc)";
                auto rb = goblin::paramedit::param_get_field(wname.c_str(), (uint64_t)nid, 0,
                                                             goblin::paramedit::FieldType::S32);
                return "ok cloned " + pname + "[" + src_s + "] -> [" + new_s +
                       "] new_row_present=" + (rb ? "1" : "0");
            }
            // fmg_set <slot> <id> <text...> — inject/override an FMG string (Gap D). Slot = the BASE
            // FMG slot: GoodsName=10, WeaponName=11, PlaceName=19 (the low tier the item-name path
            // falls through to). Do NOT use the DLC/menu tiers (GoodsName 419/319, WeaponName 410/310)
            // — their group id-spans hang patch_fmg_in_memory (now guarded → fast error, not a freeze).
            // Read-back via the reply (raw_message_utf8).
            if (cmd == "fmg_set")
            {
                std::string slot_s = next_token(rest), id_s = next_token(rest);
                size_t b = rest.find_first_not_of(" \t");
                std::string text = b == std::string::npos ? std::string{} : rest.substr(b);
                if (slot_s.empty() || id_s.empty() || text.empty())
                    return "err usage: fmg_set <slot> <id> <text>";
                uint32_t slot = 0;
                int32_t id = 0;
                try
                {
                    slot = static_cast<uint32_t>(std::stoul(slot_s, nullptr, 0));
                    id = static_cast<int32_t>(std::stol(id_s, nullptr, 0));
                }
                catch (...)
                {
                    return "err bad slot/id";
                }
                std::wstring wtext(text.begin(), text.end());  // ASCII widen (dev test path)
                if (!goblin::inject_fmg_entries(slot, {{id, wtext}}))
                    return "err inject failed (repo not ready / slot invalid)";
                return "ok " + slot_s + ":" + id_s + "=" +
                       goblin::raw_message_utf8(slot, static_cast<uint32_t>(id));
            }
            // inv_probe — report the captured inventory accessor (AddItemFunc `inv`) + the
            // live player chain, for the Gap C grant / sidecar inventory-accessor RE. The
            // accessor populates after the game grants ANY item this session (pick something
            // up / a rune / a reward); [INVACCESS] in the events log carries the offset scan.
            if (cmd == "inv_probe")
            {
                void *inv = goblin::debug_events::last_inventory_accessor();
                void *lp = goblin::get_local_player_ptr();
                void *wcm = goblin::get_world_chr_man_ptr();
                char b[192];
                long long dlp = (inv && lp) ? (long long)((uintptr_t)inv - (uintptr_t)lp) : 0;
                long long dwcm = (inv && wcm) ? (long long)((uintptr_t)inv - (uintptr_t)wcm) : 0;
                std::snprintf(b, sizeof(b),
                              "ok inv=%p LocalPlayer=%p WCM=%p inv-lp=0x%llx inv-wcm=0x%llx%s",
                              inv, lp, wcm, dlp, dwcm,
                              inv ? "" : " (no grant seen yet — pick up an item)");
                return std::string(b);
            }
            // sidecar <sub> — drive/verify the Phase 1 sidecar state store. Subs:
            //   status                — path/loaded/flags/kv/dirty
            //   setkv <key> <value..> — set a persisted kv
            //   getkv <key>           — read it
            //   addflag <id> / rmflag <id> / flags — custom-flag set ops
            //   save / load           — force persist / reload of <save>.mfg
            if (cmd == "sidecar")
            {
                std::string sub = next_token(rest);
                if (sub == "status" || sub.empty())
                    return "ok " + goblin::sidecar::status_line();
                if (sub == "setkv")
                {
                    std::string k = next_token(rest);
                    size_t b = rest.find_first_not_of(" \t");
                    std::string v = b == std::string::npos ? std::string{} : rest.substr(b);
                    if (k.empty()) return "err usage: sidecar setkv <key> <value>";
                    goblin::sidecar::set_kv(k, v);
                    return "ok set " + k + "=" + v;
                }
                if (sub == "getkv")
                {
                    std::string k = next_token(rest);
                    if (k.empty()) return "err usage: sidecar getkv <key>";
                    return "ok " + k + "=" + goblin::sidecar::get_kv(k);
                }
                if (sub == "addflag" || sub == "rmflag")
                {
                    std::string id_s = next_token(rest);
                    uint32_t id = 0;
                    try { id = (uint32_t)std::stoul(id_s, nullptr, 0); }
                    catch (...) { return "err bad flag id"; }
                    if (sub == "addflag") goblin::sidecar::add_custom_flag(id);
                    else goblin::sidecar::remove_custom_flag(id);
                    return "ok " + sub + " " + std::to_string(id);
                }
                if (sub == "flags")
                {
                    std::string out = "ok flags:";
                    for (uint32_t f : goblin::sidecar::custom_flags())
                        out += " " + std::to_string(f);
                    return out;
                }
                if (sub == "additem")
                {
                    std::string id_s = next_token(rest), qty_s = next_token(rest);
                    if (id_s.empty()) return "err usage: sidecar additem <id(0x..)> <qty>";
                    uint32_t id = 0; int32_t qty = 1;
                    try {
                        id = (uint32_t)std::stoul(id_s, nullptr, 0);
                        if (!qty_s.empty()) qty = (int32_t)std::stol(qty_s, nullptr, 0);
                    } catch (...) { return "err bad id/qty"; }
                    goblin::sidecar::add_custom_item(id, qty);
                    return "ok additem " + id_s + " x" + std::to_string(qty);
                }
                if (sub == "items")
                {
                    std::string out = "ok items:";
                    for (auto &[id, qty] : goblin::sidecar::custom_items())
                    {
                        char b[32]; std::snprintf(b, sizeof(b), " %#010x=%d", id, qty);
                        out += b;
                    }
                    return out;
                }
                if (sub == "serclear")
                {
                    goblin::sidecar::reset_serialize_probe();
                    return "ok serialize probe reset";
                }
                if (sub == "save")
                    return goblin::sidecar::save() ? "ok saved " + goblin::sidecar::sidecar_path_utf8()
                                                   : "err save failed (no save file seen yet?)";
                if (sub == "load")
                    return goblin::sidecar::load() ? "ok " + goblin::sidecar::status_line()
                                                   : "err load failed (no save file seen yet?)";
                return "err usage: sidecar status|setkv|getkv|addflag|rmflag|flags|save|load";
            }
            // give_item <id> <qty> — call AddItemFunc to GRANT (qty>0) or REMOVE (qty<0) an
            // item (Gap C grant / sidecar Phase-2 strip RE). id is category-encoded
            // (goods = 0x40000000|goodsId). Mutates the live inventory — dev-only, SEH-guarded.
            // inv_probe reports the resolved accessor; grep [INVGRANT] for the call result.
            if (cmd == "give_item")
            {
                std::string id_s = next_token(rest), qty_s = next_token(rest);
                if (id_s.empty()) return "err usage: give_item <id(0x..)> <qty(+grant/-remove)>";
                uint32_t id = 0;
                int32_t qty = 1;
                try
                {
                    id = static_cast<uint32_t>(std::stoul(id_s, nullptr, 0));
                    if (!qty_s.empty()) qty = static_cast<int32_t>(std::stol(qty_s, nullptr, 0));
                }
                catch (...) { return "err bad id/qty"; }
                bool ok = goblin::inventory::give_item(id, qty);
                char b[96];
                std::snprintf(b, sizeof(b), "%s give_item id=%#010x qty=%d", ok ? "ok" : "err", id, qty);
                return std::string(b);
            }
            // goods_count <id> — how many of the category-encoded `id` the player HOLDS (carried
            // inventory), read-only. The sidecar clean-save oracle (grant→save→empty .mfg→reload→
            // assert 0). Reports not-in-world separately from a real 0 (chain unresolved).
            if (cmd == "goods_count")
            {
                std::string id_s = next_token(rest);
                if (id_s.empty()) return "err usage: goods_count <id(0x..)>";
                uint32_t id = 0;
                try { id = static_cast<uint32_t>(std::stoul(id_s, nullptr, 0)); }
                catch (...) { return "err bad id"; }
                if (!goblin::inventory::equip_game_data()) return "err not in-world (inventory unresolved)";
                uint32_t n = goblin::inventory::goods_count(id);
                char b[80];
                std::snprintf(b, sizeof(b), "ok goods_count id=%#010x n=%u", id, n);
                return std::string(b);
            }
            // strip_test <id> — validate the Phase-2 strip primitive WITHOUT a save (no dirtying):
            // read held count, strip_goods([id]) (zero the node), re-read (expect 0), restore, re-read
            // (expect the original). Proves inventory::strip_goods/restore_goods round-trips the live
            // inventory the serialize reads.
            if (cmd == "strip_test")
            {
                std::string id_s = next_token(rest);
                if (id_s.empty()) return "err usage: strip_test <id(0x..)>";
                uint32_t id = 0;
                try { id = static_cast<uint32_t>(std::stoul(id_s, nullptr, 0)); }
                catch (...) { return "err bad id"; }
                if (!goblin::inventory::equip_game_data()) return "err not in-world (inventory unresolved)";
                uint32_t before = goblin::inventory::goods_count(id);
                auto snap = goblin::inventory::strip_goods({id});
                uint32_t during = goblin::inventory::goods_count(id);
                goblin::inventory::restore_goods(snap);
                uint32_t after = goblin::inventory::goods_count(id);
                char b[128];
                std::snprintf(b, sizeof(b),
                              "ok strip_test id=%#010x before=%u during=%u after=%u nodes=%zu",
                              id, before, during, after, snap.size());
                return std::string(b);
            }
            // refresh_markers — force a fresh marker/bucket build so a LIVE param edit (a
            // pickUpItemLotParamId repoint, a lot's lotItemId01, any param override) shows on the drawn
            // map without a game reload. Disk source only (default). Re-reads live params on the build
            // worker (async) — poll the log's [BENCH] build line / the map to see it land. NB a NEWLY
            // CLONED lot still won't resolve until the LotReader index is rebuildable (see HANDOFF).
            if (cmd == "refresh_markers")
            {
                goblin::overlay_api::rebuild_markers();
                return "ok refresh_markers triggered (rebuild runs on the disk worker; poll the map)";
            }
            // warp <graceId> — fast-travel to a site of grace (dev-world nav). graceId = the
            // bonfire entity id (e.g. 1042362951 = The First Step, 10002951 = Margit). Must be
            // in-world + the grace unlocked. grep [WARP] for the call result.
            if (cmd == "warp")
            {
                std::string id_s = next_token(rest);
                if (id_s.empty()) return "err usage: warp <graceId> (e.g. 1042362951 = The First Step)";
                int32_t gid = 0;
                try { gid = static_cast<int32_t>(std::stol(id_s, nullptr, 0)); }
                catch (...) { return "err bad graceId"; }
                bool ok = goblin::warp::to_grace(gid);
                return ok ? "ok warp " + std::to_string(gid)
                          : "err warp failed (unresolved / not in-world / grace locked)";
            }
            // mem_dump <hexaddr> <len> — raw RPM hex-dump of an absolute address (follow pointers /
            // diff to locate the goods inventory). len capped at 256.
            if (cmd == "mem_dump")
            {
                std::string a_s = next_token(rest), len_s = next_token(rest);
                uint64_t addr = 0; uint32_t len = 64;
                try { addr = std::stoull(a_s, nullptr, 0);
                      if (!len_s.empty()) len = (uint32_t)std::stoul(len_s, nullptr, 0); }
                catch (...) { return "err bad addr/len"; }
                if (len > 256) len = 256;
                unsigned char buf[256]; SIZE_T got = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), (void *)addr, buf, len, &got) || got != len)
                    return "err read failed";
                char out[800];
                int p = std::snprintf(out, sizeof(out), "ok %#llx:", (unsigned long long)addr);
                for (uint32_t i = 0; i < len && p < (int)sizeof(out) - 4; i++)
                    p += std::snprintf(out + p, sizeof(out) - p, " %02x", buf[i]);
                return std::string(out);
            }
            // mem_fwa <hexaddr> <len> [r|w] — arm a HW find-what-accesses breakpoint on an ABSOLUTE
            // address (e.g. PlayerGameData+0x9c = the char name, a COLD serialized field). Trigger a
            // save (warp) → [FWA] logs the serialize read RIP. Cold target avoids the VEH storm that a
            // per-frame-hot byte (equipped id) causes.
            if (cmd == "mem_fwa")
            {
                std::string a_s = next_token(rest), len_s = next_token(rest), rw = next_token(rest);
                uint64_t addr = 0; uint32_t len = 2;
                try { addr = std::stoull(a_s, nullptr, 0);
                      if (!len_s.empty()) len = (uint32_t)std::stoul(len_s, nullptr, 0); }
                catch (...) { return "err bad addr/len"; }
                bool wo = (rw == "w");
                bool ok = goblin::field_probe::arm_raw((uintptr_t)addr, (int)len, wo, "serialize");
                char b[96];
                std::snprintf(b, sizeof(b), "%s armed FWA @ %#llx len=%u %s", ok ? "ok" : "err",
                              (unsigned long long)addr, len, wo ? "w" : "r");
                return std::string(b);
            }
            // equip_dump <off(0x..)> <len> — hex-dump EquipGameData+off (find the inventory layout
            // for the Phase-2 serialize bracket RE). len capped at 256.
            if (cmd == "equip_dump")
            {
                std::string off_s = next_token(rest), len_s = next_token(rest);
                uint32_t off = 0, len = 64;
                try { off = (uint32_t)std::stoul(off_s, nullptr, 0);
                      if (!len_s.empty()) len = (uint32_t)std::stoul(len_s, nullptr, 0); }
                catch (...) { return "err bad off/len"; }
                if (len > 256) len = 256;
                void *egd = goblin::inventory::equip_game_data();
                if (!egd) return "err EquipGameData null (not in-world?)";
                unsigned char buf[256];
                SIZE_T got = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), (unsigned char *)egd + off, buf, len, &got) ||
                    got != len)
                    return "err read failed";
                char out[800];
                int p = std::snprintf(out, sizeof(out), "ok egd=%p +%#x:", egd, off);
                for (uint32_t i = 0; i < len && p < (int)sizeof(out) - 4; i++)
                    p += std::snprintf(out + p, sizeof(out) - p, " %02x", buf[i]);
                return std::string(out);
            }
            // loot_at <aegRow> — resolve LIVE what the map's loot marker for AssetEnvironmentGeometry
            // row `aegRow` would show: pickUpItemLotParamId → ItemLotParam_map → item name (the exact
            // chain map_entry_layer builds a marker from). Lets a pickUpItemLotParamId repoint be
            // verified without a screenshot: read loot_at, param_set the pickup lot, read loot_at again.
            if (cmd == "loot_at")
            {
                std::string a_s = next_token(rest);
                if (a_s.empty()) return "err usage: loot_at <aegRow>";
                uint32_t aeg = 0;
                try { aeg = (uint32_t)std::stoul(a_s, nullptr, 0); }
                catch (...) { return "err bad aegRow"; }
                uint32_t lot = goblin::aeg_pickup_lot(aeg);
                int32_t textid = lot ? goblin::resolve_loot_item_textid(lot, 1, -1) : -1;
                std::string name = (textid >= 0) ? goblin::lookup_text_utf8(textid) : std::string{};
                char b[224];
                std::snprintf(b, sizeof(b), "ok aeg=%u lot=%u item_textid=%d name='%s'",
                              aeg, lot, textid, name.c_str());
                return std::string(b);
            }
            // equip_fwa <off(0x..)> <len> [r|w] — arm a HW find-what-accesses breakpoint on
            // EquipGameData+off, then trigger a save (warp) → [FWA] logs the serialize read RIP.
            if (cmd == "equip_fwa")
            {
                std::string off_s = next_token(rest), len_s = next_token(rest), rw = next_token(rest);
                uint32_t off = 0, len = 1;
                try { off = (uint32_t)std::stoul(off_s, nullptr, 0);
                      if (!len_s.empty()) len = (uint32_t)std::stoul(len_s, nullptr, 0); }
                catch (...) { return "err bad off/len"; }
                void *egd = goblin::inventory::equip_game_data();
                if (!egd) return "err EquipGameData null (not in-world?)";
                uintptr_t addr = reinterpret_cast<uintptr_t>(egd) + off;
                bool write_only = (rw == "w");
                bool ok = goblin::field_probe::arm_raw(addr, (int)len, write_only, "equip-serialize");
                char b[96];
                std::snprintf(b, sizeof(b), "%s armed FWA @ %#llx len=%u %s", ok ? "ok" : "err",
                              (unsigned long long)addr, len, write_only ? "w" : "r");
                return std::string(b);
            }
            // we_scan — build the World Editor picker lists (pickup assets + named goods) from the
            // live params and report the counts. Same scan the F1 "Browse" button runs; present-thread.
            if (cmd == "we_scan")
            {
                int total = goblin::world_editor::scan();
                char b[96];
                std::snprintf(b, sizeof(b), "ok we_scan assets=%zu goods=%zu total=%d",
                              goblin::world_editor::asset_count(),
                              goblin::world_editor::goods_count(), total);
                return std::string(b);
            }
            // bundle <sub> — drive/verify the World Editor world-bundle persistence (slice 7):
            //   status                         path + clones/sets held
            //   clone <param> <src> <new>      record a clone op
            //   set <param> <row> <field> <v>  record a set op (dedup per param/row/field)
            //   save [path]                    write the bundle (default: <mod>/world_bundle.toml)
            //   load <path>                    parse a bundle INTO memory (no apply)
            //   apply [path]                   apply the bundle to live params (default path)
            //   clear                          empty the in-memory bundle
            if (cmd == "bundle")
            {
                namespace wb = goblin::world_bundle;
                std::string sub = next_token(rest);
                if (sub == "status" || sub.empty())
                    return "ok " + wb::status_line() + " path=" + wb::default_path().string();
                if (sub == "clear") { wb::clear(); return "ok cleared"; }
                if (sub == "clone")
                {
                    std::string p = next_token(rest), s = next_token(rest), n = next_token(rest);
                    if (p.empty() || s.empty() || n.empty())
                        return "err usage: bundle clone <param> <srcRow> <newRow>";
                    try {
                        wb::record_clone(p, std::stoull(s, nullptr, 0),
                                         (int32_t)std::stol(n, nullptr, 0));
                    } catch (...) { return "err bad id"; }
                    return "ok " + wb::status_line();
                }
                if (sub == "set")
                {
                    std::string p = next_token(rest), r = next_token(rest), f = next_token(rest),
                                v = next_token(rest);
                    if (p.empty() || r.empty() || f.empty() || v.empty())
                        return "err usage: bundle set <param> <row> <field> <value>";
                    try {
                        wb::record_set(p, std::stoull(r, nullptr, 0), f, std::stod(v));
                    } catch (...) { return "err bad number"; }
                    return "ok " + wb::status_line();
                }
                if (sub == "save")
                {
                    std::string p = next_token(rest);
                    bool ok = p.empty() ? wb::save_default() : wb::save(p);
                    return ok ? "ok saved " + wb::status_line() : "err save failed";
                }
                if (sub == "load")
                {
                    std::string p = next_token(rest);
                    if (p.empty()) return "err usage: bundle load <path>";
                    return wb::load(p) ? "ok " + wb::status_line() : "err load failed";
                }
                if (sub == "apply")
                {
                    std::string p = next_token(rest);
                    int n = p.empty() ? wb::apply_default() : wb::apply(p);
                    return "ok applied " + std::to_string(n) + " ops (" + wb::status_line() + ")";
                }
                return "err usage: bundle status|clone|set|save|load|apply|clear";
            }
            // move_asset <dx> <dy> <dz> — LIVE test of the geom transform SETTER (vtable[0xd0]): pick a
            // live geom instance, move it by the delta via the engine's own virtual setter, read back,
            // then restore. Present-thread (the setter drives physics/render). Proves the MSB-write-free
            // move primitive (docs/re/windows_msb_placement_write_re_findings.md).
            if (cmd == "move_asset")
            {
                std::string xs = next_token(rest), ys = next_token(rest), zs = next_token(rest);
                float dx = 0, dy = 0, dz = 0;
                try { dx = std::stof(xs); dy = std::stof(ys); dz = std::stof(zs); }
                catch (...) { return "err usage: move_asset <dx> <dy> <dz>"; }
                auto r = goblin::geom_move::move_first(dx, dy, dz);
                char b[224];
                if (!r.ok)
                {
                    std::snprintf(b, sizeof(b), "err move_asset: %s", r.err);
                    return std::string(b);
                }
                std::snprintf(b, sizeof(b),
                              "ok move_asset inst=%#llx vt=%#llx before=(%.2f,%.2f,%.2f) "
                              "moved=(%.2f,%.2f,%.2f) restored=(%.2f,%.2f,%.2f)",
                              (unsigned long long)r.inst, (unsigned long long)r.vtable,
                              r.before[0], r.before[1], r.before[2], r.moved[0], r.moved[1], r.moved[2],
                              r.restored[0], r.restored[1], r.restored[2]);
                return std::string(b);
            }
            // move_hold <dx> <dy> <dz> — like move_asset but does NOT restore; remembers the instance.
            // move_read — re-read the held instance's +0x220 translation. Poll it to see if the engine
            // reverts a cache-only write over frames (the move-persistence probe).
            // geom_dump — read-only recon for ADD-new-placement: dump a live geom instance + its
            // CSMsbParts record to the [GEOMDUMP] log (what a cloned record must satisfy for the ctor).
            if (cmd == "geom_dump")
            {
                auto r = goblin::geom_move::geom_dump();
                char b[224];
                std::snprintf(b, sizeof(b), "%s geom_dump %s", r.ok ? "ok" : "err", r.err);
                return std::string(b);
            }
            // spawn_probe — ADD/spawn_clone STAGE 1: read-only recon of every Dynamic-ctor argument
            // (srcType, transform module, parts rec + registry, BlockData +0x288 vector room). No mutation.
            if (cmd == "spawn_probe")
            {
                auto r = goblin::geom_move::spawn_probe();
                char b[224];
                std::snprintf(b, sizeof(b), "%s spawn_probe %s", r.ok ? "ok" : "err", r.err);
                return std::string(b);
            }
            // spawn_clone <dx> <dy> <dz> [go] — ADD a new geom placement (the last MSB-write primitive):
            // clone a live dynamic geom via the engine's Dynamic ctor + a freshly-built pose descriptor,
            // offset by (dx,dy,dz). Without 'go' it BUILDS the descriptor only (no ctor — safe recon).
            if (cmd == "spawn_clone")
            {
                std::string xs = next_token(rest), ys = next_token(rest), zs = next_token(rest);
                std::string g = next_token(rest);
                float dx = 0, dy = 0, dz = 0;
                try { dx = std::stof(xs); dy = std::stof(ys); dz = std::stof(zs); }
                catch (...) { return "err usage: spawn_clone <dx> <dy> <dz> [go]"; }
                bool go = (g == "go" || g == "1");
                auto r = goblin::geom_move::spawn_clone(dx, dy, dz, go);
                char b[256];
                if (!r.ok) { std::snprintf(b, sizeof(b), "err spawn_clone: %s", r.err); return std::string(b); }
                if (!go) { std::snprintf(b, sizeof(b), "ok spawn_clone %s", r.err); return std::string(b); }
                std::snprintf(b, sizeof(b),
                              "ok spawn_clone inst=%#llx vt=%#llx src=(%.1f,%.1f,%.1f) clone=(%.1f,%.1f,%.1f)",
                              (unsigned long long)r.inst, (unsigned long long)r.vtable, r.before[0],
                              r.before[1], r.before[2], r.moved[0], r.moved[1], r.moved[2]);
                return std::string(b);
            }
            // geom_stats — count loaded geom instances + class histogram (why some objects moved, others
            // not: move_all only touches CSWorldGeomIns-family, is capped, and LOD dupes an asset).
            if (cmd == "geom_stats")
            {
                auto r = goblin::geom_move::geom_stats();
                char b[224];
                std::snprintf(b, sizeof(b), "ok geom_stats %s", r.err);
                return std::string(b);
            }
            // move_all <dx dy dz> — move EVERY loaded geom instance by the delta (mass visual confirm).
            if (cmd == "move_all")
            {
                std::string xs = next_token(rest), ys = next_token(rest), zs = next_token(rest);
                float dx = 0, dy = 0, dz = 0;
                try { dx = std::stof(xs); dy = std::stof(ys); dz = std::stof(zs); }
                catch (...) { return "err usage: move_all <dx> <dy> <dz>"; }
                auto r = goblin::geom_move::move_all(dx, dy, dz);
                char b[96];
                if (!r.ok) { std::snprintf(b, sizeof(b), "err move_all: %s", r.err); return std::string(b); }
                std::snprintf(b, sizeof(b), "ok move_all moved=%llu instances", (unsigned long long)r.inst);
                return std::string(b);
            }
            // move_aeg <aegRow> <dx dy dz> — move the SPECIFIC asset's nearest placement (targeted move).
            if (cmd == "move_aeg")
            {
                std::string as = next_token(rest), xs = next_token(rest), ys = next_token(rest), zs = next_token(rest);
                uint32_t aeg = 0; float dx = 0, dy = 0, dz = 0;
                try { aeg = (uint32_t)std::stoul(as, nullptr, 0); dx = std::stof(xs); dy = std::stof(ys); dz = std::stof(zs); }
                catch (...) { return "err usage: move_aeg <aegRow> <dx> <dy> <dz>"; }
                auto r = goblin::geom_move::move_aeg(aeg, dx, dy, dz);
                char b[224];
                if (!r.ok) { std::snprintf(b, sizeof(b), "err move_aeg: %s", r.err); return std::string(b); }
                std::snprintf(b, sizeof(b), "ok move_aeg aeg=%u inst=%#llx before=(%.2f,%.2f,%.2f) now=(%.2f,%.2f,%.2f)",
                              aeg, (unsigned long long)r.inst, r.before[0], r.before[1], r.before[2],
                              r.moved[0], r.moved[1], r.moved[2]);
                return std::string(b);
            }
            // move_near <dx dy dz> — move the geom instance NEAREST the player (on-screen visual confirm).
            if (cmd == "move_near")
            {
                std::string xs = next_token(rest), ys = next_token(rest), zs = next_token(rest);
                float dx = 0, dy = 0, dz = 0;
                try { dx = std::stof(xs); dy = std::stof(ys); dz = std::stof(zs); }
                catch (...) { return "err usage: move_near <dx> <dy> <dz>"; }
                float dist = -1;
                auto r = goblin::geom_move::move_near(dx, dy, dz, dist);
                char b[224];
                if (!r.ok) { std::snprintf(b, sizeof(b), "err move_near: %s", r.err); return std::string(b); }
                std::snprintf(b, sizeof(b),
                              "ok move_near inst=%#llx dist=%.2f before=(%.2f,%.2f,%.2f) now=(%.2f,%.2f,%.2f)",
                              (unsigned long long)r.inst, dist, r.before[0], r.before[1], r.before[2],
                              r.moved[0], r.moved[1], r.moved[2]);
                return std::string(b);
            }
            if (cmd == "move_hold" || cmd == "move_read" || cmd == "move_restore")
            {
                goblin::geom_move::MoveResult r;
                if (cmd == "move_hold")
                {
                    std::string xs = next_token(rest), ys = next_token(rest), zs = next_token(rest);
                    float dx = 0, dy = 0, dz = 0;
                    try { dx = std::stof(xs); dy = std::stof(ys); dz = std::stof(zs); }
                    catch (...) { return "err usage: move_hold <dx> <dy> <dz>"; }
                    r = goblin::geom_move::move_hold(dx, dy, dz);
                }
                else if (cmd == "move_restore")
                    r = goblin::geom_move::restore_held();
                else
                    r = goblin::geom_move::read_held();
                char b[224];
                if (!r.ok) { std::snprintf(b, sizeof(b), "err %s: %s", cmd.c_str(), r.err); return std::string(b); }
                std::snprintf(b, sizeof(b),
                              "ok %s inst=%#llx before=(%.2f,%.2f,%.2f) now=(%.2f,%.2f,%.2f)", cmd.c_str(),
                              (unsigned long long)r.inst, r.before[0], r.before[1], r.before[2],
                              r.moved[0], r.moved[1], r.moved[2]);
                return std::string(b);
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
