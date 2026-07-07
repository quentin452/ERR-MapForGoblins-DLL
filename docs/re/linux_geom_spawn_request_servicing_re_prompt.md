# Runtime ADD-AEG — make the accepted request become a RENDERED instance (last gap) — LIVE RE prompt (Linux/Proton)

Status: **OPEN.** Follow-up of `windows_geom_spawn_thread_re_findings.md` ("Live RE loop part 2") — the
final brick of the MSB-write ADD path (RE README frontier #1). Everything up to "the engine accepts our
spawn request" is DONE and live-validated; this prompt is the live instrumentation + diagnosis to turn the
accepted request into a visible `CSWorldGeom` instance. Findings go to
`docs/re/linux_geom_spawn_request_servicing_re_findings.md` (supersedes this prompt when written).

## Context — what is already PROVEN (do not re-derive)

- **Thread wall PASSED.** `goblin_geom_spawn.cpp` hooks the reqMgr per-frame update
  **`FUN_1406d31f0` (er+0x6d31f0)** (`STREAMER_STEP_RVA = 0x6d31f0`); the `hk_step` detour fires every
  frame in-world **on the registrar's own main-update thread** (same TID as the streamer's legit registrar
  calls, proven live). No deadlock, no freeze.
- **The correct native entry is the by-id helper `FUN_1406d4e80(state, aegId, worldPos)`** (er+0x6d4e80),
  with `state` = `FUN_1406d31f0`'s `param_1` (the value handed to `hk_step`), `aegId` = numeric
  (`AEG095_002` → 95002), `worldPos` = world-frame float3. It returns **`ok=true` + a nonzero handle, no
  fault**, for every name tried (incl. `AEG099_090`). It builds `L"AEG%03u_%03u"`, resolves the player's
  block via the RB-tree, computes `worldPos - blockOrigin`, and calls the registrar itself.
- **The raw registrar `FUN_1406a5080(reqMgr, name)` is NOT a valid cold entry** — it faults even on the
  correct thread with streamer-captured names. Don't go back to it.
- **The direct-ctor route (`FUN_1406b9880` + `thunk_FUN_144cbdae7`) is a DEAD END** — the pose builder
  hangs the game standalone (`windows_geom_spawn_builder_re_findings.md`). Don't retry it.

## The GAP (what this session must answer)

After 9 accepted by-id spawns: **`geom_stats` total unchanged (16130→16130, identical class histogram),
nothing visible in-world.** The request is registered but never serviced into a live rendered instance.

Known servicing machinery (static, `windows_geom_spawn_pivot2_re_findings.md`): the registrar allocates a
real geom request/instance container via `FUN_1406c7000` (er+0x6c7000) and sets **state 4** via the request
state machine **`FUN_1406c6050(req, state)`** (er+0x6c6050; flips resident/loading bits at `req+0x4d`,
registers via `FUN_1406a6630`). State-4 service path: `FUN_1406c9c30` / `FUN_1406c8750` / `FUN_1406e38c0`;
request state flags at `req+0x4d`, `+0x264`, `+0x268`; render sub-object at `+0x6f`. The proximity step
`FUN_140699170` (er+0x699170) was observed **gated** (a detour on it never fired at Church of Elleh).

### Hypotheses, in test order
- **H1 — the request stalls in a state.** Our request enters the RB-tree but some per-request gate
  (residency? proximity? a flag the legit callers set that `FUN_1406d4e80` does not) never lets it advance.
- **H2 — the instance EXISTS but we can't see it.** It spawns at the player's exact feet (inside the
  player model) and/or lives in a class/pool `geom_stats` doesn't enumerate.
- **H3 — `worldPos` frame mismatch.** `FUN_1406d4e80` subtracts `blockOrigin` itself, so the arg must be
  WORLD frame; confirm `get_player_world_pos` matches the frame the streamer's own callers pass.

## Instrumentation to BUILD (extend `goblin_geom_spawn.cpp`, host-side — no host↔render boundary)

1. **Request-state dump (`spawn_reqdump`).** Remember the last N handles returned by the drain. Each
   `hk_step`, read (SEH-wrapped, read-only) `req+0x00` (vtable → log the RVA and whether it is
   `CSWorldGeomDynamicIns` er+0x2a84208 / the request container class), the state bytes `+0x4d`,
   `+0x264`, `+0x268`, and `+0x6f`. Log ONLY on change (edge-detect) to keep the log readable. RPC verb
   `spawn_reqdump` prints the current snapshot of all tracked handles.
2. **State-machine capture (`spawn_capstate 1|0`).** A read-only capture hook on `FUN_1406c6050`
   (er+0x6c6050, **hook by RVA, not AOB** — see gotchas) logging `(req, state, retaddr-RVA, TID)` into a
   small ring buffer, dumped via the verb. Purpose: record the state SEQUENCE of a legit streamer request
   (walk/warp so the world streams new AEGs) vs OUR request, and diff where ours diverges/stops.
3. **Offset spawn + look.** Extend `spawn_asset <AEGname> [dx dy dz]` to add a world-frame offset to the
   player pos (default keeps current behaviour). Pair with `r3d box <x> <y> <z>` at the same target so the
   spot is visible even if the asset isn't.
4. *(only if H2 suspected)* **Enumeration cross-check.** After a spawn, walk the source BlockData's
   record registry (`rec+0xe8` slots / `+0xf8` cursor) and/or the reqMgr RB-tree (`mgr+0x318`,
   reqMgr = `*(DAT_143d69ba8 + 0x30)`) to see whether a NEW entry appeared that `geom_stats` misses.

New RPC verbs → document in `docs/memory/tooling/rpc-commands.md`. If you add a `tools/rpc_tests/test_*.py`,
also add it to the `rpcTest` pickString in `.vscode/tasks.json`, and regenerate+commit `STATUS.md`
(`python tools/rpc_tests/check_regress.py`) before finishing.

## Live protocol (needs ER booted once)

1. Build + deploy BOTH builds green (`build-linux`; run the hot-reload link check only if you touch the
   boundary — this work is host-only so normally not needed). Boot: `python tools/mfg.py repl --boot`
   (profile is `err_offline.me3` — do NOT override `MFG_PROFILE`). **Freshness first:** `mfg_build` must
   show your build stamp, `status` must show in-world (not menu) — `ping` answers from a stale DLL too.
2. `spawn_capstate 1` → warp between two graces (or walk a stretch) to stream legit AEGs → dump the ring:
   record the legit state sequence + which retaddr-RVAs drive each transition.
3. `spawn_asset AEG099_090 3 0 0` (3 m ahead) + `r3d box` at the same spot → `spawn_reqdump` over the next
   seconds: where does OUR request's state stop vs the legit sequence?
4. Decide by hypothesis:
   - **H1 confirmed (stall state identified):** try driving the missing transition live — e.g. call
     `FUN_1406c6050(req, <next-state>)` from `hk_step` (SEH-wrapped, one request only), or replicate the
     flag the legit path sets. If the gate is inside the state-4 service subtree and not understandable
     live, STOP and write a targeted Windows/Ghidra prompt (name the exact gate fn + the flag) instead of
     blind pokes.
   - **H2 confirmed (new entry exists):** fix `geom_stats` enumeration / look at the r3d-marked spot;
     if the asset renders offset-spawned, ADD is CLOSED — write findings + changelog.
   - **H3 confirmed (frame mismatch):** log the `worldPos` a legit `FUN_1406d4e80` call receives
     (capture hook on er+0x6d4e80 — `arm_byid_capture` exists but that helper was NOT observed firing on
     normal streaming; if it still doesn't fire, compare against the registrar-level capture
     `spawn_capreg` + the block origin) and convert ours accordingly.
5. Acceptance = **a requested AEG visibly renders at the offset spot** (walk-on for collision is a bonus)
   with no crash over ~2 min after. That closes runtime ADD-AEG end-to-end.

## Gotchas (all bit us already — read before coding)

- **Hook by RVA, not the findings' AOBs.** The `FUN_1406d31f0` prologue AOB matches 3 sites (first = a
  WRONG function); assume the same for neighbours like `FUN_1406c6050`. RVA-direct is correct for this
  build; unique AOBs are backlog hardening (`rva_aob_hardening_backlog.md`).
- **Lazy hook install must use `modutils::hook_now`** (immediate `MH_EnableHook`) — `modutils::hook`
  queues for the init-time apply that already ran → detour never fires.
- **SEH-wrap every engine read/call**; poke one thing at a time; never alias source-owned sub-objects
  (the move-init steals from `param_4` — proven corruption vector).
- **NO full-address-space `/proc/mem` scans** (D-state freeze incident, 2026-07-06c rule): scan-heavy RE
  belongs to the Windows box; in-DLL probes + capped rw-p heap scans only.
- **Liveness:** gate on `status` `frame=<N>` advancing, not `ping` (listener survives a frozen main
  thread). The game can also fail to reap — check for a pre-existing D-state `eldenring.exe` husk before
  cold-booting.
- All RVAs below are ERR-build (imagebase `0x140000000`); struct offsets are patch-stable, RVAs are not.

## Anchors (er-relative)

| What | Where |
|------|-------|
| Injection hook (per-frame reqMgr update; `hk_step`) | `FUN_1406d31f0` er+0x6d31f0 (param_1 = `DAT_143d69ba8` singleton) |
| By-id spawn helper (the working entry) | `FUN_1406d4e80` er+0x6d4e80; keyed-block variant `FUN_1406d0040` er+0x6d0040 |
| Registrar (NOT cold-callable) | `FUN_1406a5080` er+0x6a5080 |
| Request state machine (capture target) | `FUN_1406c6050` er+0x6c6050; state bits `req+0x4d/+0x264/+0x268`, render sub `+0x6f` |
| State-4 service subtree | `FUN_1406c9c30` / `FUN_1406c8750` / `FUN_1406e38c0` |
| Request/instance allocator | `FUN_1406c7000` er+0x6c7000 |
| Proximity step (observed gated) | `FUN_140699170` er+0x699170; proximity registrar caller `FUN_140699d80` er+0x699d80 |
| reqMgr | `*(DAT_143d69ba8 + 0x30)` (er+0x3d69ba8, `GEOM_REQ_MGR` pinned); request RB-tree `mgr+0x318` |
| Block-registry (per-record instance list) | `rec+0xe8` slots / `+0xf0` free / `+0xf8` cursor / `+0xfc` cap; register fn `FUN_1406a6630` |
| Live instance layout (for vtable ID) | `CSWorldGeomDynamicIns` vt er+0x2a84208; `CSWorldGeomIns` vt er+0x2a84cb0; world matrix `inst+0x220` |
| Existing scaffolding | `src/goblin_geom_spawn.{hpp,cpp}` (`spawn_asset`, `hk_step`, `spawn_capreg`, `spawn_cap4e80`); viz `r3d box` |

## Deliverables

1. `docs/re/linux_geom_spawn_request_servicing_re_findings.md` — the state-sequence diff, which hypothesis
   held, and either "ADD CLOSED (recipe)" or the named gate + a targeted Windows/Ghidra prompt.
2. The instrumentation committed (new verbs in `rpc-commands.md`; `STATUS.md` regenerated if tests ran).
3. On close: update `docs/re/README.md` frontier #1, `docs/HANDOFF.md` ADD-AEG entry, and
   `docs/changelog.md` `[Unreleased]` (feature: runtime ADD-AEG) — this is the last brick of the MSB-write
   wall for geometry placement.
