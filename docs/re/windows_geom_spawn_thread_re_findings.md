# Streamer thread + per-frame injection point for ADD-AEG (pivot 2) — RE findings (static, Ghidra, 2026-07-05)

Status: **SOLVED (static).** Delivers the always-per-frame hook on the registrar's OWN thread, the engine's
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
  (the `48 81 EC E8 00 00` = `sub rsp,0xE8` disambiguates it from the near-identical `FUN_140623410` prologue,
  which ends `48 81 EC B8 00 00`). ABI: `char FUN_1406d31f0(reqMgr, FD4Time*, u8, u8, u8)`.

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

## Anchors (er-relative, imagebase 0x140000000)
- **Injection hook `FUN_1406d31f0` er+0x6d31f0** (reqMgr per-frame update; AOB above). Parent task
  `FUN_140623410` er+0x623410 (world-geom per-frame update; calls it with `DAT_143d69ba8`).
- Native by-id spawn helpers `FUN_1406d4e80` er+0x6d4e80, `FUN_1406d0040` er+0x6d0040 (both build
  `L"AEG%03u_%03u"` → `FUN_1406a5080`).
- registrar `FUN_1406a5080` er+0x6a5080; state machine `FUN_1406c6050` er+0x6c6050; RB-tree insert
  `FUN_14069e660`/`FUN_1406a0270`; reqMgr = `*(DAT_143d69ba8 + 0x30)` (er+0x3d69ba8, `GEOM_REQ_MGR` pinned).
- streamer `FUN_140699d80` er+0x699d80 (the proximity registrar caller); proximity step `FUN_140699170`
  er+0x699170 (called from `FUN_1406d31f0` via `param_1+0x1e8`).
