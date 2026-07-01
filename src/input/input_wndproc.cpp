#include "input_wndproc.hpp"
#include "input_shared.hpp"

#include <atomic>

#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <spdlog/spdlog.h>

#include "goblin_inject.hpp"          // goblin::world_map_open()
#include "worldmap/map_renderer.hpp"  // goblin::worldmap::inworld_hovered()

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
// diag: real WM_LBUTTONDOWN reaching us while the panel is open (0 => ER raw-input swallows
// legacy click messages -> poll buttons instead).
std::atomic<unsigned> g_wndproc_lbdown_while_open{0};

LRESULT CALLBACK hk_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
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
        // [KBDIAG] raw arrival count, independent of menu_open()/consumption — see the
        // g_diag_wm_char/g_diag_wm_keydown declaration comment.
        if (msg == WM_CHAR)
            g_diag_wm_char.fetch_add(1, std::memory_order_relaxed);
        else if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
            g_diag_wm_keydown.fetch_add(1, std::memory_order_relaxed);
        break;
    default:
        break;
    }

    if (menu_open())
    {
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
    // F1 panel CLOSED but the world map is open: feed ImGui the mouse so in-world chips
    // (region toggles) stay clickable, and consume the L-button PRESS for the game ONLY
    // when the cursor is over a chip (map pan/select elsewhere is untouched). Releases
    // always pass through to the game (never swallow an UP → no "held forever" bug).
    else if (goblin::world_map_open())
    {
        switch (msg)
        {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
            if (msg == WM_LBUTTONDOWN && goblin::worldmap::inworld_hovered())
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
unsigned diag_wndproc_lbdown_while_open_load() { return g_wndproc_lbdown_while_open.load(std::memory_order_relaxed); }
} // namespace goblin::input
