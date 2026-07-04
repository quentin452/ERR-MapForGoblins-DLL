#pragma once

// Live ADD-a-geom probe — pivot 2 (asset streaming-REQUEST path).
//
// Static RE complete (docs/re/windows_geom_spawn_pivot2_re_findings.md): request an EXISTING AEG asset by
// name and the streamer builds+places+tracks a real CSWorldGeom instance (renders; collision follows the
// standard world-geom path). This is the streaming-native ADD route (no standalone-ctor hang like the
// dead-end direct-ctor path). Chain:
//   reqMgr = *(void**)(DAT_143d69ba8 /*FD4Singleton, GEOM_REQ_MGR AOB*/ + 0x30)
//   req    = FUN_1406a5080(reqMgr, L"AEG###_###")   // registers the request; streamer services state 4
// Dev/RE tool behind the `spawn_asset <AEGname>` RPC. Present-thread only (drives streaming/scene).

#include <cstdint>

namespace goblin::geom_spawn
{
    struct SpawnResult
    {
        bool ok = false;
        uint64_t singleton = 0;  // DAT_143d69ba8 (the FD4Singleton) resolved from the AOB
        uint64_t reqMgr = 0;     // *(singleton+0x30) = FUN_1406a5080's param_1
        uint64_t req = 0;        // FUN_1406a5080 return (the request / CSWorldGeom instance), 0 on gate/fail
        char name[32] = {};      // requested AEG name (as passed)
        char err[128] = {};      // failure reason (ok=false)
    };

    // Resolve the request manager (AOB → FD4Singleton → +0x30). Fills singleton+reqMgr; ok on success.
    // A read-only sanity step (no request), so a driver can confirm the chain resolves before spawning.
    SpawnResult resolve_req_mgr();

    // Request an existing AEG asset by name (e.g. "AEG099_090") via FUN_1406a5080(reqMgr, wname).
    //
    // ⚠ LIVE PROBE RESULT 2026-07-04: the direct call DEADLOCKS the game (freeze watchdog fires; present
    // beat frozen, worker threads alive). Our RPC handler runs on the PRESENT thread (pump()), yet the
    // registrar's RB-tree insert into the live reqMgr tree (mgr+0x318) contends with the streamer thread →
    // lock inversion → deadlock. So it is NOT safe to call standalone off the engine's native update thread
    // (the static "streaming-friendly, no hang" claim held for the algorithm, not for an off-thread call).
    // ⇒ `force=false` (default) does the read-only reqMgr resolve ONLY and reports the dead end — no call.
    //   `force=true` actually fires FUN_1406a5080 (WILL hang; kept only for a future main-update-thread hook
    //   or an in-dump diagnostic). See docs/re/windows_geom_spawn_pivot2_re_findings.md.
    SpawnResult spawn_asset(const char *aegName, bool force = false);
}
