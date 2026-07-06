#include "input_wndproc.hpp"
#include "input_shared.hpp"

#include <atomic>

#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <spdlog/spdlog.h>

#include "goblin_inject.hpp"          // goblin::world_map_open()
#include "goblin_config.hpp"          // goblin::config::vmapOnMapKey — vmap-covers-map input gate
#include "goblin_virtual_world.hpp"   // goblin::vworld::active() — custom-world open-on-map-key path
#include "goblin_overlay_render_loader.hpp"  // call_inworld_hovered()

// ImGui's Win32 backend message handler (defined in imgui_impl_win32.cpp) — not declared by
// the public backend header in this ImGui version, same extern goblin_overlay.cpp uses.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace goblin::input
{
namespace
{
WNDPROC o_orig_wndproc = nullptr;

// [KBDIAG] dx-bugs 2026-07-01 followup: <user> reports the keyboard can lose the "hook"
// while typing in the item-search bar EVEN WITHOUT any Alt+Tab (so distinct from the
// g_has_focus/[FOCUSDIAG] fix — this is an in-focus keyboard-loss, secondary bug, not yet
// reproduced/explained). Counts raw WM_CHAR/WM_KEYDOWN arrival at the wndproc level so the
// periodic [KBDIAG] log (goblin_overlay.cpp, same 1/sec cadence as [CURSORDIAG]) can tell
// apart "no keyboard messages are arriving at all" (real OS/hook-level loss) from "messages
// arrive but ImGui isn't consuming them" (WantCaptureKeyboard false / ActiveID not the
// search field — an internal ImGui/nav state issue, e.g. gamepad nav stealing focus away
// from the InputText).
std::atomic<unsigned> g_diag_wm_char{0};
std::atomic<unsigned> g_diag_wm_keydown{0};
// Monotonic total (never reset) — the RPC input path polls it to VERIFY an injected key
// actually reached the game's wndproc (first-command-after-refocus loss, 2026-07-02).
std::atomic<unsigned> g_wm_keydown_total{0};
// diag: real WM_LBUTTONDOWN reaching us while the panel is open (0 => ER raw-input swallows
// legacy click messages -> poll buttons instead).
std::atomic<unsigned> g_wndproc_lbdown_while_open{0};

// RPC auto-idle (2026-07-03): tick of the last GENUINE user kb/mouse activity, so the debug
// RPC can suspend its own SendInput injection while the human is driving (no scripted-vs-human
// input fight). The catch is that our OWN injected input generates the SAME WM messages — so
// mark_rpc_injection() arms a short guard window around each RPC SendInput, and note_user_input()
// ignores activity that lands inside it. 0 = no user input seen yet.
std::atomic<ULONGLONG> g_last_user_input_tick{0};
std::atomic<ULONGLONG> g_rpc_injection_guard_until{0};

// [IDLEDIAG] per-source tally of note_user_input — to find why rpc_input_idle false-fires when no
// human is present. Index: 0 WM_INPUT-kbd raw packet, 1 WM_MOUSEMOVE (legacy), 2 legacy WM_KEY/click;
// 3 = guard-dropped (our own echo). src 1 is TALLIED but no longer moves the clock (see below).
std::atomic<unsigned> g_note_src[4]{};

// Returns true when the activity RECORDED (moved the auto-idle clock), false otherwise.
bool note_user_input(int src)
{
    const ULONGLONG now = GetTickCount64();
    // Drop activity that is really our own RPC injection echoing back through the wndproc.
    if (now < g_rpc_injection_guard_until.load(std::memory_order_relaxed))
    {
        g_note_src[3].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (src >= 0 && src < 3) g_note_src[src].fetch_add(1, std::memory_order_relaxed);
    // Mouse MOVEMENT alone does NOT move the idle clock: it doesn't fight scripted KEY injection, and
    // it is generated spuriously by our own cursor recenter + Proton/Wine during load — measured ~151
    // phantom moves per headless boot, the actual cause of rpc_input_idle false-firing in tests (a key
    // scripted right after load_save landed inside the 1.5s window of the last phantom move). Only
    // genuine key/click activity (a human demonstrably typing/clicking) suspends RPC input. The ini
    // rpc_auto_idle switch still disables the whole gate.
    if (src == 1) return false;
    g_last_user_input_tick.store(now, std::memory_order_relaxed);
    return true;
}

bool vmap_covers_map()
{
    // Fullscreen vmap standing in for the native map: the game map is open, AND we open the vmap on
    // the map key (either the vmap_on_map_key opt-in for the base world, or a custom virtual world is
    // active — the "M, not F1" path). Both bits are host-side (config + vworld are host-exported), so
    // this needs no render call. Approximates panel::virtual_map_fullscreen()'s s_from_map for input
    // gating (true over the same open→close window).
    return goblin::world_map_open() &&
           (goblin::config::vmapOnMapKey || goblin::vworld::active() != 0);
}

LRESULT CALLBACK hk_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // Keyboard-arrival counter, RAW-INPUT leg: in gameplay ER runs keyboard raw-input
    // NOLEGACY, so WM_KEYDOWN never arrives and the legacy-message count below goes silent —
    // the RPC key-delivery verify then false-retries EVERY key (benign — down,down,up is one
    // logical press — but +~390ms latency each). WM_INPUT still traverses this wndproc in
    // every state, so count keyboard-type raw packets too (header-only read; must filter
    // RIM_TYPEKEYBOARD or the mouse-move WM_INPUT flood would fake key arrivals).
    if (msg == WM_INPUT)
    {
        RAWINPUTHEADER rh;
        UINT sz = sizeof(rh);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_HEADER, &rh, &sz,
                            sizeof(RAWINPUTHEADER)) == sizeof(rh) &&
            rh.dwType == RIM_TYPEKEYBOARD)
        {
            g_wm_keydown_total.fetch_add(1, std::memory_order_relaxed);
            note_user_input(0);  // NOLEGACY gameplay: raw packets are the only kb signal
        }
    }

    // Focus messages MUST always reach ImGui, independent of menu_open(). g_show is
    // recomputed once/frame from a foreground-window check, so it can still be FALSE for a
    // frame or two right after the OS delivers WM_SETFOCUS on alt-tab-back — if that message
    // only reached ImGui_ImplWin32_WndProcHandler inside the `if (menu_open())` branch below,
    // ImGui's internal focus-lost state never clears and UpdateMouseData() permanently stops
    // writing the mouse position (the "F1 opens but the cursor never responds again" bug
    // after alt-tab+back — see docs/re/proton11_cursor_lock_re_prompt.md's H3). Cheap and
    // side-effect-free to forward unconditionally: ImGui's own handler no-ops these when its
    // context isn't initialized yet, and forwarding while the panel is closed only updates
    // ImGui's idle io.AddFocusEvent state, nothing visible.
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS)
    {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
        // [FOCUSDIAG] dx-bugs 2026-07-01 "alt-tab back, ImGui receives no input" followup —
        // the fg-gate/debounce fix on last_input_was_gamepad did NOT resolve this in-game
        // (still reproduces per <user>), so this logs the raw focus-transition + io state
        // instead of guessing a third time. Rare event (only fires on actual focus changes),
        // safe to always log.
        {
            ImGuiIO *io = ImGui::GetCurrentContext() ? &ImGui::GetIO() : nullptr;
            spdlog::info("[FOCUSDIAG] {} g_show={} g_user_show={} WantCaptureMouse={} "
                         "WantCaptureKeyboard={} MousePos=({:.0f},{:.0f}) NavActive={}",
                         (msg == WM_SETFOCUS) ? "WM_SETFOCUS" : "WM_KILLFOCUS",
                         menu_open(), user_show(),
                         io ? io->WantCaptureMouse : false,
                         io ? io->WantCaptureKeyboard : false,
                         io ? io->MousePos.x : -1.0f, io ? io->MousePos.y : -1.0f,
                         io ? (io->NavActive ? "1" : "0") : "?");
        }
        // ROOT CAUSE (confirmed via the [FOCUSDIAG] log above, 2026-07-01): `fg` used to be
        // re-polled every present frame via GetForegroundWindow()==g_hwnd, which flapped
        // true/false several times during a single real Alt+Tab-back under Wine (the
        // compositor transition briefly hands foreground to something else for a few
        // frames) — see g_has_focus's declaration comment. Track focus from these
        // event-driven messages instead; they only fire on real transitions.
        set_has_focus(msg == WM_SETFOCUS);
    }

    // Losing focus (alt-tab away): reset the pad-switch state machine to a clean slate.
    // Without this, residual pad activity while backgrounded (idle hand on the stick —
    // hk_present's gamepad poll has no fg gate on its OWN read, only on whether it acts
    // on it) could leave last_input_was_gamepad/streak primed, so regaining focus could
    // immediately resume mid-transition instead of starting fresh (dx-bugs 2026-07-01
    // "alt-tab back, ImGui receives no input" followup).
    if (msg == WM_KILLFOCUS)
    {
        set_last_input_was_gamepad(false);
        set_gamepad_active_streak(0);
    }

    // Real mouse/keyboard activity means input is no longer pad-only (see the XInput poll
    // in hk_present, item 2 of dx-bugs-backlog PR C) — clear regardless of overlay state.
    switch (msg)
    {
    case WM_MOUSEMOVE:
        // Our OWN recenter (item 2/6) calls SetCursorPos, which generates exactly this
        // message — don't let our own cursor move look like "real" mouse input, or it
        // re-arms the gamepad-switch edge next frame and the two feed each other forever.
        if (ignore_next_mousemove_for_gamepad_flag())
            set_ignore_next_mousemove_for_gamepad_flag(false);
        else
        {
            set_last_input_was_gamepad(false);
            set_gamepad_active_streak(0);
            note_user_input(1);  // a real mouse move (not our own recenter)
        }
        break;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR:
        // A real mouse click/wheel/keypress cancels any in-progress pad-switch detection
        // outright (see kGamepadSwitchDebounceFrames below) — the user is demonstrably
        // still on mouse/kb right now, regardless of what the pad happens to report.
        set_last_input_was_gamepad(false);
        set_gamepad_active_streak(0);
        note_user_input(2);  // real click/wheel/keypress — RPC auto-idle signal
        // [KBDIAG] raw arrival count, independent of menu_open()/consumption — see the
        // g_diag_wm_char/g_diag_wm_keydown declaration comment.
        if (msg == WM_CHAR)
            g_diag_wm_char.fetch_add(1, std::memory_order_relaxed);
        else if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
        {
            g_diag_wm_keydown.fetch_add(1, std::memory_order_relaxed);
            g_wm_keydown_total.fetch_add(1, std::memory_order_relaxed);
        }
        break;
    default:
        break;
    }

    if (menu_open())
    {
        // dx-bugs F3 (2026-07-01, deterministic repro): a real Alt+Tab cycle can permanently
        // stop legacy WM_CHAR/WM_KEYDOWN/WM_KEYUP delivery to this wndproc (same RIDEV_NOLEGACY
        // family as the mouse-click Proton fix below) — before that point they work fine, so
        // forwarding them here risked DOUBLE input once a poll fallback existed. Same fix as
        // mouse buttons: keyboard TEXT input is fed exclusively by
        // goblin::input::poll_keyboard_text_input() (hk_present, every frame while the menu is
        // open) — a single source of truth that doesn't depend on whether these messages ever
        // arrive. Still forward everything else (mouse/focus/wheel) to ImGui as before.
        const bool isKeyboardMsg = (msg == WM_CHAR || msg == WM_KEYDOWN || msg == WM_KEYUP ||
                                    msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP);
        if (!isKeyboardMsg)
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
        if (msg == WM_LBUTTONDOWN) g_wndproc_lbdown_while_open.fetch_add(1, std::memory_order_relaxed);
        // While the menu is open, swallow ALL mouse/keyboard input so the
        // game gets none of it — regardless of where the cursor is (over the
        // map, the panel, anywhere). ImGui was already fed above, so the
        // panel stays fully usable. This stops the world-map panning when
        // the cursor is outside the panel (WantCaptureMouse would be false
        // there and let the move reach the game).
        switch (msg)
        {
        // RELEASES always pass through to the game (fall out of the switch). If we
        // swallowed them, a key/button held BEFORE the overlay opened (or held when the
        // map is quit abnormally) would never get its KEYUP → the game thinks it is held
        // forever → camera/movement stuck "à vie". ImGui was already fed above.
        case WM_KEYUP: case WM_SYSKEYUP:
        case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
            break;
        // PRESSES / moves / wheel / char are consumed so the game gets none while open.
        case WM_INPUT:
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_KEYDOWN: case WM_CHAR:
        case WM_SYSKEYDOWN:
            return (msg == WM_INPUT) ? 0 : 1;  // consume; game never sees it
        default:
            break;
        }
    }
    else if (goblin::world_map_open() && vmap_covers_map())
    {
        // Fullscreen vmap covers the native map: feed ImGui and SWALLOW the game's mouse (moves,
        // wheel, button PRESSES) so the hidden native map can't pan/zoom/select — the vmap owns the
        // pointer. Wheel is forwarded here (branch below never did), so vmap zoom works too. Keyboard
        // is NOT touched (falls through to the game) so the map-close key still closes the native map,
        // which then auto-closes the vmap — the user is never trapped. Releases pass through (feeding
        // ImGui) so nothing is "held forever".
        switch (msg)
        {
        case WM_MOUSEMOVE:
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
            return 1; // game never sees it
        case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
            break; // release passes through to the game (no "held forever")
        default:
            break; // keyboard / everything else → game (map-close key still works)
        }
    }
    // F1 panel CLOSED, native map open (and NOT a fullscreen-vmap stand-in): feed ImGui the mouse so
    // in-world chips (region toggles) stay clickable, and consume the L-button PRESS for the game ONLY
    // when the cursor is over a chip (map pan/select elsewhere is untouched). Releases always pass
    // through to the game (never swallow an UP → no "held forever" bug).
    else if (goblin::world_map_open())
    {
        switch (msg)
        {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
            if (msg == WM_LBUTTONDOWN && goblin::overlay_render_loader::call_inworld_hovered())
                return 1; // chip ate the click; the game must not pan/select
            break;
        default:
            break;
        }
    }
    return CallWindowProcW(o_orig_wndproc, hwnd, msg, wp, lp);
}
} // namespace

void install_wndproc_hook(HWND hwnd)
{
    o_orig_wndproc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&hk_wndproc)));
}

void uninstall_wndproc_hook(HWND hwnd)
{
    if (hwnd && o_orig_wndproc)
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(o_orig_wndproc));
}

unsigned diag_wm_char_exchange() { return g_diag_wm_char.exchange(0, std::memory_order_relaxed); }
unsigned diag_wm_keydown_exchange() { return g_diag_wm_keydown.exchange(0, std::memory_order_relaxed); }
unsigned wm_keydown_total() { return g_wm_keydown_total.load(std::memory_order_relaxed); }
unsigned diag_wndproc_lbdown_while_open_load() { return g_wndproc_lbdown_while_open.load(std::memory_order_relaxed); }

void mark_rpc_injection(unsigned ms)
{
    // Arm the guard window so hk_wndproc's note_user_input() ignores the WM messages our own
    // SendInject is about to generate (they land async, up to ~ms later). Extend (never shorten)
    // an already-armed window so back-to-back injections don't leave a gap.
    const ULONGLONG until = GetTickCount64() + ms;
    ULONGLONG cur = g_rpc_injection_guard_until.load(std::memory_order_relaxed);
    while (until > cur &&
           !g_rpc_injection_guard_until.compare_exchange_weak(cur, until, std::memory_order_relaxed))
        ; // cur reloaded on failure
}

unsigned long long ms_since_user_input()
{
    const ULONGLONG last = g_last_user_input_tick.load(std::memory_order_relaxed);
    if (last == 0) return ~0ull;  // no user input observed yet this session
    const ULONGLONG now = GetTickCount64();
    return now > last ? (now - last) : 0ull;
}

// [IDLEDIAG] snapshot the per-source note_user_input tally: out = {wm_input_kbd, wm_mousemove,
// legacy_key/click, guard_dropped}. Poll twice over a gap to see which source moves the clock.
void idle_diag_snapshot(unsigned out[4])
{
    for (int i = 0; i < 4; ++i) out[i] = g_note_src[i].load(std::memory_order_relaxed);
}
} // namespace goblin::input
