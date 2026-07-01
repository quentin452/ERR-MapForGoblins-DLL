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
} // namespace goblin
