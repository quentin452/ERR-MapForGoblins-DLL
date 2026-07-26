#pragma once

#include <cstdint>
#include <filesystem>
#include <utility>

namespace goblin
{
// Install a last-resort unhandled-exception filter that writes a minidump
// (.dmp) to `dump_dir` when the process crashes. Works under Proton/Wine
// because it runs in-process via dbghelp's MiniDumpWriteDump (Wine has no
// functional WER LocalDumps). The dump is a triage dump (thread stacks +
// module list + faulting context, no heap) — exactly what
// docs/crash_dump_diagnostics.md parses. Safe to call more than once;
// each install chains to the previously-registered filter.
void install_crash_handler(const std::filesystem::path &dump_dir);

// [base, end) of MapForGoblins.dll's own image, captured once at
// install_crash_handler() time. {0, 0} if called before install or the
// self-lookup failed. Lets other code (e.g. the XInput swallow hook) tell
// whether a return address belongs to us vs. the host process.
std::pair<uintptr_t, uintptr_t> self_module_range();

// [base, end) of the HOST GAME executable's image. Resolved name-agnostically
// (GetModuleHandleW(nullptr)) so it also works before install_crash_handler()
// and on a non-ER host. {0, 0} only if even that lookup fails.
std::pair<uintptr_t, uintptr_t> game_module_range();

// True when `ret_addr` (a _ReturnAddress()) lies inside the host game executable.
// The input hooks falsify/blank device data ONLY for the GAME's own reads: other
// overlay mods in the same process (EROverlay.dll, ...) poll the very same APIs
// (GetRawInputData/Buffer, DirectInput8 GetDeviceState/Data, Get/SetCursorPos) and
// must keep seeing REAL input — without this gate, opening our F1 panel silently
// deadens every other mod's UI. Fails OPEN (returns true) if the host range is
// unknown, i.e. degrades to the historical blank-everyone behaviour rather than
// letting the game keep moving under an open panel.
bool caller_is_game(const void *ret_addr);
} // namespace goblin
