#pragma once
// Load / world-transition watchdog — the freeze watchdog's blind spot.
//
// goblin_freeze_watchdog watches the PRESENT beat: it fires when NO frame renders. But a stuck
// world-load keeps rendering (the loading screen animates), so present keeps beating and the
// freeze watchdog stays silent — exactly the grace-warp "infinite loading" symptom.
//
// This watchdog instead watches the world-load state directly (RE:
// docs/re/windows_loading_screen_state_re_findings.md):
//   LocalPlayer = [WorldChrMan + 0x1E508]  == null  ⇒ world not playable (load in progress)
// A warp is ARMED via arm_warp(); if LocalPlayer goes null (load started) and then stays null
// past the threshold, the load is stuck → write logs/MapForGoblins_load_stall_<pid>.txt + a full
// all-thread minidump (the frozen threads' stacks are the diagnosis) + the target grace / mapId.

#include <cstdint>
#include <filesystem>

namespace goblin::load_watchdog
{
// Start the poll thread (no-op if config load_watchdog_secs == 0). Call once at init.
void install(const std::filesystem::path &log_dir);

// A warp/teleport was just triggered (goblin::warp::to_grace) — start watching for its load to
// complete. target_grace = the BonfireWarpParam rowId (logged in the stall report as the "why").
void arm_warp(int32_t target_grace);
} // namespace goblin::load_watchdog
