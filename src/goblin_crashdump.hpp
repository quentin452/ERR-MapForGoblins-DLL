#pragma once

#include <filesystem>

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
} // namespace goblin
