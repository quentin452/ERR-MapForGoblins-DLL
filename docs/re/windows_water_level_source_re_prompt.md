> **SUPERSEDED — see `windows_water_level_source_re_findings.md` (2026-07-06).** BOTH options ruled out.
> Option 3 (`GXWaterHeightMap@GXSR`) is a GPU render-resource for the water *interaction*/wave sim (sub-object
> of `GXSceneContext` @ +0xBE20), not a CPU base-plane. Option 2 (a water-surface cast filter) is impossible:
> ER has NO water collision surface — the cast filter byte is a 7-bit collision-LAYER index and water is not a
> collision layer; water is a ground MATERIAL (Default/Grass/Water/Swamp). The real path is a **material-based
> sea-tag** (cast 0x5e → hit material ∈ {Water, Swamp} via `hknpMaterialLibrary`). Prompt kept for context.

# RE brief — per-region WATER LEVEL source for the vmap relief sea-tag (no global sea level)

**Why.** The vmap's procedural relief (`goblin_heightfield.cpp`, down-ray terrain cast → hillshade) wants to
paint WATER cells blue (M1 sea-tag). The first design used a single global `kSeaLevelY` constant
(`c.sea = groundY < kSeaLevelY`). That model is **WRONG** and is now a dormant sentinel: ER has **no single
water plane** — Liurnia lake, the ocean, and the Siofra/Ainsel/Nokstella underground rivers are all at
different heights, so one threshold over-tags low coastal land and misses elevated water.

**Confirmed (2026-07-06, Linux — `strings` on `eldenring.exe` 2.6.2.0 + prior RE):**
- NO `SeaLevel` / `WaterLevel` constant string in the exe.
- Water is a **per-region height field**: class **`GXWaterHeightMap@GXSR`** (RTTI `.?AVGXWaterHeightMap@GXSR@@`;
  vtable `er+0x30370b0`, ctor `er+0x1b78960` per `far_terrain_heightmap_re_findings.md`), driven by
  **`GXWaterInteractionManager`** / **`GXWaterInteractionParam`** / **`GXWaterInteractionParamManager@GXBS`**,
  with `GXWaterNormalMap` + `GXWaveTerrain`.
- **NavMesh** is Havok AI (`hkaiWorld`, `hkaiNavMesh*`) — WALKABILITY, not a water surface. Water areas are
  non-navigable, but the navmesh does not carry a water height; it is the wrong source for the sea-tag.

## THE QUESTION — give the relief a per-(x,z) water surface Y, map-CLOSED, mod-agnostic

Two acceptable answers (either unblocks M1); Option 2 is cheaper if the filter exists.

### Option 2 (preferred if it exists) — a WATER-INCLUSIVE cast filter
The terrain cast uses filter `0x5e` (`FILTER_GROUND`, `goblin_heightfield.cpp` — `FUN_140c70360` line-cast:
`(ctx, filter, start[3], dir[3], outPt[3], outNrm[3], &outDist)`). `0x5e` SKIPS the water volume and hits the
seabed. **Find the filter value (collision layer/mask) that HITS the water SURFACE.** Then per cell: cast
once with `0x5e` (terrain/seabed Y) and once with the water filter (surface Y); `water = surfaceY > terrainY`,
and the surface Y is the real per-region water level. No manager, no singleton — just a second cast.
- Deliverable: the water-surface filter constant (and confirmation it returns the surface, not foliage/objects
  — validate at a known lake AND the ocean, expecting different Ys).

### Option 3 (authoritative) — sample `GXWaterHeightMap@GXSR`
Resolve the live **`GXWaterInteractionManager`** (its singleton/owner), reach the active region's
`GXWaterHeightMap`, and find the **sample(x,z) → height** entry. Give: the manager singleton (static slot or
owner chain from a known root), the `GXWaterHeightMap` layout (bounds/origin/cell size/height array or the
sampler fn RVA), and whether it's resident map-closed. Then the relief reads the true water Y per cell.

## Runtime validation (Linux daily box is fine)
- The game runs under Proton here; the down-ray cast is already proven (`hf_probe`/`hf_sample` RPCs,
  `[HEIGHTFIELD]` log). For Option 2: add a filter arg to a cast RPC, stand over a lake vs the ocean, and
  compare the water-filter Y vs the `0x5e` Y — expect surface>seabed and DIFFERENT per region.
- For Option 3: dump the resolved `GXWaterHeightMap` for the loaded region and compare a sampled height to the
  player's foot Y while wading.

## Deliverable
`docs/re/windows_water_level_source_re_findings.md`: the filter value (Option 2) OR the manager+sampler
(Option 3). Then `goblin_heightfield.cpp` swaps `c.sea = groundY < kSeaLevelY` for the per-region water-Y test
and drops the `kSeaLevelY` sentinel; the render branch (`panel_virtual_map.cpp`, water-blue) is already wired.

## Pointers
- Code: `src/goblin_heightfield.cpp` (cast + `kSeaLevelY` note), `src/overlay_panel/panel_virtual_map.cpp`
  (`Cell.sea` → water-blue render).
- RE builds on: `windows_terrain_raycast_heightfield_re_findings.md` §4 (water = GXSR WaterInteractionManager
  / WaterHeightMap, the three options), `far_terrain_heightmap_re_findings.md` §4 (`GXWaterHeightMap@GXSR`
  vt `0x30370b0` / ctor `0x1b78960`, `GXWaveTerrain@GXSR` vt `0x303a1a0`).
- Strategy: `docs/plans/imgui_only_map_plan.md` M1 (sea-tag) + `procedural_map_derivation_design.md`.
