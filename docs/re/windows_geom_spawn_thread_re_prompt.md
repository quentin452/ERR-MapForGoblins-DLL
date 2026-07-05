# Windows-Ghidra RE prompt — the streamer thread + a per-frame injection point for ADD-AEG (pivot 2)

## Why (the one wall left for runtime ADD-AEG)
Pivot 2 (`windows_geom_spawn_pivot2_re_findings.md`) is fully RE'd and the reqMgr resolves live, but the
**request registrar `FUN_1406a5080` (er+0x6a5080)** cannot be called from where the mod runs:
- **present/RPC thread → DEADLOCK** (lock inversion with the streamer on the reqMgr RB-tree, `mgr+0x318`).
- **a dedicated worker `std::thread` → NO deadlock but the registrar FAULTS** → `FUN_1406a5080` is
  **game-thread-bound** (thread-local / main-update-only state), not merely "any non-present thread".
- **hooking the two proximity-streamer steps `FUN_140699170` / `FUN_14069a550` → they NEVER FIRE** in an
  open-field scene (proximity-gated, not reliable per-frame hooks).

So the mod has a **deferred-spawn queue ready** (`goblin_geom_spawn.cpp`: `spawn_asset` queues; a servicer
drains + calls the registrar) — it just needs a servicer that runs **on the game's MAIN-UPDATE thread AND
fires every frame**. That injection point is the deliverable.

## Deliverable
One of:
1. **A reliable ALWAYS-per-frame function on the streamer's thread** that MFG can MinHook (RVA + AOB, per repo
   doctrine) — so `hk_step` drains the queue there (the scaffolding already does this; just swap
   `STREAMER_STEP_RVA`). It must fire unconditionally each frame (unlike the proximity steps), and run on the
   SAME thread that legitimately calls `FUN_1406a5080`.
2. **OR** a confirmation that a specific already-known per-frame tick (below) is on that thread + safe, with the
   exact hook point.
3. **OR** a safer engine-exposed deferral: a per-frame "pending request" list the streamer drains itself (does
   the reqMgr have an input queue we can append to off-thread, instead of calling the registrar directly?), or
   a flag/param that makes the proximity streamer request OUR `AEG###_###` name.

## Questions
1. **Which thread runs the streamer / the reqMgr service?** Trace who executes `FUN_140699670`/`FUN_14069a9b0`
   (the proximity AEG streamer) and `FUN_1406c6050` (the request state machine) — which task-graph node /
   `CSEzTask`/`CSEzUpdateTask` owns them, and on which worker/main thread does that task run? (The task system
   is the same one behind `CSSystemStep` `FUN_140ded060` and the menu tick `FUN_140766980` er+0x766980.)
2. **A per-frame, non-gated function on that thread.** The proximity steps gate on nearby assets. What runs
   EVERY frame on that thread regardless — the top-level `CSSystemStep` execute, the `CSWorldGeom`/world update
   tick, or the task dispatcher for that thread? Give its RVA + a unique AOB + the ABI (arg count) so the mod
   can hook it and drain the queue.
3. **Is the registrar's thread-boundness a lock or true TLS/context?** Confirm what `FUN_1406a5080` touches
   that faults off-thread (a TLS slot? a per-thread allocator? `mgr`-relative state that assumes the owner
   thread?). If it's only the RB-tree lock, a different safe entry may exist; if it's TLS, only the owner
   thread works.
4. **Any engine-native deferral?** Does the reqMgr expose an append-a-request-by-name API that's safe off the
   owner thread (the streamer then services it), avoiding a direct `FUN_1406a5080` call entirely? (Look around
   `FUN_1406c6050` state 4 / the RB-tree insert for an alternate producer.)

## Anchors (from the pivot-2 findings)
- registrar `FUN_1406a5080` er+0x6a5080; state machine `FUN_1406c6050` er+0x6c6050; block-registry
  `FUN_1406a6630`. reqMgr = `*(DAT_143d69ba8 + 0x30)` (er+0x3d69ba8, FD4Singleton;
  AOB `GEOM_REQ_MGR` er+0x1dcc53 already pinned).
- streamer `FUN_140699670` er+0x699670; caller `FUN_14069a9b0` er+0x69a9b0; steps `FUN_140699170` er+0x699170,
  `FUN_14069a550` er+0x69a550 (both proximity-gated — DON'T fire reliably).
- task/step system: `CSSystemStep` ctor `FUN_140ded060`; menu update tick `FUN_140766980` er+0x766980.

## Acceptance (handed back to the Linux/game-driving side)
The mod sets `STREAMER_STEP_RVA` to the delivered target, boots, `spawn_asset AEG###_###` → the log shows
`hk_step FIRED` (the hook runs every frame) + `step serviced ok=true req=<nonzero>` (the registrar ran on the
right thread, no deadlock/fault) → then walk to the requested world spot to confirm the asset spawned +
renders (+ walk-on for collision). That closes runtime ADD-AEG.
