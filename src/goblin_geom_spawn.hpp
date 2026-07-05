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
    // THREAD WALL (2026-07-04): the direct call off the engine's update thread DEADLOCKS on the present
    // thread (lock inversion vs the streamer over reqMgr+0x318) and FAULTS on a worker thread — the
    // registrar is main-update-CONTEXT-bound. SOLVED (2026-07-05, windows_geom_spawn_thread_re_findings.md):
    // `force=false` (default) QUEUES the request; a lazy `hook_now` detour on the reqMgr per-frame update
    // FUN_1406d31f0 (STREAMER_STEP_RVA=0x6d31f0) drains it on the game's own main-update thread — the
    // registrar's own context. Validated live: the drain runs there with no deadlock/freeze.
    //   ⚠ OPEN: the raw FUN_1406a5080(reqMgr, name) drain still FAULTS (SEH-caught) — the registrar needs the
    //   block-resolved context its callers build; route through FUN_1406d4e80(state, aegId, worldPos) next.
    //   `force=true` fires the direct FUN_1406a5080 on the caller thread (WILL hang — diagnostic only).
    // See docs/re/windows_geom_spawn_thread_re_findings.md.
    SpawnResult spawn_asset(const char *aegName, bool force = false);
}
