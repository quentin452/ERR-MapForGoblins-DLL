# FINDINGS — disk water source, Probe 1 (MSB water-plane) → NEGATIVE; Source A (collision material) is the path

Executes Probe 1 of `far_water_surface_disk_re_prompt.md`. Offline MSB inspection via SoulsFormats
(`SoulsFormats.MSBE` through pythonnet/coreclr, `tools/lib/Andre.SoulsFormats.dll`) on the **vanilla
UXM-unpacked** install (`D:\SteamLibrary\...\ELDEN RING\Game\map\mapstudio`, base geometry = mod-agnostic),
2026-07-06. Probes: `tools/_probe_water_msb.py`, `tools/_probe_water_deep.py`.

**Verdict: Source B (identify a water plane in the MSB and read its Y) is a DEAD END.** ER MSBs carry no
water-identifiable entity, no per-part water Y, and the overworld tiles (where the big water bodies are) have
no MSB collision at all. The disk water source must come from the FLVER/collision geometry layer →
**Source A: the `hkxpwv` collision `Water`/`Swamp` material, riding the far-terrain bake.** Evidence below.

---

## 1. No water-identifiable MSB entity (19-map sample: legacy + underground rivers + overworld)
Scanned Parts / Models / Regions for water tokens (`water/umi/mizu/kawa/gawa/numa/taki/lake/sea/pond/wet/
swamp/moat/river`) across m10–m19 legacy, m12 underground (Siofra/Ainsel **rivers**), and 20 spread overworld
tiles:
- **0 water-token models, 0 water-token parts, 0 water-token regions.**
- **Part subtypes:** `Asset, Enemy, DummyEnemy, MapPiece, Collision, DummyAsset, ConnectCollision, Player` —
  **no `Water` part subtype.**
- **Region subtypes** (34 kinds: PatrolRoute, EnvironmentMapEffectBox, SFX, MufflingBox, WeatherOverride, …) —
  **no water/hazard-water region.**
- Model names are **opaque numeric codes** (`mNNNNNN` map-pieces, `hNNNNNN` collision, `AEG…` assets), NOT
  romaji/English words — so nominal token-matching can't identify a water map-piece even if one exists.

Even a literal river (Siofra `m12_01`) has **no** water-named anything. ER does not name water in the MSB
(consistent with the exe having no water strings, `windows_water_level_source_re_findings.md`).

## 2. The surface Y is NOT in the MSB (it's in the FLVER/hkx)
Deep dump of `m14_00_00_00` (Academy, sits **in** Liurnia lake) + `m12_01_00_00` (Siofra): **every MapPiece
and every Collision part has Position `(0,0,0)`, identity rotation, unit scale.** In ER the terrain FLVER (and
the collision hkx) carry world-positioned geometry themselves; the MSB part is a placement stub at the origin.
So there is **no per-part water surface Y to read** — the Y lives in the FLVER vertices / hkx mesh. This kills
Source B as a *cheap* lookup: identifying a water plane (even if possible) would still require parsing the FLVER
to get its Y — the same FLVER frontier as the far terrain.

## 3. Collision `HitFilterID` is not a water discriminator (and overworld has no MSB collision)
MSBE `Collision` parts DO carry a behaviour class **`HitFilterID`** (+ `DisableTorrent`, `PlayRegionID`,
`LocationTextID`, …). Tested whether a value means "water" — histogram across water vs dry maps:
```
m14 (Academy, in-lake):  8×73, 17×7, 11×2, 29×2, 13/15/16/22 ×1
m12_01 (Siofra river):   8×60, 11×5, 16×2, 13/15/17 ×1
m10 (Stormveil, dry):    8×98, 17×4, 9/11/13/15/19/20/23 ×1, 16×2
m60_10_09_02 (overworld):  0 collisions
m60_11_09_02 (overworld):  0 collisions
```
`8` (standard walkable) dominates everywhere; the small-count special values (11/13/15/16/17/…) appear in BOTH
water and dry maps with no water-distinct value. **`HitFilterID` does not tag water.** And the sampled
**overworld LOD tiles have ZERO MSB Collision parts** — overworld collision (where the ocean/big lakes are)
lives in the separate `hkxpwv`, not the MSB. So the MSB collision layer is useless for overworld water too.

## 4. Conclusion — Source A (hkxpwv collision material) is the only disk water source
The MSB carries no water name, no water part/region type, no per-part water Y, no water `HitFilterID`, and no
overworld collision at all. The **only** disk signal that distinguishes water is the collision **material**
(`Water`/`Swamp`) in the `hkxpwv`/`hknpMaterialLibrary` layer — exactly the runtime material-tag
(`windows_water_level_source_re_findings.md` §10), read offline. That layer is the far-terrain collision bake
(`far_terrain_heightmap_re_findings.md` §5b), so **water rides the terrain bake**: rasterize seabed Y (relief)
+ per-triangle material (Water/Swamp → the sea mask) in one pass. There is no lighter MSB shortcut.

## 5. Next — Probe 2 (the material path, unchanged from the prompt)
1. **Material ids:** in Ghidra resolve the `Water`/`Swamp` ordinals (the `MatRatio` material enum
   er+0x2bc32b8, and the `hknpMaterialLibrary` er+0x2ee36b0 material→id map).
2. **Offline `hkxpwv` decode:** one Liurnia/Siofra collision → material histogram → confirm the lakebed/river
   floor is Water/Swamp-tagged map-wide (not just wadeable edges — the open risk). This is the real work
   (Havok tagfile + `hknpCompressedMeshShape` decode, SoulsFormats/HKLib-solved offline, a build step).
3. Then feed the relief sea-tag map-wide from the bake, retire `kSeaLevelY`.

## 6. Pointers
- Prompt (sources ranked): `far_water_surface_disk_re_prompt.md`.
- Runtime side + material tag: `windows_water_level_source_re_findings.md` (§9–§11).
- The shared bake + hkxpwv/material anchors: `far_terrain_heightmap_re_findings.md` (§5b).
- Probes: `tools/_probe_water_msb.py` (taxonomy + token scan), `tools/_probe_water_deep.py` (per-map deep dump
  + HitFilterID histogram). Read the vanilla `game_dir/map/mapstudio` via SoulsFormats.
