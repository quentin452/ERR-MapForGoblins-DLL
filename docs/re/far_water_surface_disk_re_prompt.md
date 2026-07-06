# RE brief — per-region WATER from DISK (whole-map / far water for the relief sea-tag)

**Why.** The runtime material-tag (`windows_water_level_source_re_findings.md` §10) can tag water only where
**collision is streamed** — the near field around the player. Distant water (the ocean seen from afar, a far
lake) has **no streamed collision**, so a down-ray misses entirely and the cell can't be classified. This is
the SAME near-field wall as the procedural relief itself (`far_terrain_heightmap_re_findings.md`): the relief
has no distant cells either. So a **whole-map** relief with correct oceans/lakes needs a **disk** water source,
not a runtime cast — and it should ride the same offline pipeline as the far terrain.

**Both prompt-options are already ruled out** (`windows_water_level_source_re_findings.md`): Option 3
(`GXWaterHeightMap`) is a GPU wave-sim; Option 2 (a water-surface cast filter) is impossible (water has no
collision surface — it's a ground MATERIAL: `{Default,Grass,Water,Swamp}`). This brief is the **disk / far**
counterpart, deliberately paired with the far-terrain FLVER/collision frontier.

## The goal
A per-(x,z), **map-wide**, **mod-agnostic** water classification (and, if cheap, the surface Y) read from the
ACTIVE install's disk files — so the relief can paint water correctly even far from the player, and under any
mod. Primary need = a **water MASK** (paint the cell blue); a true surface **Y** is a bonus.

## What MFG already has (build ON these — don't rebuild)
- **Mature disk-MSB parser** — `src/worldmap/msbe_parser.{hpp,cpp}` (`parse_msb(...)`): decodes MODEL / EVENT /
  POINT / PARTS in BOTH disk (entry-relative) and resident (VA) modes; extracts Asset/Enemy/Treasure/Region/
  ObjAct with **position at Part+0x20 (x/y/z)** and the MODEL index. 99.3% position match
  (`windows_msbe_position_transform_validation.md`). **It does NOT yet read MapPiece/Collision parts or filter
  by model name** — that's the gap for water.
- **Full archive extraction** — `src/worldmap/dvdbnd_reader.{hpp,cpp}` + `msbe_parser.cpp::dcx_decompress`:
  RSA+AES dvdbnd read, DCX `DFLT`(zlib)/`KRAK`(Oodle `oo2core_6`) decompress, TPF texture slice. So MFG can
  already pull ANY packed map file (`.msb.dcx`, `.mapbnd.dcx`, `.hkxpwv`/`hkxbhd`) off disk.
- **NO FLVER parser, NO disk Havok/`hkxpwv` parser** (collision is only reached via live raycast today).
- **Runtime material-tag finding** — the collision carries a `Water`/`Swamp` material (`hknpMaterialLibrary`
  er+0x2ee36b0); the near-field tag reads it from a live hit. The disk path must find the SAME material offline.

## Core open question (answer FIRST)
**How is the ER water SURFACE represented in the map files, mod-agnostically?** Nobody here has confirmed it.
Three candidate disk sources — rank them by probing, pick the one that is whole-map + mod-agnostic:

### Source A (preferred — rides the far-terrain bake) — `hkxpwv` collision `Water`/`Swamp` MATERIAL
The far-terrain recommendation is an OFFLINE bake of the map collision (`hkxpwv` → `hknpCompressedMeshShape`,
`far_terrain_heightmap_re_findings.md` §5b). That mesh's `hknpMaterialLibrary` gives a **per-triangle material**
— so the same bake that yields whole-map elevation **also yields the water mask for free**: a cell is water iff
its seabed triangle's material ∈ {Water, Swamp}. Disk `hkxpwv` exists at full detail even where runtime
collision isn't streamed → whole-map coverage. **This is the natural answer: water = a rider on the terrain
collision bake, no extra pipeline.**
- Deliverable: the material-id VALUES for `Water`/`Swamp` (the enum ordinals — from Ghidra: the `MatRatio`
  material enum / `hknpMaterialLibrary`, or the SoulsFormats material map), and confirmation the lakebed/ocean
  floor is actually tagged Water/Swamp map-wide (not just wadeable edges — the key risk; validate at Liurnia +
  the ocean).
- Effort: HIGH but SHARED with far-terrain (Havok tagfile + compressed-mesh decode — SoulsFormats/HKLib solved
  offline; the base bake is a BUILD step, no in-DLL Havok parser). Gives the MASK at seabed Y (fine for a tint).

### Source B (the true surface Y, lighter) — MSB water-plane PART
If ER places the water surface as a flat part (MapPiece/Asset) in the MSB, its transform **Y = the real surface
level** (Liurnia lake ≠ ocean ≠ Siofra). MFG's parser already reads Part+0x20 position — it just needs to (1)
also decode MapPiece/Collision part types, (2) read MODEL names, (3) filter the water parts by a name/material
convention. LOW effort **if** water is an MSB part.
- Deliverable: does ER represent the water surface as an MSB part? If yes — the part TYPE + the model-name/model
  convention that identifies a water plane (e.g. a `mXX_water*`/AEG water model), so `msbe_parser` can emit a
  `WaterPlane{ Y, bounds }` list per map. If no — rule it out and fall to A.

### Source C (fallback) — a per-map water-height param / water REGION volume
Some engines carry a per-map water height or a water region volume (with a Y) in the MSB POINT/EVENT section or
a param. Uncertain in ER (no `SeaLevel`/`WaterHeight` exe const — already confirmed). Probe only if A and B fail.

## First probe (concrete, cheap — do this before committing to a bake)
1. **MSB water-part probe (tests Source B).** Extend `msbe_parser` (a throwaway debug pass) to dump, for a map
   KNOWN to contain a large water body — **Liurnia (`m14`/legacy) or an overworld tile over the sea/a lake** —
   EVERY PARTS entry with its part TYPE, MODEL name, and Part+0x24 **Y**. Look for a flat plane / water-named
   model at a plausible lake Y. Pull the `.msb.dcx` with the existing `dvdbnd` reader. If a water plane exists →
   Source B is the cheap win; read its Y.
2. **Material-id probe (tests Source A).** In Ghidra (`D:\ghidra_proj2\ER`), resolve the `Water`/`Swamp` material
   ORDINALS: the `MatRatio` material enum (strings `..._MatRatio_{Default,Grass,Water,Swamp}` @ er+0x2bc32b8) and
   the `hknpMaterialLibrary` (er+0x2ee36b0) material→id map. Then a single offline `hkxpwv` decode of one Liurnia
   collision + a material histogram tells you whether the lakebed is Water/Swamp-tagged map-wide.
3. Compare: if B gives a clean per-region surface Y with a simple convention, ship B for the surface + A's
   material only if a MASK independent of the plane is needed. If B doesn't exist, A is the answer.

## How this pairs with far-terrain (the unifying point)
`far_terrain_heightmap_re_findings.md` already recommends an **offline `hkxpwv` collision bake** for whole-map
elevation (over render-FLVER, to avoid false relief). **Do water in the SAME bake:** rasterize seabed Y (relief)
AND the per-triangle material (Water/Swamp → the sea mask) in one pass. So the far relief and the far water are
one deliverable, not two. Source B (MSB plane Y) is an optional overlay for the true surface height.

## Platform / tooling
- **Extraction:** already in MFG (`dvdbnd_reader`, in-process; Oodle in-process works on Linux — the daily box).
- **Offline parse:** SoulsFormats / HKLib (community-solved) for MSB + FLVER + Havok tagfile/compressed-mesh — a
  BUILD step, no in-DLL parser needed for the base bake.
- **Material ids:** Windows Ghidra (`D:\ghidra_proj2\ER`) for the `Water`/`Swamp` ordinals + `hknpMaterialLibrary`
  layout.
- **Validation:** Linux/Proton — compare the baked water mask/Y to the player's foot Y while standing in Liurnia
  lake vs on the ocean shore vs a Siofra river (expect DIFFERENT per-region levels; that's the whole point).

## Deliverable
`docs/re/far_water_surface_disk_re_findings.md`: which source (A material-tag rider / B MSB plane Y / C param)
carries the per-region water, the exact ids/convention/part-type to read it, whole-map coverage confirmation,
and how it folds into the far-terrain collision bake. Then the relief's sea-tag is fed map-wide, not just near
the player, and `kSeaLevelY` is finally retired.

## Pointers
- Runtime side (both options dead + the material path): `windows_water_level_source_re_findings.md`.
- Far terrain (the shared bake + `hkxpwv`/`hknpMaterialLibrary`/FLVER anchors): `far_terrain_heightmap_re_findings.md`.
- MSB parse to extend: `src/worldmap/msbe_parser.{hpp,cpp}`; extraction: `src/worldmap/dvdbnd_reader.{hpp,cpp}`.
- Relief consumer: `src/goblin_heightfield.cpp` (`c.sea`), render `src/overlay_panel/panel_virtual_map.cpp:1545`.
- Anchors: `hknpMaterialLibrary` er+0x2ee36b0; `hkxpwv` `HkxpwvResCap@CS` er+0x2b94c40; `hknpCompressedMeshShape`
  vt er+0x2eeb908; `MatRatio_{...}` strings er+0x2bc32b8..0x2bc34b8.
