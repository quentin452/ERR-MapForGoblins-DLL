# Bug — vmap grace warp used the param ROW KEY, not bonfireEntityId (infinite loading)

**Status:** resolved 2026-07-04 (`89d0cd8`). Caught via the load watchdog + on-warp logging.

## Symptom
Double-clicking a grace on the Virtual World Map → infinite loading screen (not a crash; the load
watchdog fired: `LocalPlayer null 30s`, `last MapId 3d2a0000` = area 61 = SOTE DLC). The SAME warp via
debug-RPC (`warp <id>`) worked fine.

## Root cause
`goblin::warp::to_grace(id)` wants the bonfire **ENTITY id** — `BonfireWarpParam.bonfireEntityId`
(field @0x08, e.g. **1042362951 = The First Step**, 10002951 = Margit) — and sends `entityId - 1000`
to `LuaWarp_01`. The RPC passes that entity id directly, so it worked. But the grace LAYER built its
marker's warp id from the **param ROW KEY** (`LiveGrace.rowId`, the iterator key). **ERR remaps the
BonfireWarpParam row keys**, so The First Step's row key was `61423601` — a different number whose
`-1000` sent the warp toward **area 61 (DLC)** → world-streaming stall → infinite load.

Diagnosis was instant once the vmap logged the warp on double-click:
`[VMAP] warp queued: grace 'The First Step' rowId=61423601 area=60 …` — a base (area 60) grace with a
614xxxxx warp id was the tell.

## Fix
Capture `bonfireEntityId` into `LiveGrace` (`goblin_world_position.cpp` build loop + the struct in
`goblin_inject.hpp`) and use it — NOT the row key — as the grace marker's `row_id` (the vmap warp id)
in `grace_layer.cpp`.

## Follow-up: the warp OFFSET was also wrong (`-1000` → `0`)
After the row-key→entity-id fix, warps still landed **one bonfire off** (e.g. Agheel Lake North
`10_43_37_1950` → the CT's `-1000` sent `10_43_37_0950`, a different bonfire in the SAME map cell).
The Hexinton CT documented `LuaWarp_01 r8d = graceId - 1000`, but **ground truth (in-game 2026-07-04):
the correct value is the `bonfireEntityId` DIRECTLY (offset 0)**. `to_grace(id, offset=0)` now; the
`-1000` was a CT artifact that happened not to be checked precisely. A live "warp off" DragInt in the
vmap toolbar found it empirically.

## Guardrail
The BonfireWarpParam **row key ≠ the warp id** under a mod that remaps params (ERR does). Anything that
fast-travels must use `bonfireEntityId` DIRECTLY (no `-1000`), never the param row key. Same lesson generalizes: never assume
a param's row key equals an entity/warp id — read the dedicated field. The mod-agnostic acceptance test
(does it work under a DIFFERENT mod's params?) would have caught this. Related: [[aob-scan-boot-race]]
(a different intra-cycle vmap-adjacent fix this session).
