#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Cursor hooks (SetCursorPos/ClipCursor/GetCursorPos) — extracted from goblin_overlay.cpp
// (see docs/plans/input_module_refactor_plan.md, slice 3). This is the single most
// bug-fragile area of the file (Alt+Tab cursor-lock, Proton click-swallow, virtual-cursor
// drift all lived here, each needing multiple live-tested rounds to fix) — the accessors
// below exist because hk_present calls the real trampolines and toggles
// g_imgui_reading_cursor directly, not just this module's own hook bodies.

namespace goblin::input
{
// Resolve SetCursorPos/ClipCursor/GetCursorPos from user32.dll and hook all three. Logs its
// own per-hook success/failure via spdlog (a silent failure here is the suspected cause of
// "click the search box never focuses" on some launches).
void install_cursor_hooks();

// Call the REAL (unhooked) SetCursorPos via the trampoline — used by the gamepad/keyboard
// cursor-recenter helpers so our own recentring isn't swallowed by the menu-open hook.
BOOL set_cursor_pos_real(int x, int y);

// True once install_cursor_hooks() found and hooked ClipCursor. Callers should check this
// before calling clip_cursor_real() if they need to distinguish "hook missing" from "call
// failed".
bool clip_cursor_hooked();

// Call the REAL (unhooked) ClipCursor via the trampoline — used to re-confine the cursor to
// the window on a real menu-close (see goblin_overlay.cpp's falling-edge re-clip).
BOOL clip_cursor_real(const RECT *rc);

// Call the REAL (unhooked) GetCursorPos via the trampoline, falling back to the plain OS
// GetCursorPos if the hook never installed. Used by marker-tooltip cursor tracking.
BOOL get_cursor_pos_real(LPPOINT p);

// Exempt the NEXT get_cursor_pos_real()/hooked-GetCursorPos call from the menu-open
// screen-centre fake — set true only around a read that legitimately needs the live
// position (ImGui's own NewFrame, our own mouse-poll block), false immediately after.
void set_imgui_reading_cursor(bool v);

// Diagnostic counters (docs/re/proton11_cursor_lock_re_prompt.md step 1 + dx-bugs 2026-07-01
// Alt+Tab followup). Each *_exchange() reads-and-resets (matches the original atomic.exchange(0)
// call sites in the [CURSORDIAG]/[DIAG] dumps); the rest are plain reads.
unsigned diag_set_cursor_pos_exchange();
unsigned diag_clip_cursor_exchange();
unsigned diag_get_cursor_pos_exchange();
unsigned diag_set_cursor_pos_live_exchange();
int diag_last_set_cursor_pos_x();
int diag_last_set_cursor_pos_y();
bool diag_set_cursor_pos_swallowed();
} // namespace goblin::input
