#include "goblin_debug_rpc.hpp"

#include "goblin_config.hpp"
#include "input/input_shared.hpp"
#include "input/input_wndproc.hpp"  // wm_keydown_total — RPC key-delivery verify
#include "goblin_inject.hpp"   // world_map_open() — status field for the driver's boot/nav loop
#include "goblin_overlay_render_api.hpp"  // overlay_api::rebuild_markers (refresh_markers cmd)
#include "input/input_cursor.hpp"  // set_cursor_pos_real — pixel-exact mouse_move via the trampoline
#include "goblin_pause.hpp"    // pause command + paused= status (unfocused-window pause escape)
#include "goblin_freeze_watchdog.hpp"  // present_beat() — frame= heartbeat for RPC freeze detection
#include "goblin_overlay.hpp"
#include "goblin_virtual_world.hpp"  // vworld registry — `vworld` RPC (custom virtual worlds)
#include "worldmap/loot_disk.hpp"    // read_game_file_decompressed — `assets_probe` path-loading guard
#include "worldmap/maptile.hpp"      // maptile::probe — `maptile_probe` (endgame phase-1a tile recon)
#include "worldmap/map_entry_layer.hpp" // far_relief_probe — `far_relief_probe` (D-far -1 Y-cloud frame check)
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
#include "goblin_geom_spawn.hpp"    // spawn_asset cmd — ADD via pivot-2 asset-request path (MSB-write RE)
#include "goblin_heightfield.hpp"   // hf_probe cmd — terrain raycast heightfield (Track D2)
#include "goblin_w2s.hpp"           // w2s_probe cmd — 3D world-to-screen camera calibration
#include "goblin_r3d.hpp"           // r3d cmd — mod-owned D3D12 3D backend test cube
#include "goblin_postfx.hpp"        // postfx cmd — greybox #2b full-screen restyle of ER's frame
#include "goblin_mod.hpp"           // mod cmd — the mod.toml manifest (status/reload)
#include "goblin_dbgrender.hpp"     // dbgrender_probe cmd — greybox #2a debug-draw gate probe
#include "goblin_custom_markers.hpp" // death_mark cmd — the DropSoul death marker
#include "goblin_add_collision.hpp"  // add_collision cmd — Route D walkable box (recon phase)
#include "goblin_field_probe.hpp"  // arm_raw — serialize find-what-accesses (Phase 2)
#include "goblin_build_id.hpp"     // er_exe_version — er_version verb (build fingerprint)

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
        // MUST exceed the WM_INPUT raw-packet echo latency of our OWN injected key — measured ~390ms
        // under Proton/Wine (see input_wndproc.cpp hk_wndproc comment). At the old 300ms the echo landed
        // AFTER the guard expired → note_user_input() logged it as genuine user activity → the next
        // scripted key within kUserIdleWindowMs (1500ms) was falsely suspended, stalling any rapid RPC
        // script (load_save only survived because its ~3s inter-key sleeps outrun the 1500ms window).
        // 700ms covers the echo + margin; since each send_vk/jiggle re-arms from its OWN call time and
        // the guard only ever extends, a held key's KEYUP echo (hold_ms + ~390) is covered too.
        constexpr unsigned kInjectionGuardMs = 700;

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
                return "ok commands: help ping status idlediag open_f1 f1_tab vmap vworld assets_probe maptile_probe pause set screenshot dumpmenu reload_overlay"
                       " | param_get param_set param_getf param_setf param_clone"
                       " | loot_at refresh_markers warp coords warp_local warp_xyz we_scan"
                       " | give_item goods_count strip_test inv_probe fmg_set sidecar bundle"
                       " | exit mfg_build er_base er_version proj mem_dump mem_fwa equip_dump equip_fwa move_asset move_hold move_read move_near move_restore move_all move_aeg geom_stats geom_dump spawn_probe spawn_clone spawn_asset spawn_cap4e80 spawn_capreg add_collision hf_probe hf_probe_present hf_sample hf_shape_probe far_relief_probe far_relief w2s_probe"
                       " | key type mouse_move mouse_click mouse_drag mouse_wheel"
                       "  (usage+caveats: docs/memory/tooling/rpc-commands.md)";
            if (cmd == "idlediag")
            {
                // Why does rpc_input_idle false-fire with no human present? Report the per-source
                // tally of what MOVES the auto-idle clock (poll twice over a gap — the source whose
                // counter climbs while idle is the culprit) + the current idle age/gate.
                unsigned s[4]{};
                goblin::input::idle_diag_snapshot(s);
                const unsigned long long idle = goblin::input::ms_since_user_input();
                char b[224];
                std::snprintf(b, sizeof(b),
                              "ok idlediag recorded[wm_input_kbd=%u wm_mousemove=%u legacy=%u] "
                              "guard_dropped=%u | idle_ms=%llu auto_idle=%d suspended=%d",
                              s[0], s[1], s[2], s[3], idle, goblin::config::rpcAutoIdle ? 1 : 0,
                              (goblin::config::rpcAutoIdle && idle < kUserIdleWindowMs) ? 1 : 0);
                return std::string(b);
            }
            // exit / quit / kill_game — self-terminate the game from inside (dev escape hatch for a
            // FROZEN game: the RPC listener is a separate thread, so this runs even when the render/main
            // thread is soft-hung on a futex/spinlock). TerminateProcess skips DLL_PROCESS_DETACH/atexit
            // (which could deadlock on a lock the frozen thread holds). CAVEAT: a thread wedged in
            // UNINTERRUPTIBLE (D) kernel I/O — GPU/driver/swap — cannot be reaped by this either (same
            // kernel rule as SIGKILL); that class needs the I/O to return or a reboot.
            if (cmd == "exit" || cmd == "quit" || cmd == "kill_game")
            {
                spdlog::warn("[RPC] exit requested — TerminateProcess(self) now.");
                TerminateProcess(GetCurrentProcess(), 0);
                return "ok exiting";   // usually not reached — the process is already gone
            }
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
                       // present-thread heartbeat — poll twice; unchanged while alive = the render/main
                       // thread is frozen (deadlock) even though this RPC listener thread still answers.
                       " frame=" + std::to_string(goblin::freeze_watchdog::present_beat()) +
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
            // f1_tab <Markers|Search|Quests|Display|Dev|0..4> — deterministically select an F1 tab (opens
            // the panel first). For scripted verification so tests drive tabs by name, not pixel-clicks.
            if (cmd == "f1_tab")
            {
                std::string a = next_token(rest);
                if (a.empty()) return "err usage: f1_tab <Markers|Search|Quests|Display|Dev>";
                static const char *kTabs[5] = {"markers", "search", "quests", "display", "dev"};
                std::string la;
                for (char c : a) la += (char)std::tolower((unsigned char)c);
                int idx = -1;
                if (la.size() == 1 && la[0] >= '0' && la[0] <= '4') idx = la[0] - '0';
                else for (int i = 0; i < 5; i++) if (la == kTabs[i]) { idx = i; break; }
                if (idx < 0) return "err f1_tab: unknown tab '" + a + "'";
                goblin::overlay::set_panel_open(true);       // ensure the panel is up
                goblin::overlay_api::f1_request_tab(idx);     // SetSelected next draw (one-shot)
                return "ok f1_tab=" + std::string(kTabs[idx]);
            }
            // vmap 0|1|toggle | fit | group <0-3> — drive the virtual world map (mod page) window.
            // vmap <sub> [args] — the whole virtual-map RPC surface. Dispatch lives in the render module
            // (panel_virtual_map.cpp vmap_rpc_command) so it hot-reloads with the panel + never re-breaks the
            // split; verbs: 0|1|toggle | fit | group | tile | tiles_clear | tiles_resident | tile_recon |
            // items | spiderfy | offmap | find | relief | locate | flip | dump_markers | view | tiles_lod.
            if (cmd == "vmap")
                return goblin::overlay_render_loader::call_vmap_command(rest);
            // assets_probe — path-loading regression guard: does the mod's disk loader resolve the key
            // menu assets on the CURRENT install, and via the loose overlay (ERR/UXM) or the packed
            // dvdbnd (vanilla)? Catches a silent load-path break per install shape (packed/unpacked/ERR).
            // Reports per-file shape+size (avoids the 2GB 71_MapTile.tpfbdt — only its 545KB header).
            if (cmd == "assets_probe")
            {
                static const char *kRels[] = {
                    "menu/71_MapTile.tpfbhd",     // world-map tiles header (endgame phase-1a target)
                    "menu/hi/01_common.tpf.dcx",  // item-icon sheet (the proven DCX disk no-bake path)
                };
                int nloose = 0, npacked = 0, nmiss = 0;
                std::string per;
                for (const char *rel : kRels)
                {
                    const char *shape;
                    size_t sz;
                    auto lo = goblin::worldmap::read_loose_file_decompressed(rel);
                    if (!lo.empty()) { shape = "loose"; sz = lo.size(); ++nloose; }
                    else
                    {
                        auto any = goblin::worldmap::read_game_file_decompressed(rel);
                        if (!any.empty()) { shape = "packed"; sz = any.size(); ++npacked; }
                        else { shape = "MISSING"; sz = 0; ++nmiss; }
                    }
                    char b[160];
                    std::snprintf(b, sizeof(b), " %s=%s(%zu)", rel, shape, sz);
                    per += b;
                }
                const char *overall = nmiss ? "PARTIAL" : (nloose ? "loose/overlay" : "packed/vanilla");
                char head[96];
                std::snprintf(head, sizeof(head), "ok assets_probe shape=%s loose=%d packed=%d missing=%d |",
                              overall, nloose, npacked, nmiss);
                return std::string(head) + per;
            }
            // w2s_probe [dot on|off | conv <0..3> | fovy <rad>] — 3D world-to-screen calibration
            // (docs/re/windows_world_to_screen_camera_re_findings.md). Bare: dump the live camera VIEW
            // matrix + player view-space coords in every interpretation + candidate screen px, on the
            // present frame (no read-tearing). `dot on` draws a crosshair at the projected player pixel;
            // `conv`/`fovy` retune the projection live until the dot locks to the character's feet.
            if (cmd == "w2s_probe")
            {
                std::string sub = next_token(rest);
                if (sub == "dot")
                {
                    std::string v = next_token(rest);
                    goblin::w2s::set_debug_dot(v != "off" && v != "0");
                    return std::string("ok w2s dot ") + (v != "off" && v != "0" ? "on" : "off");
                }
                if (sub == "conv")
                {
                    try { goblin::w2s::set_conv(std::stoi(next_token(rest))); } catch (...) { return "err usage: w2s_probe conv <0..3>"; }
                    return goblin::w2s::probe();
                }
                if (sub == "fovy")
                {
                    try { goblin::w2s::set_fovy(std::stof(next_token(rest))); } catch (...) { return "err usage: w2s_probe fovy <radians>"; }
                    return goblin::w2s::probe();
                }
                return goblin::w2s::probe();
            }
            // maptile_probe [rel_base] [maxProbe] [nameFilter] — endgame phase-1a sub-slice 1b: read the
            // world-map tile archive (BHF4 split: menu/71_MapTile.tpfbhd/.tpfbdt) off the active install,
            // parse the entry table, and probe the first entries through DCX->TPF->DDS so we learn the tile
            // naming/count/dims in-game (the packed 71_MapTile can't be read offline). Full per-tile detail
            // goes to the log; the reply is a one-line summary. Dev-only; no game memory touched.
            if (cmd == "maptile_probe")
            {
                std::string base = next_token(rest);
                if (base.empty()) base = "menu/71_MapTile";
                std::string ms = next_token(rest);
                int mx = 8;
                if (!ms.empty()) { try { mx = std::stoi(ms); } catch (...) {} }
                std::string filt = next_token(rest);
                return goblin::worldmap::maptile::probe(base, mx, filt.empty() ? nullptr : filt.c_str());
            }
            // far_relief_probe — D-far -1: dump the MSB placement Y-cloud distribution per overworld tile
            // (validates whether posY is world-ish or block-local before building the far-relief grid).
            if (cmd == "far_relief_probe")
                return goblin::overlay_render_loader::call_far_relief_probe();
            // far_relief [group] [cellSize] — D-far -1 v0: build the MSB Y-cloud relief field for a vmap
            // group (0=OW 1=UG 2=DLC-OW 3=DLC-UG; default 0) at cellSize (default 128u). The vmap also
            // auto-builds the active group when Relief is on, so this is mainly for a headless/forced build.
            if (cmd == "far_relief")
            {
                std::string gs = next_token(rest), cs = next_token(rest);
                int group = 0, cell = 128;
                if (!gs.empty()) { try { group = std::stoi(gs); } catch (...) {} }
                if (!cs.empty()) { try { cell = std::stoi(cs); } catch (...) {} }
                return goblin::overlay_render_loader::call_build_far_relief(group, cell);
            }
            // vworld create <name> | marker <id> <x> <z> [name] | active <id> | list | clear —
            // drive the virtual-world registry (custom mod worlds shown on the virtual map).
            if (cmd == "vworld")
            {
                std::string sub = next_token(rest);
                auto ltrim = [](std::string &s) { size_t b = s.find_first_not_of(" \t"); s = (b == std::string::npos) ? std::string{} : s.substr(b); };
                if (sub == "create")
                {
                    ltrim(rest);
                    int id = goblin::vworld::create(rest);
                    return "ok vworld create id=" + std::to_string(id);
                }
                if (sub == "marker")
                {
                    std::string is = next_token(rest), xs = next_token(rest), zs = next_token(rest);
                    int id = 0; float x = 0, z = 0;
                    try { id = std::stoi(is); x = std::stof(xs); z = std::stof(zs); }
                    catch (...) { return "err usage: vworld marker <id> <x> <z> [name]"; }
                    ltrim(rest);
                    bool ok = goblin::vworld::add_marker(id, x, z, rest, 0xFFEB82E6u);
                    return ok ? "ok vworld marker" : "err vworld: unknown world " + is;
                }
                if (sub == "active")
                {
                    int id = 0;
                    try { id = std::stoi(next_token(rest)); } catch (...) { return "err usage: vworld active <id>"; }
                    return goblin::vworld::set_active(id) ? "ok vworld active=" + std::to_string(id)
                                                          : "err vworld: unknown world " + std::to_string(id);
                }
                if (sub == "list")
                {
                    std::string out = "ok vworld active=" + std::to_string(goblin::vworld::active()) + " worlds:";
                    for (auto &p : goblin::vworld::list())
                    {
                        out += " [" + std::to_string(p.first) + "]" + p.second;
                        goblin::vworld::World w;
                        if (goblin::vworld::get_world(p.first, w))  // false for the synthetic id 0 (Base ER)
                            out += "(mk=" + std::to_string(w.markers.size()) + ")";
                    }
                    return out;
                }
                if (sub == "clear") { goblin::vworld::clear(); return "ok vworld clear"; }
                if (sub == "save")
                    return goblin::vworld::save_default() ? "ok vworld save" : "err vworld save failed";
                if (sub == "load")
                    return "ok vworld load worlds=" + std::to_string(goblin::vworld::load_default());
                return "err usage: vworld create|marker|active|list|save|load|clear";
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
            // warp <graceId> [offset] — fast-travel to a site of grace (dev-world nav). graceId = the
            // bonfire entity id (e.g. 1042362951 = The First Step, 10002951 = Margit). Optional offset
            // (default 0 = entity id direct, the ground-truthed value; the CT's -1000 landed one bonfire
            // off) is added before LuaWarp_01. Must be in-world + the grace unlocked. grep [WARP].
            if (cmd == "warp")
            {
                std::string id_s = next_token(rest);
                if (id_s.empty()) return "err usage: warp <graceId> [offset] (e.g. 1042362951 = The First Step)";
                int32_t gid = 0, off = 0;
                try { gid = static_cast<int32_t>(std::stol(id_s, nullptr, 0)); }
                catch (...) { return "err bad graceId"; }
                std::string off_s = next_token(rest);
                if (!off_s.empty()) { try { off = static_cast<int32_t>(std::stol(off_s, nullptr, 0)); } catch (...) { return "err bad offset"; } }
                bool ok = goblin::warp::to_grace(gid, off);
                return ok ? "ok warp " + std::to_string(gid) + " offset " + std::to_string(off)
                          : "err warp failed (unresolved / not in-world / grace locked)";
            }
            // hf_probe — queue a heightfield validation cast (Track D2). The cast runs on the GAME
            // thread (the world-map step hook), so OPEN THE MAP after issuing this. Result → [HEIGHTFIELD]
            // in the log (ground Y at the player + normal). Compare ground Y to the player foot Y.
            if (cmd == "hf_probe")
            {
                goblin::heightfield::request_probe();
                return "ok hf_probe queued — open the map; grep [HEIGHTFIELD] in the log";
            }
            // hf_probe_present — like hf_probe but fires on the PRESENT thread during GAMEPLAY (map
            // CLOSED). Tests whether a read-only cast is safe off the game thread (map-open unloads
            // collision, so the game-thread hk_c32f0 path can't sample). Do NOT open the map.
            if (cmd == "hf_probe_present")
            {
                goblin::heightfield::request_present_probe();
                return "ok hf_probe_present queued — stay in gameplay (map CLOSED); grep [HEIGHTFIELD]";
            }
            // hf_shape_probe — cast at the player + scan the ctx for an hknp shape vtable → is the ground a
            // heightfield GRID (readable) or a compressed mesh (raycast-only)? Gameplay, map CLOSED. [HFSHAPE].
            if (cmd == "hf_shape_probe")
            {
                goblin::heightfield::request_shape_probe();
                return "ok hf_shape_probe queued — stay in gameplay (map CLOSED); grep [HFSHAPE]";
            }
            // add_collision — Route D walkable box, staged (brief §5):
            //   add_collision                       -> resolve hknpWorld/bodyMgr only (read-only)
            //   add_collision recon                 -> phase-1 layout dumps ([ADDCOL], read-only)
            //   add_collision <hx> <hy> <hz>        -> build+dump the cinfo, NO world mutation
            //   add_collision <hx> <hy> <hz> go     -> alloc + addBody at player + 40u up
            //   add_collision <hx> <hy> <hz> <x> <y> <z> go  -> at an explicit block-local position
            // Shape is BORROWED from a live body until the box builder lands (findings §6 shortcut).
            if (cmd == "add_collision")
            {
                std::string sub = next_token(rest);
                char b[224];
                if (sub.empty())
                {
                    auto r = goblin::add_collision::resolve_world();
                    if (!r.ok) { std::snprintf(b, sizeof(b), "err add_collision: %s", r.err); return std::string(b); }
                    std::snprintf(b, sizeof(b), "ok add_collision resolve: world=%#llx bodyMgr=%#llx bodies=%#llx count=%u",
                                  (unsigned long long)r.world, (unsigned long long)r.bodyMgr,
                                  (unsigned long long)r.bodies, r.count);
                    return std::string(b);
                }
                if (sub == "recon")
                {
                    auto r = goblin::add_collision::recon();
                    if (!r.ok) { std::snprintf(b, sizeof(b), "err add_collision recon: %s", r.err); return std::string(b); }
                    std::snprintf(b, sizeof(b), "ok add_collision recon: world=%#llx bodyMgr=%#llx count=%u — see [ADDCOL] log",
                                  (unsigned long long)r.world, (unsigned long long)r.bodyMgr, r.count);
                    return std::string(b);
                }
                // numeric form: hx hy hz [x y z] [go]
                float v[6] = {};
                int nv = 0;
                bool force = false;
                std::string tok = sub;
                while (!tok.empty())
                {
                    if (tok == "go") { force = true; break; }
                    if (nv >= 6) return "err too many args — usage: add_collision <hx> <hy> <hz> [<x> <y> <z>] [go]";
                    try { v[nv++] = std::stof(tok); } catch (...) { return "err bad number '" + tok + "'"; }
                    tok = next_token(rest);
                }
                if (nv != 3 && nv != 6) return "err usage: add_collision <hx> <hy> <hz> [<x> <y> <z>] [go]";
                float half[3] = {v[0], v[1], v[2]};
                float pos[3];
                if (nv == 6) { pos[0] = v[3]; pos[1] = v[4]; pos[2] = v[5]; }
                else
                {
                    float px = 0, py = 0, pz = 0;
                    if (!goblin::get_player_world_pos(px, py, pz)) return "err not in-world (no player pos)";
                    pos[0] = px; pos[1] = py + 40.f; pos[2] = pz;   // clearly above the feet → oracle-separable
                }
                auto r = goblin::add_collision::add_box(half, pos, force);
                if (!r.ok) { std::snprintf(b, sizeof(b), "err add_collision: %s", r.err); return std::string(b); }
                if (!force)
                    std::snprintf(b, sizeof(b), "ok add_collision DUMPED cinfo (shape=%#llx borrowed, pos %.1f %.1f %.1f) — append 'go' to add",
                                  (unsigned long long)r.shape, pos[0], pos[1], pos[2]);
                else
                    std::snprintf(b, sizeof(b), "ok add_collision ADDED bodyId=%#x (shape=%#llx pos %.1f %.1f %.1f) — verify hf_probe_present",
                                  r.bodyId, (unsigned long long)r.shape, pos[0], pos[1], pos[2]);
                return std::string(b);
            }
            // hf_sample [extent] [res] — queue a heightfield GRID sample around the player (D2.2).
            // extent = world-units square side (default 4096), res = cells/side (default 48). Runs on the
            // game thread → OPEN THE MAP after issuing. Result → [HEIGHTFIELD] sample DONE (hit%, Y range).
            if (cmd == "hf_sample")
            {
                float extent = 4096.f; int res = 48;
                std::string es = next_token(rest), rs = next_token(rest);
                if (!es.empty()) { try { extent = std::stof(es); } catch (...) { return "err bad extent"; } }
                if (!rs.empty()) { try { res = std::stoi(rs); } catch (...) { return "err bad res"; } }
                goblin::heightfield::request_sample(extent, res);
                return "ok hf_sample queued (extent " + std::to_string((int)extent) + ", res " +
                       std::to_string(res) + ") — stay in gameplay (map CLOSED); grep [HEIGHTFIELD]";
            }
            // vis sec|cat <idx> <0|1> | vis master <0|1> — toggle marker visibility (test the vmap
            // category filter + scriptable). Drives the same config the F1 category checkboxes do.
            if (cmd == "vis")
            {
                std::string kind = next_token(rest), a = next_token(rest), b = next_token(rest);
                try
                {
                    if (kind == "master")
                    {
                        bool on = std::stoi(a) != 0;
                        goblin::ui::set_icons_enabled(on);
                        return std::string("ok master=") + (on ? "1" : "0");
                    }
                    int idx = std::stoi(a);
                    bool on = std::stoi(b) != 0;
                    if (kind == "sec") { goblin::ui::set_section_visible(idx, on); return "ok sec " + a + "=" + (on ? "1" : "0"); }
                    if (kind == "cat") { goblin::ui::set_category_visible(idx, on); return "ok cat " + a + "=" + (on ? "1" : "0"); }
                }
                catch (...) { return "err bad args"; }
                return "err usage: vis sec|cat <idx> <0|1> | vis master <0|1>";
            }
            // death_mark — set the "you died here" marker at the CURRENT player map pos (native DropSoul
            // icon). death_clear — remove it. Manual trigger + test until HP-auto-detect lands.
            if (cmd == "death_mark")
            {
                int area = 0, group = 0; float wx = 0, wz = 0;
                if (!goblin::get_player_map_pos(area, wx, wz, nullptr, nullptr, &group))
                    return "err not in-world";
                goblin::death_marker::set(wx, wz, group, 9999);   // manual test marker (dummy rune count)
                char b[96]; std::snprintf(b, sizeof(b), "ok death_mark world=(%.0f,%.0f) group=%d", wx, wz, group);
                return std::string(b);
            }
            if (cmd == "death_clear") { goblin::death_marker::clear(); return "ok death_clear"; }
            if (cmd == "map_rect")
            {
                // map_rect <MENU_MAP_name> — dump the parsed sheet sub-rect so a name can be cropped +
                // previewed offline. Needs the map-point layout parsed (open the native map once first).
                std::string nm = next_token(rest);
                int x = 0, y = 0, w = 0, h = 0; std::string sheet;
                bool ok = goblin::map_point_rect_by_name(nm, x, y, w, h, sheet);
                char b[192];
                std::snprintf(b, sizeof(b), "ok map_rect name=%s sheet=%s x=%d y=%d w=%d h=%d found=%d",
                              nm.c_str(), sheet.c_str(), x, y, w, h, (int)ok);
                return std::string(b);
            }
            if (cmd == "bloodstain_probe")
            {
                float x = 0, y = 0, z = 0; uint32_t map = 0; int32_t souls = 0;
                bool ok = goblin::inventory::read_bloodstain(x, y, z, map, souls);
                char b[160];
                std::snprintf(b, sizeof(b), "ok bloodstain xyz=(%.1f,%.1f,%.1f) map=0x%08X souls=%d read=%d",
                              x, y, z, map, souls, (int)ok);
                return std::string(b);
            }
            if (cmd == "hp_probe")
            {
                int ac = 0, am = 0, bc = 0, bm = 0, cur = 0, mx = 0;
                bool ok = goblin::debug_player_hp_candidates(ac, am, bc, bm);
                bool got = goblin::get_player_hp(cur, mx);
                char b[192];
                std::snprintf(b, sizeof(b), "ok hp_probe A=(%d/%d) B=(%d/%d) picked=%s(%d/%d) resolved=%d",
                              ac, am, bc, bm, got ? "yes" : "no", cur, mx, (int)ok);
                return std::string(b);
            }
            // proj <area> <gx> <gz> [px] [pz] — call the LIVE engine converter worldmap_probe::project
            // (raw area/grid/pos → map-space u,v + page). Test primitive for converter RESIDENCY: it must
            // keep returning the same valid u,v AFTER the native map closes (the VM is cached + persists) —
            // the property Fork 2's underground projection AND the M5 native-draw cull depend on. err if the
            // VM isn't resolved yet (map never opened this session) or project declines.
            if (cmd == "proj")
            {
                std::string as = next_token(rest), gxs = next_token(rest), gzs = next_token(rest),
                            pxs = next_token(rest), pzs = next_token(rest);
                int area = 0, gx = 0, gz = 0; float px = 0.f, pz = 0.f;
                try { area = std::stoi(as); gx = std::stoi(gxs); gz = std::stoi(gzs);
                      if (!pxs.empty()) px = std::stof(pxs); if (!pzs.empty()) pz = std::stof(pzs); }
                catch (...) { return "err usage: proj <area> <gx> <gz> [px] [pz]"; }
                float u = 0.f, v = 0.f; int page = -1;
                bool ok = goblin::worldmap_probe::project(area, gx, gz, px, pz, u, v, page);
                char b[128];
                if (!ok) { std::snprintf(b, sizeof(b), "err proj: converter unresolved (map never opened?) or declined"); return std::string(b); }
                std::snprintf(b, sizeof(b), "ok proj area=%d grid=(%d,%d) -> u=%.2f v=%.2f page=%d", area, gx, gz, u, v, page);
                return std::string(b);
            }
            // proj_conv <area> <gxbase> <gzbase> <ox> <oz> <bx> <bz> <scale> <gx> <gz> [px] [pz] —
            // fd0ad45 validation: build an OFF-VM converter slot from the given fields (in our own memory)
            // and project ONE point through the engine fn FUN_140876140 — the native map NEVER opened, no
            // live VM. Lets the test confirm the world→map-space affine is reproducible MAP-CLOSED: pick the
            // exe-invariant fields, compare du/dv to the map-open `proj` reference (must be 0). legacyNode=0
            // (base affine only; the legacy-dungeon fold stays with goblin::legacy_fold).
            if (cmd == "proj_conv")
            {
                std::string as = next_token(rest), gxbs = next_token(rest), gzbs = next_token(rest),
                            oxs = next_token(rest), ozs = next_token(rest), bxs = next_token(rest),
                            bzs = next_token(rest), scs = next_token(rest), gxs = next_token(rest),
                            gzs = next_token(rest), pxs = next_token(rest), pzs = next_token(rest);
                int area = 0, gxb = 0, gzb = 0, gx = 0, gz = 0;
                float ox = 0, oz = 0, bx = 0, bz = 0, sc = 1.f, px = 0.f, pz = 0.f;
                try {
                    area = std::stoi(as); gxb = std::stoi(gxbs); gzb = std::stoi(gzbs);
                    ox = std::stof(oxs); oz = std::stof(ozs); bx = std::stof(bxs); bz = std::stof(bzs);
                    sc = std::stof(scs); gx = std::stoi(gxs); gz = std::stoi(gzs);
                    if (!pxs.empty()) px = std::stof(pxs); if (!pzs.empty()) pz = std::stof(pzs);
                } catch (...) {
                    return "err usage: proj_conv <area> <gxbase> <gzbase> <ox> <oz> <bx> <bz> <scale> <gx> <gz> [px] [pz]";
                }
                float u = 0.f, v = 0.f;
                bool ok = goblin::worldmap_probe::project_offvm(area, gxb, gzb, ox, oz, bx, bz, sc, gx, gz, px, pz, u, v);
                char b[160];
                if (!ok) { std::snprintf(b, sizeof(b), "err proj_conv: FUN_140876140 unresolved or point rejected"); return std::string(b); }
                std::snprintf(b, sizeof(b), "ok proj_conv area=%d grid=(%d,%d) -> u=%.2f v=%.2f", area, gx, gz, u, v);
                return std::string(b);
            }
            // proj_nvm <area> <gx> <gz> [px] [pz] — force the map-CLOSED no-VM projection path
            // (worldmap_probe::project_no_vm): base off-VM affine for 60/61/12, else fold via the resident
            // WorldMapLegacyConvParam + base affine for legacy-dungeon / DLC-UG areas. Must equal the live
            // `proj` (same u,v,page) for any placed area. Lets the test check off-VM+fold equivalence even
            // with the map open — `proj` would otherwise use the cached live VM and shadow this path.
            if (cmd == "proj_nvm")
            {
                std::string as = next_token(rest), gxs = next_token(rest), gzs = next_token(rest),
                            pxs = next_token(rest), pzs = next_token(rest);
                int area = 0, gx = 0, gz = 0; float px = 0.f, pz = 0.f;
                try { area = std::stoi(as); gx = std::stoi(gxs); gz = std::stoi(gzs);
                      if (!pxs.empty()) px = std::stof(pxs); if (!pzs.empty()) pz = std::stof(pzs); }
                catch (...) { return "err usage: proj_nvm <area> <gx> <gz> [px] [pz]"; }
                float u = 0.f, v = 0.f; int page = -1;
                bool ok = goblin::worldmap_probe::project_no_vm(area, gx, gz, px, pz, u, v, page);
                char b[128];
                if (!ok) { std::snprintf(b, sizeof(b), "err proj_nvm: no base match + no fold row (area not placed off-VM)"); return std::string(b); }
                std::snprintf(b, sizeof(b), "ok proj_nvm area=%d grid=(%d,%d) -> u=%.2f v=%.2f page=%d", area, gx, gz, u, v, page);
                return std::string(b);
            }
            // conv_affine <area> — dump the LIVE per-area converter slot fields (origin/bias/scale/gridbase)
            // read from the resolved VM (needs the map opened once). Lets the test CAPTURE the exe-invariant
            // fields, close the map, then replay them through proj_conv (off-VM) and assert du/dv==0.
            if (cmd == "conv_affine")
            {
                int area = 0;
                try { area = std::stoi(next_token(rest)); } catch (...) { return "err usage: conv_affine <area>"; }
                goblin::worldmap_probe::ConvAffine a{};
                if (!goblin::worldmap_probe::get_converter_affine(area, a))
                    return "err conv_affine: no live converter for that area (map never opened?)";
                char b[192];
                std::snprintf(b, sizeof(b),
                              "ok conv_affine area=%d gxbase=%d gzbase=%d origin=%.3f,%.3f bias=%.3f,%.3f scale=%.4f",
                              a.area, a.gridXbase, a.gridZbase, a.originX, a.originZ, a.biasX, a.biasZ, a.scale);
                return std::string(b);
            }
            // dbgrender_probe — greybox #2a: probe ER's debug-draw gate DAT_143d85b18 (er+0x3d85b18).
            // `read` (classifies flag vs pointer), `set <hex>` (SEH write). See
            // windows_debug_render_flag_re_findings.md. Pair with `screenshot` to see if debug primitives draw.
            if (cmd == "dbgrender_probe" || cmd == "dbgrender")
            {
                std::string a = next_token(rest);
                if (a == "set")
                {
                    std::string vs = next_token(rest);
                    uint64_t v = 0;
                    try { v = std::stoull(vs, nullptr, 0); } catch (...) { return "err usage: dbgrender_probe set <value|0xhex>"; }
                    return goblin::dbgrender::probe_write(v) ? "ok dbgrender wrote" : "err dbgrender write failed";
                }
                if (a == "read" || a.empty()) return goblin::dbgrender::probe_read();
                if (a == "findhk") return goblin::dbgrender::find_hkdbg();
                if (a == "dump")
                {
                    uintptr_t rva = 0; size_t len = 0x80;
                    try { rva = std::stoull(next_token(rest), nullptr, 0); std::string ls = next_token(rest); if (!ls.empty()) len = std::stoull(ls, nullptr, 0); }
                    catch (...) { return "err usage: dbgrender_probe dump <rva> [len]"; }
                    return goblin::dbgrender::dump(rva, len);
                }
                if (a == "poke8")
                {
                    uintptr_t rva = 0; unsigned val = 0;
                    try { rva = std::stoull(next_token(rest), nullptr, 0); val = std::stoul(next_token(rest), nullptr, 0); }
                    catch (...) { return "err usage: dbgrender_probe poke8 <rva> <byte>"; }
                    return goblin::dbgrender::poke8(rva, (uint8_t)val) ? "ok poked" : "err poke failed";
                }
                return "err usage: dbgrender_probe read|dump <rva> [len]|findhk|poke8 <rva> <b>|set <val>";
            }
            // mod — the mod.toml manifest (mod_manifest_system_plan.md). `mod status` = what loaded;
            // `mod reload` = re-parse + re-apply the mod folder's mod.toml (dev: edit + reload live).
            if (cmd == "mod")
            {
                std::string a = next_token(rest);
                if (a == "reload") { goblin::mod::reload(); return goblin::mod::status(); }
                if (a == "status" || a.empty()) return goblin::mod::status();
                return "err usage: mod status|reload";
            }
            // postfx — greybox job #2b: restyle ER's final frame via a full-screen post-process pass in the
            // present hook (no ER-shader RE). `postfx 0|1|toggle`, `postfx mode <1..4>` (1 grayscale, 2
            // posterize, 3 edge-outline, 4 edge+desat), `postfx strength <f>`.
            if (cmd == "postfx")
            {
                std::string a = next_token(rest);
                if (a == "mode")
                {
                    try { goblin::postfx::set_mode(std::stoi(next_token(rest))); } catch (...) { return "err usage: postfx mode <1..4>"; }
                    return "ok postfx mode set";
                }
                if (a == "strength")
                {
                    try { goblin::postfx::set_strength(std::stof(next_token(rest))); } catch (...) { return "err usage: postfx strength <f>"; }
                    return "ok postfx strength set";
                }
                bool on = goblin::postfx::enabled();
                if (a == "1" || a == "on") on = true;
                else if (a == "0" || a == "off") on = false;
                else if (a == "toggle" || a.empty()) on = !on;
                else return "err usage: postfx 0|1|toggle | mode <1..4> | strength <f>";
                goblin::postfx::set_enabled(on);
                return std::string("ok postfx ") + (on ? "on (restyle ER frame)" : "off");
            }
            // r3d — mod-owned D3D12 3D backend test (virtual_world_3d_backend_plan.md step 1). Toggles a
            // spinning greybox wireframe cube drawn into the swapchain by OUR pipeline. `r3d 1|0|toggle`.
            if (cmd == "r3d")
            {
                std::string a = next_token(rest);
                if (a == "box")   // r3d box <x> <y> <z> [size] — debug box at a WORLD position
                {
                    try {
                        float x = std::stof(next_token(rest)), y = std::stof(next_token(rest)), z = std::stof(next_token(rest));
                        std::string ss = next_token(rest); float sz = ss.empty() ? 1.0f : std::stof(ss);
                        goblin::r3d::add_box(x, y, z, sz);
                        goblin::r3d::set_enabled(true);
                    } catch (...) { return "err usage: r3d box <x> <y> <z> [size]"; }
                    char b[64]; std::snprintf(b, sizeof(b), "ok r3d box added (%d total)", goblin::r3d::box_count());
                    return std::string(b);
                }
                if (a == "clear") { goblin::r3d::clear_boxes(); return "ok r3d boxes cleared"; }
                bool on = goblin::r3d::enabled();
                if (a == "1" || a == "on") on = true;
                else if (a == "0" || a == "off") on = false;
                else if (a == "toggle" || a.empty()) on = !on;
                else return "err usage: r3d 0|1|toggle | box <x> <y> <z> [size] | clear";
                goblin::r3d::set_enabled(on);
                return std::string("ok r3d ") + (on ? "on" : "off");
            }
            // movieclip — native-map viewport diagnostic + a DISPROVEN cull experiment. `read` reports the
            // live Scaleform map clip rect + buffer size (a useful live map-viewport readout). `hide`/`show`
            // arm/disarm a per-frame zero-write of MovieImpl+0xB0 — but that clip is DESCRIPTIVE, not a render
            // gate: zeroing it does NOT hide the map (proven live 2026-07-05, screenshots). Kept as reusable
            // scaffolding for a future movie visible/enable-flag hunt. See §4c/§4d of
            // windows_native_map_render_toggle_re_findings.md (the D3D12 scissor path is also dead).
            if (cmd == "movieclip")
            {
                std::string sub = next_token(rest);
                if (sub == "hide") { goblin::worldmap_probe::movieclip_set_hide(true);
                    return "ok movieclip hide (writes clip=0 each frame — INERT: does not cull, see findings §4d)"; }
                if (sub == "show") { goblin::worldmap_probe::movieclip_set_hide(false);
                    return "ok movieclip show (restoring original clip)"; }
                if (sub == "read" || sub.empty())
                {
                    int r[4] = {0,0,0,0}, bw[2] = {0,0};
                    if (!goblin::worldmap_probe::movieclip_read(r, bw))
                        return "err movieclip: MovieImpl unresolved (map open? in-world?)";
                    char b[160];
                    std::snprintf(b, sizeof(b),
                        "ok movieclip clip=(L=%d,T=%d,W=%d,H=%d) buf=(%d,%d) armed=%d",
                        r[0], r[1], r[2], r[3], bw[0], bw[1], goblin::worldmap_probe::movieclip_hiding()?1:0);
                    return std::string(b);
                }
                return "err usage: movieclip read|hide|show";
            }
            // mfg_build — FRESHNESS GUARD. Returns the compile time of THIS RPC translation unit
            // (goblin_debug_rpc.cpp). Adding/changing ANY verb edits this file → its __TIME__ advances,
            // so a stale DLL is detectable: `ping` answers even from an OLD DLL (the listener lives), but
            // mfg_build reveals the actual build. Check this BEFORE RPC-verifying new code — a rebuild
            // needs a game RESTART/hot-reload to load (a redeploy alone keeps the old DLL resident).
            if (cmd == "mfg_build")
                return std::string("ok mfg_build built=") + __DATE__ + " " + __TIME__;
            // er_base — absolute base of eldenring.exe, so a Python RPM client can turn er+RVA
            // anchors into absolute addresses for mem_dump/mem_fwa (tools/hf_hook_scout.py).
            if (cmd == "er_base")
            {
                uintptr_t er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
                if (!er) return "err eldenring.exe not found";
                char b[48];
                std::snprintf(b, sizeof(b), "ok er_base=%#llx", (unsigned long long)er);
                return std::string(b);
            }
            // er_version — eldenring.exe file version ("a.b.c.d"). The build fingerprint the fixed
            // RVAs/AOBs are pinned to; verify it matches before trusting an RVA-derived address
            // (docs/re/patch_diff_maintenance.md). Twin of er_base + the [BUILD] boot-log line.
            if (cmd == "er_version")
            {
                std::string v = goblin::er_exe_version();
                if (v.empty()) return "err version unavailable";
                return "ok er_version=" + v;
            }
            // coords — player position in BOTH frames the teleport work needs: the tile-local Havok
            // frame (LocalPlayer+0x6C0, what er_console_mod's coords/tp use) and the unified WORLD /
            // marker frame (grid*256 + local). Cross-checks er_console's frame + feeds warp_xyz.
            if (cmd == "coords")
            {
                float lx, ly, lz;
                if (!goblin::get_player_world_pos(lx, ly, lz))
                    return "err not in-world (LocalPlayer null / loading)";
                int area = 0, gx = 0, gz = 0; float wx = 0, wz = 0;
                bool okw = goblin::get_player_map_pos(area, wx, wz, &gx, &gz);
                char b[224];
                if (okw)
                    std::snprintf(b, sizeof(b),
                        "ok local=(%.2f,%.2f,%.2f) world=(%.2f,%.2f) area=%d grid=(%d,%d)",
                        lx, ly, lz, wx, wz, area, gx, gz);
                else
                    std::snprintf(b, sizeof(b),
                        "ok local=(%.2f,%.2f,%.2f) world=? (map-pos unresolved)", lx, ly, lz);
                return std::string(b);
            }
            // warp_local <x> <y> <z> — write the tile-local Havok pos DIRECTLY (mirrors er_console
            // `tp`). Absolute within the current tile frame. The discriminating test: warp_local to
            // the SAME x y z twice → same spot ⇒ absolute-in-frame; drifts ⇒ pure delta.
            if (cmd == "warp_local")
            {
                std::string xs = next_token(rest), ys = next_token(rest), zs = next_token(rest);
                if (xs.empty() || ys.empty() || zs.empty()) return "err usage: warp_local <x> <y> <z>";
                float x, y, z;
                try { x = std::stof(xs); y = std::stof(ys); z = std::stof(zs); }
                catch (...) { return "err bad x/y/z"; }
                if (!goblin::write_player_local_pos(x, y, z, /*set_y=*/true))
                    return "err write failed (not in-world?)";
                float rx = 0, ry = 0, rz = 0; goblin::get_player_world_pos(rx, ry, rz);  // read-back
                char b[160];
                std::snprintf(b, sizeof(b),
                    "ok warp_local set=(%.2f,%.2f,%.2f) readback=(%.2f,%.2f,%.2f)", x, y, z, rx, ry, rz);
                return std::string(b);
            }
            // warp_xyz <worldX> <worldZ> [worldY] — ABSOLUTE teleport in the unified WORLD/marker
            // frame. Converts via the CONFIRMED linear map (world = grid*256 + local): keep the
            // current tile, newLocal = curLocal + (worldTarget − curWorldRaw). Intra-region only —
            // a far cross-map target may hit unstreamed void (the streaming gate). worldY = height.
            if (cmd == "warp_xyz")
            {
                std::string xs = next_token(rest), zs = next_token(rest), ys = next_token(rest);
                if (xs.empty() || zs.empty()) return "err usage: warp_xyz <worldX> <worldZ> [worldY]";
                float twx, twz, twy = 0.0f; bool has_y = !ys.empty();
                try { twx = std::stof(xs); twz = std::stof(zs); if (has_y) twy = std::stof(ys); }
                catch (...) { return "err bad coords"; }
                float lx, ly, lz;
                if (!goblin::get_player_world_pos(lx, ly, lz)) return "err not in-world";
                int area = 0; float cwx = 0, cwz = 0;
                if (!goblin::get_player_raw_pos(area, cwx, cwz)) return "err world-pos unresolved";
                float nlx = lx + (twx - cwx), nlz = lz + (twz - cwz), nly = has_y ? twy : ly;
                if (!goblin::write_player_local_pos(nlx, nly, nlz, /*set_y=*/true))
                    return "err write failed";
                int narea = 0; float rwx = 0, rwz = 0;
                goblin::get_player_raw_pos(narea, rwx, rwz);  // read-back in the world frame
                char b[224];
                std::snprintf(b, sizeof(b),
                    "ok warp_xyz target_world=(%.2f,%.2f) readback_world=(%.2f,%.2f) local=(%.2f,%.2f,%.2f)",
                    twx, twz, rwx, rwz, nlx, nly, nlz);
                return std::string(b);
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
                // `mem_fwa off` (or `disarm`) — tear down the current watch + free the single FWA slot so a
                // new arm can proceed (the slot otherwise wedges on a stale probe; no other way to release it).
                if (a_s == "off" || a_s == "disarm")
                    return goblin::field_probe::disarm_reset() ? "ok FWA disarmed (was armed)"
                                                               : "ok FWA already idle";
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
            // spawn_asset [AEGname] [force] — ADD via pivot 2 (asset streaming-REQUEST). Resolves the reqMgr
            // singleton (GEOM_REQ_MGR AOB) → FUN_1406a5080(reqMgr, L"AEG###_###").
            // Default (no force) QUEUES the request; the drain runs it on the game's own main-update thread
            // via the per-frame hook on FUN_1406d31f0 (er+0x6d31f0) — the direct present-thread call
            // DEADLOCKS (lock inversion vs the streamer), a worker call FAULTS. `force` fires the direct
            // call anyway (WILL hang; diagnostic only). No arg = resolve-only.
            // RE: docs/re/windows_geom_spawn_thread_re_findings.md.
            if (cmd == "spawn_asset")
            {
                std::string name = next_token(rest);
                std::string f = next_token(rest);
                bool force = (f == "force" || f == "1" || f == "go");
                char b[256];
                if (name.empty())
                {
                    auto r = goblin::geom_spawn::resolve_req_mgr();
                    if (!r.ok) { std::snprintf(b, sizeof(b), "err spawn_asset: %s", r.err); return std::string(b); }
                    std::snprintf(b, sizeof(b), "ok spawn_asset reqMgr singleton=%#llx reqMgr=%#llx (resolve-only; direct call deadlocks — see findings)",
                                  (unsigned long long)r.singleton, (unsigned long long)r.reqMgr);
                    return std::string(b);
                }
                auto r = goblin::geom_spawn::spawn_asset(name.c_str(), force);
                if (!r.ok) { std::snprintf(b, sizeof(b), "err spawn_asset %s: %s", name.c_str(), r.err); return std::string(b); }
                if (!force)
                {
                    std::snprintf(b, sizeof(b), "ok spawn_asset %s QUEUED reqMgr=%#llx — serviced on the game main-update thread (FUN_1406d31f0 hook); check log for 'step serviced'",
                                  name.c_str(), (unsigned long long)r.reqMgr);
                    return std::string(b);
                }
                std::snprintf(b, sizeof(b), "ok spawn_asset %s FORCED req=%#llx reqMgr=%#llx%s", name.c_str(),
                              (unsigned long long)r.req, (unsigned long long)r.reqMgr,
                              r.req ? "" : " (req=0)");
                return std::string(b);
            }
            // spawn_cap4e80 — arm a read-only capture hook on the native by-id spawn helper FUN_1406d4e80
            // to RE the OPEN drain-fault wall: logs live (state, aegId, worldPos) + state↔singleton relation
            // each time the streamer calls it. Then WARP/MOVE to force block streaming and read [CAP4e80]
            // log lines. RE: docs/re/windows_geom_spawn_thread_re_findings.md "NEXT wall".
            if (cmd == "spawn_cap4e80")
            {
                bool ok = goblin::geom_spawn::arm_byid_capture();
                return ok ? std::string("ok spawn_cap4e80 armed FUN_1406d4e80 — now warp/move; grep log [CAP4e80]")
                          : std::string("err spawn_cap4e80: arm failed (in-world? eldenring.exe base?)");
            }
            // spawn_capreg — arm a read-only capture on the registrar FUN_1406a5080 to log the legit
            // (param_1, name) the engine calls it with during streaming (the by-id helper never fires).
            // Warp/move to trigger; grep log [CAPREG]. RE: windows_geom_spawn_thread_re_findings.md.
            if (cmd == "spawn_capreg")
            {
                bool ok = goblin::geom_spawn::arm_registrar_capture();
                return ok ? std::string("ok spawn_capreg armed FUN_1406a5080 — now warp/move; grep log [CAPREG]")
                          : std::string("err spawn_capreg: arm failed (in-world? AOB?)");
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
