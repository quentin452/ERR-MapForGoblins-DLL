#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Xinput.h>

// XInputGetState hook — extracted from goblin_overlay.cpp (see
// docs/plans/input_module_refactor_plan.md, slice 2). Unlike DirectInput/mouse/keyboard,
// XInput is polled every frame (no window messages), so hk_present's own per-frame gamepad
// poll needs to call the REAL trampoline directly — that's why this module exposes
// xinput_get_state_real() in addition to the install function, instead of hiding
// o_xinput_get_state as purely hook-internal.

namespace goblin::input
{
// Resolve XInputGetState dynamically (xinput1_4 -> xinput1_3 -> xinput9_1_0, no static link
// dependency) and hook it so the game gets a connected-but-zeroed Gamepad while the menu is
// open. Sets xinput_available() true on success. Logs its own success/failure via spdlog.
void install_xinput_hook();

// True once install_xinput_hook() found a usable XInput DLL and hooked it successfully.
bool xinput_available();

// Call the REAL (unhooked) XInputGetState via the trampoline — for our own polling (e.g.
// hk_present's gamepad-toggle/recenter poll) that needs the actual controller state even
// while the menu is open, bypassing the zeroing hk_xinput_get_state applies to outside
// callers. Only valid after xinput_available() is true.
DWORD xinput_get_state_real(DWORD user_index, XINPUT_STATE *state);
} // namespace goblin::input
