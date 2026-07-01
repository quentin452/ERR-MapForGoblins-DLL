#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Raw input hooks (GetRawInputData/GetRawInputBuffer) — extracted from goblin_overlay.cpp
// (see docs/plans/input_module_refactor_plan.md, slice 4). ER reads gameplay keyboard/mouse
// through these (not window messages), so they're neutralised while the menu is open; the
// mouse deltas are also accumulated into a virtual cursor (see accumulate_virtual_cursor's
// comment in the .cpp) kept for the [DIAG] on-screen readout.

namespace goblin::input
{
// Resolve GetRawInputData/GetRawInputBuffer from user32.dll and hook both. Logs its own
// per-hook success/failure via spdlog.
void install_rawinput_hooks();

// Virtual cursor accumulated from raw mouse deltas — read by the [DIAG] on-screen overlay
// (config::debugCursorDiagnostic). Kept only as a comparison value; the real cursor tracking
// uses the exemption in input_cursor.cpp's hk_get_cursor_pos, not this.
float virtual_cursor_x();
float virtual_cursor_y();

// Diagnostic counters (docs/re/proton11_cursor_lock_re_prompt.md step 1). Read-and-reset,
// matching the original atomic.exchange(0) call sites in the [CURSORDIAG] dump.
unsigned diag_get_raw_input_data_exchange();
unsigned diag_get_raw_input_buffer_exchange();

// Mouse-wheel delta accumulated (in ImGui units: raw usButtonData / WHEEL_DELTA) from the
// raw-input hooks while the menu is open, drained once per frame by hk_present into
// io.AddMouseWheelEvent. The wheel has NO pollable async state (unlike buttons/keys) and
// ER's RIDEV_NOLEGACY means WM_MOUSEWHEEL never reaches the wndproc — the blanked raw
// event here is the only place the delta exists at all.
float take_wheel_delta();
} // namespace goblin::input
