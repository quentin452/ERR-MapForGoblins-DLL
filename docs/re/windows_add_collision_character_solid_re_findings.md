# add_collision — making the injected box CHARACTER-solid (not just raycast-hit)

**Status:** root-caused in Ghidra + live-instrumented on 2.6.2.0 (2026-07-08). Fix shipped in
`goblin_add_collision.cpp` (`add_box` stamps `collisionFilterInfo` before `addBody`); **live walk/fall
confirm pending a game restart** (host DLL change).

## Problem

`add_collision` injects a real `hknpBoxShape` static body into the live world. It is broadphase-present —
the mod's own down-ray (`hf_probe_present`, engine cast `FUN_140c70360`, filter `0x5e`) hits the box top
exactly. **But the character controller falls straight through it** (verified: warp player up, drop onto
the box → rests at real-ground Y, decelerating nowhere near the box top). So the box was *ray-collidable
but not character-solid* — the multi-session "pillar isn't walkable" blocker.

## Root cause — collision-layer filter, cached at addBody

ER's collision gating is a **128-layer group filter**, `CS::CSCollisionFilter` (vtable `er+0x2b91d00`,
ctor `er+0xc5d730`): its ctor zero-inits a **0x800-byte matrix at `filter+0x20`** (128 layers × 128 bits).
The decision fns `isCollisionEnabled` (`er+0xc61940` pair / `er+0xc61d70` single / `er+0xc61be0` by-id):

```
filterInfo = *(u32*)(body + 0x6c)          // <-- collisionFilterInfo lives at BODY+0x6c
layer      = filterInfo & 0x7f             // 0..127  (the shape-tag codec's 7-bit field)
group      = (filterInfo >> 7) & 3         // 2-bit system group (equal non-zero groups DON'T collide)
collide    = (matrix[layerA] >> layerB) & 1   AND the group test
```

- The mod's ground raycast queries **layer `0x5e` (94)**; `matrix[94][boxLayer]` is set → **rays hit**.
- The **character** sweeps with *its own* layer; `matrix[charLayer][boxLayer]` was **NOT** set → falls through.
- Our box got the **default** `collisionFilterInfo` → the wrong layer. Real **placed-static map bodies
  carry `body+0x6c = 0x38`** (layer **56**, group 0) — the walkable layer the character collides with.

### Why every live `mem_write` to the body failed
`addBody` (`FUN_1418a9ff0`) inserts into a **single** broadphase (`world+0x4d8`, `hknpWideBroadPhase`,
insert via `vt[0x28]`) and **caches the filter into the broadphase leaf at insert time**. A post-`addBody`
`mem_write` to `body+0x6c` updates the body but not the cached leaf, so the character keeps filtering on
the stale layer. Confirmed live: poking `+0x64/+0x68/+0x6c` to real-body values (even in a fresh-pair
fall) did nothing. **The filter MUST be set before `addBody`.**

## Fix

`add_box` (`goblin_add_collision.cpp`): after `allocateBody` populates the slot and **before** `addBody`,
`WriteProcessMemory(body + 0x6c, &filterInfo, 4)`. Default `0x38` (layer 56); RPC-tunable:
`add_collision <hx> <hy> <hz> [<x> <y> <z>] go [<addMode> <actMode> <filterInfo>]`. `filterInfo` accepts
`0x`-hex; `-1`/absent → `0x38`.

## Not the cause (ruled out live)
- **addMode/actMode** — `0`=direct / `1`=deferred insert path; `actMode` = dynamic activation. Neither sets
  the collision layer. Box is in the broadphase either way.
- **Body slot fields `+0x64` (0x277, universal) / `+0x68`**  — not the filter; `+0x64` is identical across
  all 20250 bodies (a struct constant, not a per-body layer).
- **The hits collector** — `CSCharacterProxyHitsCollector` `addHit` (`er+0xc58990`) is closest-hit only, no
  per-body reject; the filter runs earlier, inside the cast.

## Key addresses (2.6.2.0, imagebase 0x140000000)
```
CSCollisionFilter            vtable er+0x2b91d00  ctor er+0xc5d730 (128-layer matrix @ filter+0x20, 0x800B)
  isCollisionEnabled         er+0xc61940 (pair) / er+0xc61d70 (single) / er+0xc61be0 (by filterInfo)
collisionFilterInfo          BODY + 0x6c   (u32; layer = &0x7f, group = (>>7)&3)
hknpUFMShapeTagCodec         world+0xb00 (shape-tag decoder, widths 2/4/7 → the 7-bit layer)
world collision filter       world+0x4d0    broadphase (hknpWideBroadPhase) world+0x4d8 (insert vt[0x28], castRay vt[0x80])
CsHkCharacterProxy           vtable er+0x2b92170   CSCharacterProxyHitsCollector vtable er+0x2b91378
engine down-ray cast         FUN_140c70360 (ctx, u32 filter, start[4], dir[4], outPt, outNrm, &dist)
addBody                      FUN_1418a9ff0 (bodyMgr, ids, count, addMode, actMode)
```

## Live-verify checklist (next boot)
1. `mfg.py rpc mfg_build` fresh; in-world.
2. `add_collision 3 1 3 go` (default filter 0x38) at player+40 → `hf_probe_present` still hits (broadphase OK).
3. Warp above a `go`-added box at the feet and drop → **rests on the box top** (was: fell through).
4. If layer 56 isn't the character's, sweep `filterInfo`: `add_collision 3 1 3 <x> <y> <z> go 0 0 <fi>` for
   `fi` in candidate layers, fall-test each. Read a body the player demonstrably stands on for its `+0x6c`.
