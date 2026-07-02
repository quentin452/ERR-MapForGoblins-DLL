#pragma once

#include <filesystem>

// Deadlock/freeze watchdog (user ask 2026-07-02): the game can freeze "deadlock-like" with NO
// exception — last log is a normal render line, no crash dump, window solid, but the DLL's own
// threads (RPC listener, workers) stay alive. The crash handler never fires for those. This
// watchdog runs on its own (healthy) thread, watches a present-thread heartbeat, and when the
// heartbeat stalls past the configured threshold writes a freeze triage .txt + a full minidump
// (all thread stacks — captured FROM the healthy thread, which works precisely because the
// frozen threads aren't running) so the deadlock becomes a symbolizable stack instead of a
// mystery. Read-only observer otherwise; ini `[Debug] freeze_watchdog_secs` (0 = off).
namespace goblin::freeze_watchdog
{
// Call once per rendered frame (top of hk_present). Lock-free, safe from any thread.
void beat_present();

// Start the watchdog thread. No-op when freeze_watchdog_secs == 0. `log_dir` receives
// MapForGoblins_freeze_<pid>.{txt,dmp} on a detected stall (one dump per session).
void install(const std::filesystem::path &log_dir);
} // namespace goblin::freeze_watchdog
