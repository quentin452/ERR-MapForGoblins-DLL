# RE findings — terrain raycast → heightfield (procedural relief map)

Answers `docs/re/windows_terrain_raycast_heightfield_re_prompt.md`. Static Ghidra on `D:\ghidra_proj2\ER`
(App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`, read-only). Scripts `D:\ghidra_scripts_mfg\mfg_ray.java`.
Builds on the AEG-streamer lead in `windows_geom_spawn_pivot2_re_findings.md`.

---

## 0. TL;DR — one callable down-ray gives ground height + normal

Physics is **Havok `hknp`**, wrapped by **`CS::PhysWorld`** (RTTI `PhysWorld@CS@@`), an FD4Singleton at
**`DAT_143d76060` (er+0x3d76060)**. The lead `FUN_140c74c70` is a *shape* cast (profiler tag
`TtWorldCastShape`, used only by the AEG streamer). The clean primitive MFG wants is the **ray** sibling:

```c
// er+0xc70360 — line-cast, returns hit point + normal + distance
int FUN_140c70360(
    void*  ctx,                 // = *(DAT_143d76060 + 0x98)   (the world holder; ctx+8 = hknpWorld)
    u32    filter,              // collision query type (see §3). Ground/terrain = 0x5e.
    float* start,   // [x,y,z]  ray origin
    float* segDir,  // [x,y,z]  segment vector; END = start + segDir  (down-ray: {0,-Dist,0})
    float* outPoint /*[3] or null*/,   // OUT world hit point (ground)
    float* outNormal/*[3] or null*/,   // OUT surface normal (→ hillshade)
    u32*   outDist  /*or null*/ );     // OUT hit distance
    // returns 1 on hit, 0 on miss.
```

Per grid cell `(x,z)`: `start={x, Yhigh, z}`, `segDir={0, -(Yhigh-Ylow), 0}`, read `outPoint[1]` = **ground
height**, `outNormal` = slope. Same world frame as markers (`worldX=mapU+7040`, `worldZ=-mapV+16512`), so the
sampled matrix drops straight onto the existing projection. Under the hood it calls
**`hknpWorld::castRay`** `FUN_14187d960` (`TtWorldCastRay`) at `ctx+8 → hknpWorld` (`+0x4d8` vtable slot
`0x80`), collecting into an `hknpClosestHitCollector`.

This is the mod-agnostic "sample the 3D world" path the brief wants: never stale, correct for any mod that
reshapes the world.

---

## 1. The cast family (all on `CS::PhysWorld`)

| Fn | RVA | Kind | Signature (args after ctx) | Notes |
|----|-----|------|----------------------------|-------|
| **`FUN_140c70360`** | er+0xc70360 | **ray, full** | `filter, start, segDir, &pt, &nrm, &dist` → hit? | **USE THIS.** point+normal+dist. |
| `FUN_140c722c0` | er+0xc722c0 | ray, bool | `filter, start, segDir` → hit? | hit/miss only. |
| `FUN_140c706c0` | er+0xc706c0 | ray/shape | `filter(0x2000058), …` | variant (high-bit flag set). |
| `FUN_140c70920` | er+0xc70920 | ray | dynamic filter | filter from a caller field. |
| `FUN_140c74c70` | er+0xc74c70 | **shape** cast | `out, 0x5d/0x67, &rayStruct` | the AEG-streamer lead; hits AEG assets. |
| `FUN_14187d960` | er+0x187d960 | low | `hknpWorld::castRay` | `(**(world+0x4d8))[0x80]`; `TtWorldCastRay`. |
| `FUN_14187d9f0` | er+0x187d9f0 | low | `hknpWorld::castShape` | vtable slot `0x88`; `TtWorldCastShape`. |

`ctx` for every wrapper = **`*(DAT_143d76060 + 0x98)`** (the AEG shape cast passes exactly this as its
`param_1`). `ctx+8` is the `hknpWorld` holder; the wrappers read `*(hknpWorld+0x4d0)`/`+0xb00` as the query
scene data and build the query + an `hknpClosestHitCollector` on the stack (query flags `0xffff`, `2`, `0xfb`
are the fixed defaults; `filter` is written into the query).

## 2. Hit extraction (what `FUN_140c70360` returns)

On hit (`collector.hasHit`): `outDist = fraction · rayLength`; `outPoint` = the collector's hit position
(swizzled from `local_c8..`); `outNormal` = the hit normal (`local_b8..`). The AEG shape variant additionally
yields the **hit collidable/body** (`param_10` — 2 qwords) and a body/material id (`param_8`), from which the
streamer derives an AEG asset number — evidence that a cast can also return *which* object was hit (useful if
we later want to reject object hits explicitly rather than by filter).

## 3. The filter (`param_2`) — which collision the ray tests

The `filter` byte is an ELDEN RING collision **query type** (there is a `WorldCollisionFilterCommand`
subsystem). Observed values by feature:

| filter | caller | feature | hits |
|--------|--------|---------|------|
| **`0x5e`** | `FUN_1403f13c0` | **snap-position-to-ground** (projects a point down onto the floor, player-gated) | **walkable ground / map** |
| `0xe0` | `FUN_1406e4210` | character ground/step probe (4 rays) | map + walkable static |
| `0x5d`,`0x67` | `FUN_140699670/d80` | AEG proximity streamer (shape) | AEG assets / props |
| `0x2000058` | `FUN_1403fb760` | character (high-bit flag `0x02000000`) | map + flag |

**Recommended terrain filter = `0x5e`** — it is the exact value the engine uses to drop a position onto the
ground, so a down-ray with `0x5e` returns the landscape/walkable surface and (unlike the AEG `0x67`) is not
the object-picking path. Confirm empirically on the Linux side (§6): cast down from a known point and check
`outPoint.y ≈ known ground Y`; if separate MSB props leak in, they can also be rejected by the returned body
id (§2) since MFG already has objects as markers. The exact query-type→collision-layer table
(`WorldCollisionFilterCommand`) is not fully decoded; `0x5e` is sufficient for the heightfield.

## 4. Water level (sea vs land)

There is **no single global water-plane Y**. Water is a **GXSR `WaterInteractionManager`** (`GXSR@@`) driven
by a **`WaterHeightMap`** + `WaterInteractionParam` (RTTI `WaterInteractionParam@GXSR@@`,
`WaterInteractionManager@GXSR@@`; strings `WaterDepth`, `WaterDischarge`) — a per-region height field, not a
constant. Options for MFG, cheapest first:
1. **Heuristic sea level** — classify a cell as sea if `groundY < seaLevelConst` (ER's main map sea is near a
   known Y; pick per-page). Good enough for a silhouette/coastline on the overworld; zero extra RE.
2. **Water-inclusive cast** — a filter value that also hits the water surface collision would return the
   water Y directly; not identified here (would need the filter table from §3).
3. **Sample `WaterHeightMap`** — the authoritative per-(x,z) water height via the `GXSR
   WaterInteractionManager`; a separate RE task (heightmap format + sampler). Flagged as follow-up.

Recommend (1) for the first relief pass; escalate to (3) only if coastlines look wrong under a mod.

## 5. Normal / material (nice-to-have)

- **Normal**: already returned by `FUN_140c70360` (`outNormal`) — feed straight into a hillshade
  (`shade = dot(normalize(normal), lightDir)`). Best relief for free.
- **Material/biome**: the cast's hit body (§2) can be resolved to a collidable; ER collision carries a
  material id, but the biome tint mapping is not decoded here — a follow-up if biome coloring is wanted.

## 6. Runtime constraints (for the Linux/Proton side)

- **Thread-safety.** The cast reads the live `hknpWorld`. The AEG streamer calls it on the **game update
  thread**. `hknp` scene queries are read-only but race the broadphase/step if run mid-physics-tick. Safest:
  call from the **per-frame game-thread hook** MFG already uses, NOT the present/RPC thread (consistent with
  the `FUN_1406a5080` present-thread deadlock noted in `windows_geom_spawn_pivot2_re_findings.md`). A pure
  `ReadProcessMemory` heightfield is not possible — the cast must execute in-process.
- **Loaded-region only.** The cast only hits **streamed-in** collision. There is no "load all collision"
  shortcut; sample the region around the player, and warp to extend coverage (grace warp is already RE'd —
  `windows_grace_warppin_teleport`). Cells with no hit = not-yet-streamed (or open air) → leave as nodata.
- **World frame.** `start`/`outPoint` are in the same world XZ frame as markers/`WorldMapPointParam.posX`
  and the solved projection — no extra transform to place the sampled matrix.

## 7. Anchors (this build, er-relative)

```
CS::PhysWorld singleton   DAT_143d76060 (er+0x3d76060)   ctx = *(DAT_143d76060 + 0x98)   RTTI PhysWorld@CS@@
ray cast (full)           FUN_140c70360  (filter,start,segDir,&pt,&nrm,&dist) -> hit
ray cast (bool)           FUN_140c722c0
shape cast (AEG lead)     FUN_140c74c70  (out,0x5d/0x67,&rayStruct)
hknpWorld::castRay        FUN_14187d960  ("TtWorldCastRay",  world+0x4d8 vslot 0x80)
hknpWorld::castShape      FUN_14187d9f0  ("TtWorldCastShape", vslot 0x88)
ground/terrain filter     0x5e   (snap-to-ground FUN_1403f13c0); char probe 0xe0; AEG 0x5d/0x67
player pos (ray origin)   WorldChrMan [er+0x3d65f88] +0x1e508 LocalPlayer +0x6C0 pos
water                     GXSR WaterInteractionManager / WaterHeightMap (per-region heightmap; no global Y)
```

## 8. Deliverable status

Cast fn + ABI + manager + arg/out layout + a working terrain filter (`0x5e`) + water source: **DONE (static)**.
Enough for the DLL to, per grid cell, cast a down-ray on the game thread, get ground Y + normal, build a
hillshade, and classify sea/land by a sea-level heuristic. Runtime to confirm on Linux: `0x5e` returns
terrain (not objects) via a known-point check; pick the overworld sea-level constant; thread-hook placement.
Follow-ups (separate briefs): exact `WorldCollisionFilterCommand` table (if `0x5e` leaks objects), `GXSR
WaterHeightMap` sampling (if the heuristic coastline is insufficient), material→biome tint.
