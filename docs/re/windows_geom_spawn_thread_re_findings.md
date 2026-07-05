# Streamer thread + per-frame injection point for ADD-AEG (pivot 2) — RE findings (static, Ghidra, 2026-07-05)

Status: **THREAD WALL SOLVED + by-id spawn request ACCEPTED, validated LIVE (2026-07-05).** The
`FUN_1406d31f0` hook fires per-frame on the registrar's own thread (same TID as the streamer, no deadlock);
the drain calls the native by-id helper `FUN_1406d4e80(p1step, aegId, worldPos)` and the engine returns a
nonzero handle with no fault. LAST GAP: the accepted request has not yet been observed to become a live
rendered geom instance (`geom_stats` flat) — the streamer state-machine build-out. See the "Live RE loop
part 2" section. Original static finding: delivers the always-per-frame hook on the registrar's OWN thread,
the engine's
native "request AEG by id at position" helper, and the cause of `FUN_1406a5080`'s thread-boundness. Answers
`windows_geom_spawn_thread_re_prompt.md`. Imagebase `0x140000000`; `query.java` on `D:\ghidra_proj2\ER`.

## ★ The per-frame task + the injection point
The world-geom per-frame update **`FUN_140623410` (er+0x623410)** is the main-update task (dispatched from the
0xaf6xxx task region; takes `float dt`, wraps it in `FD4Time`, and drives dozens of subsystems with it). Every
frame, gated only by a "world active" flag (`cVar9==0`), it calls the **request-manager update**:
```c
FUN_1406d31f0(DAT_143d69ba8, &FD4Time, param_3, ...);   // er+0x623410 line ~433
```
- `DAT_143d69ba8` (er+0x3d69ba8) = the geom-request FD4Singleton — **the same one whose `+0x30` is the reqMgr**
  (`GEOM_REQ_MGR` AOB, pivot-2). So `FUN_1406d31f0`'s `param_1` IS the request manager singleton.
- **`FUN_1406d31f0` (er+0x6d31f0)** iterates the loaded-block RB-tree (`param_1+0x20`), updates each block, and
  drives the streamer sub-object (`param_1+0x1e8` → the proximity step `FUN_140699170`) and
  `FUN_1406d6260` → `FUN_1406d4e80` → registrar `FUN_1406a5080`. It runs **every frame while in-world**, on the
  main-update thread — the SAME thread + call-context that legitimately calls the registrar.

⇒ **Injection point (deliverable 1): MinHook `FUN_1406d31f0` (er+0x6d31f0)** — set the scaffolding's
`STREAMER_STEP_RVA = 0x6d31f0`. The hook fires unconditionally per-frame in-world, on the registrar's own
thread; `hk_step` drains the deferred-spawn queue there. (Its parent `FUN_140623410` is an even-more-guaranteed
per-frame tick but is huge and doesn't hand you the reqMgr; `FUN_1406d31f0` is the tight, correct target — it
receives the reqMgr as `param_1`.)
- **AOB `FUN_1406d31f0`:** `40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 E9 48 81 EC E8 00 00`
  (the `48 81 EC E8 00 00` = `sub rsp,0xE8` was meant to disambiguate it from the near-identical
  `FUN_140623410` prologue ending `48 81 EC B8 00 00`). ABI: `char FUN_1406d31f0(reqMgr, FD4Time*, u8, u8, u8)`.
  ⚠ **This AOB is NOT usable as-is:** live on ERRv2.2.9.6 it matched **3 sites** (MULTI) and its first match
  was a different function (`er+0x3e6bb0`) — `sub rsp,0xE8` is a common frame size, so the prologue is shared.
  The impl hooks `FUN_1406d31f0` by RVA (`0x6d31f0`) instead; a longer, body-anchored unique AOB is still TODO.

## ★ The clean native spawn call (deliverable 3 — better than the raw registrar)
Two engine helpers build the `AEG###_###` name from a numeric id + resolve the block + call the registrar, so
the mod does NOT have to hand-build the name/BlockData:
- **`FUN_1406d4e80(blockStreamerState, uint aegId, float* worldPos)`** (er+0x6d4e80) — resolves the player's
  block via the RB-tree, `swprintf(L"AEG%03u_%03u", aegId/1000, aegId%1000)`, computes `worldPos - blockOrigin`,
  and calls `FUN_1406a5080(block, nameStr)`. Returns the request handle (0 = fail). AOB:
  `48 8B C4 55 57 41 56 48 8D A8 38 FF FF FF 48 81 EC B0 01 00 00`.
- **`FUN_1406d0040(blockState, uint* blockKey, uint aegId, float radius, u32, float3* posA, float3* posB)`**
  (er+0x6d0040) — same shape (`L"AEG%03u_%03u"` → registrar), for the keyed-block variant.
Called from within `FUN_1406d31f0`'s subtree (`FUN_1406d6260`→`FUN_1406d4e80`), so at hook time (inside the
`FUN_1406d31f0` hook) both the reqMgr and the block state are the live, correct-thread objects. The servicer can
either call `FUN_1406d4e80(state, aegId, pos)` (cleanest) or the raw `FUN_1406a5080(reqMgr, nameStr)`.

## Why the registrar is thread-bound (deliverable / Q3) — context, not TLS
`FUN_1406a5080` (er+0x6a5080) mutates the reqMgr's request RB-tree (`mgr+0x318`, insert via `FUN_14069e660`/
`FUN_1406a0270`) and transitions the new request's state machine (`FUN_1406c6050(req, 4)`) — all **single-writer
state owned by the `FUN_1406d31f0` per-frame update**, with no cross-thread lock. It is **main-update-context-
bound** (the streamer assumes it is the sole writer, running synchronously inside the world-geom task), which is
why:
- present/RPC thread → **deadlock** (lock inversion on `mgr+0x318` vs the streamer holding it), and
- a worker `std::thread` → **fault** (touches block/reqMgr state mid-update from the wrong context).
It is NOT a TLS slot (no `gs:`-relative / thread-local access seen) — so ANY function on the `FUN_1406d31f0`
call stack is a safe caller. Hooking `FUN_1406d31f0` puts our drain exactly there.

## Legit registrar callers (context confirmation)
`FUN_1406a5080` is called from the streamer `FUN_140699d80` (proximity path), the by-id helpers
`FUN_1406d0040`/`FUN_1406d4e80`, plus `FUN_1401dc870` / `FUN_14039cc40` — all reached under the per-frame
world-geom update on the main thread. None run off that thread, confirming the boundness.

## Acceptance (hand back to the Linux/game-driving side)
Set `STREAMER_STEP_RVA = 0x6d31f0` (hook `FUN_1406d31f0`), boot, `spawn_asset AEG###_###` → the log shows
`hk_step FIRED` every frame + `step serviced ok=true req=<nonzero>` (the drain called `FUN_1406d4e80`/
`FUN_1406a5080` on the right thread, no deadlock/fault) → walk to the spot; the asset spawns + renders (walk-on
for collision). That closes runtime ADD-AEG.

### Live acceptance run — 2026-07-05 (Linux/Proton, ERRv2.2.9.6). Thread wall PASSED; registrar arg-context is the new wall.
Implemented in `src/goblin_geom_spawn.cpp` (`STREAMER_STEP_RVA = 0x6d31f0`, `spawn_asset` queues, `hk_step`
detour drains). Cold-boot `mfg run --boot`, in-world, `spawn_asset AEG099_090` / `AEG099_510`:
- ✅ `[SPAWNASSET] hk_step FIRED (first) — main-update thread reached` — the `FUN_1406d31f0` detour fires
  per-frame on the registrar's own thread. **No deadlock, no freeze** — the game ran normally to kill. The
  thread wall (present=deadlock / worker=fault) is genuinely passable from this hook.
- ❌ `[SPAWNASSET] step serviced -> ok=false req=0x0` — the raw `FUN_1406a5080(reqMgr, L"AEG###_###")` call
  **FAULTS** (SEH-caught, so no crash; both queued items faulted the same way). ok=false = access violation, NOT
  the graceful req=0 "unknown asset" gate. So the raw registrar needs more than `(reqMgr, name)`: the
  block-resolved context its real callers (`FUN_1406d6260`→`FUN_1406d4e80`) build first.
- **NEXT wall:** route the drain through the by-id helper **`FUN_1406d4e80(blockStreamerState, aegId, worldPos)`**
  (deliverable 3), which resolves the player's block + computes `worldPos - blockOrigin` before calling the
  registrar.

### Live RE loop 2026-07-05 (part 2) — by-id helper drain WORKS; the request→instance servicing is the last gap
Diagnosed the raw-registrar fault by capturing what the ENGINE does, then reproducing it:
- **Thread ruled out.** Logged `GetCurrentThreadId()` in both our `FUN_1406d31f0` drain and a capture hook on
  the registrar `FUN_1406a5080`. The streamer's legit registrar calls AND our drain are **the same TID (352 /
  340 across boots)**. So the `FUN_1406d31f0` hook IS on the registrar's own thread — the thread wall is
  genuinely passed. (present=deadlock / worker=fault still hold; this hook is neither.)
- **Asset name ruled out.** Captured legit registrar calls near First Step: names `AEG095_002 / AEG099_204 /
  AEG095_003 / AEG099_20x`, param_1 usually `== reqMgr` (`[singleton+0x30]`), sometimes a per-block mgr
  (`reqMgr ± offset`). Replaying those EXACT names through the raw registrar on the same thread STILL faults
  (`ok=false`). So the raw `FUN_1406a5080` is **not a valid cold entry** regardless of name/param_1 — it
  assumes per-call block/desc context its callers set up.
- **✅ The by-id helper works.** `FUN_1406d4e80(state, aegId, worldPos)` with **`state` = `FUN_1406d31f0`'s
  param_1** (the value passed to our `hk_step`), `aegId` parsed from the name (`AEG095_002`→95002),
  `worldPos` = `get_player_world_pos`: returns **`ok=true`, nonzero handle**, no fault, for every name
  (incl. `AEG099_090`). Tried 5 candidate `state` pointers; `p1step` is accepted on the first try. This is the
  correct native ADD-AEG entry.  → implemented as the drain in `goblin_geom_spawn.cpp`.
- **⏳ OPEN — request ≠ rendered instance yet.** After 9 by-id spawns, `geom_stats` total is **unchanged**
  (16130→16130, identical class histogram). The request is accepted but does not (yet) materialize as a live
  counted `CSWorldGeom` instance, and nothing new is visible on screen (spawns land at the player's exact feet
  too). The request→instance build-out is the streamer state machine (`FUN_1406c6050(req,4)` → proximity step
  `FUN_140699170`, which earlier RE flagged as gated). NEXT: advance/drive that state transition (or confirm
  whether the instance exists in an un-enumerated class / wrong block-local position from a world↔block frame
  mismatch in `worldPos`).

### Two implementation bugs fixed en route (both would silently no-op the hook)

### Two implementation bugs fixed en route (both would silently no-op the hook)
1. **The `FUN_1406d31f0` prologue AOB above is NOT unique** — live health check = MULTI (3 matches) and its
   first match is a DIFFERENT function (`er+0x3e6bb0`), so AOB-first `resolve_func_aob` hooked the wrong
   function and the queue never drained. Fix: hook by RVA directly (correct for this build), NO AOB in the
   health table. A longer, unique AOB is future hardening (`rva_aob_hardening_backlog.md`).
2. **Lazy hook install must use `modutils::hook_now`** (immediate `MH_EnableHook`), not `modutils::hook`
   (`MH_QueueEnableHook`, applied only by the init-time `MH_ApplyQueued`/`enable_hooks()` that already ran).
   With `hook()` the detour is created but never armed → hooked-but-never-fires.

## Anchors (er-relative, imagebase 0x140000000)
- **Injection hook `FUN_1406d31f0` er+0x6d31f0** (reqMgr per-frame update; AOB above). Parent task
  `FUN_140623410` er+0x623410 (world-geom per-frame update; calls it with `DAT_143d69ba8`).
- Native by-id spawn helpers `FUN_1406d4e80` er+0x6d4e80, `FUN_1406d0040` er+0x6d0040 (both build
  `L"AEG%03u_%03u"` → `FUN_1406a5080`).
- registrar `FUN_1406a5080` er+0x6a5080; state machine `FUN_1406c6050` er+0x6c6050; RB-tree insert
  `FUN_14069e660`/`FUN_1406a0270`; reqMgr = `*(DAT_143d69ba8 + 0x30)` (er+0x3d69ba8, `GEOM_REQ_MGR` pinned).
- streamer `FUN_140699d80` er+0x699d80 (the proximity registrar caller); proximity step `FUN_140699170`
  er+0x699170 (called from `FUN_1406d31f0` via `param_1+0x1e8`).
