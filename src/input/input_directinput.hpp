#pragma once

// DirectInput8 device hooks — extracted from goblin_overlay.cpp (see
// docs/plans/input_module_refactor_plan.md, first slice: the smallest/least-coupled hook
// group, moved first to prove the split pattern before touching the more entangled ones).

namespace goblin::input
{
// Resolve IDirectInputDevice8's vtable via a throwaway mouse device and hook
// GetDeviceState/GetDeviceData (ER's primary input path). Safe to call once, after MinHook
// is initialized. Logs its own success/failure via spdlog.
void install_directinput_hooks();

// [INPUTDIAG] read-and-reset: the GAME's own DirectInput reads this second, and how many we zeroed.
// DI is ER's PRIMARY keyboard/mouse path, so a nonzero `zeroed` with the overlay closed IS the bug.
unsigned diag_di_calls_exchange();
unsigned diag_di_zeroed_exchange();
} // namespace goblin::input
