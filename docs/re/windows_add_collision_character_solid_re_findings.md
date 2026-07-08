# add_collision — making the injected box CHARACTER-solid (not just raycast-hit)

**Status (2026-07-08):** collision-filter mechanism fully root-caused in Ghidra + live-instrumented on
2.6.2.0. `add_box` now stamps `collisionFilterInfo` at `body+0x6c` before `addBody` (RPC-tunable). **RESULT:
NECESSARY BUT NOT SUFFICIENT — the box remains raycast-collidable but is NOT character-solid at ANY layer**
(verified live, incl. a zero-velocity embed test that rules out CCD tunneling). The character controller
does not collide with our dynamically-added convex `hknpBody`, regardless of `filterInfo`. Leading open
lead: the `addBody` broadphase-path split on `body+0x44 & 2` (see "Still open" below). The `filterInfo`
stamp is kept because it is the *correct* way to set the filter (matches real bodies) and is a live-tunable
lever for further experiments — it just isn't the whole fix.

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

## What the character query actually is (Ghidra + live, 2026-07-08)
The character controller's collision uses the **same world, filter and shape-tag codec** as the mod's
raycast — the only difference is the query `filterInfo`:
```
FUN_1418da590 (checkSupport/ground cast) & FUN_1418a3c50 (integrate/cast):
  desc+0x00 = world+0xb00 (shape-tag codec)   desc+0x08 = world+0x4d0 (collision filter)
  desc+0x14 = *(proxy+0x3c)  <- the CHARACTER's query filterInfo (mod raycast uses 0x5e)
  desc+0x20 = 2   desc+0x24 = 0xfb   (identical to the mod's raycast descriptor)
  cast: FUN_14187daa0 / FUN_14187d9f0 (world, &desc, proxy+0x50, collector)   collector = hknpCollisionQueryCollector
```
Live: the player `hknpCharacterProxy` (vtable `er+0x2ee97b8`) at `proxy+0x3c` = `0x8801cdf7`
(**layer 119**, group 3). So a body needs `matrix[119][bodyLayer]=1` to be hit by the character.

## RESULT — filterInfo layer is NOT the fix (live, reliable)
Swept the box `body+0x6c` layer; the character **falls through for every layer tested** (clean single-box
fall tests + a **zero-velocity embed test** = no eject, so not a CCD/tunneling artifact). Setting the
filter correctly (even to real walkable-body values) does **not** make the box character-solid.
- Trap that produced a false "all-solid" sweep: (a) boxes accumulate (no remove RPC) and (b) fall polls
  that early-exit on a mid-fall hitch report a phantom rest ~3 m above the box. Always poll to a genuine
  stable Y (Δ<0.02 over 6 reads) and test each box at a fresh, un-piled spot.

## Still open — leading hypothesis: the broadphase-path split
`addBody` (`FUN_1418a9ff0`) branches on `body+0x44 & 2`:
- **bit1 clear** (our box, and real bodies at layer 56) → `FUN_141920b80` insert.
- **bit1 set** (real templates `real0/1/2` = `+0x44` `0x106`, and `real15000` = `0xe`) → `FUN_141922f80`
  insert via `*(body+0x40)*0x80 + *(world+0x180)` (a motion/second structure).
The character may query the structure the **bit1-set** path populates, which our box never enters. Next test:
route our body down that path (set the motion/quality so `addBody` takes the `FUN_141922f80` branch) and
re-run the embed test. Alternative angle: the character might only collide with the **baked static-map
compound** (terrain mesh w/ per-triangle shape tags), in which case a dynamically-added convex `hknpBody`
can't be made character-solid at all via this route, and Route D is raycast-only.

## Bottom line for the mod
`add_collision` produces a body that is **raycast/probe-collidable** (usable for ground/height queries,
`hf_probe`, projectiles) but **NOT walkable/blocking for the player**. Do not advertise Route D greyboxes
as walkable until the `+0x44` path (or the static-compound route) is resolved.
