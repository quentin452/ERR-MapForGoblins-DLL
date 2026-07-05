# Far-terrain elevation source — RE FINDINGS (static scoping) — reshapes the prompt

Answers `far_terrain_heightmap_re_prompt.md` (find the distant-terrain height source for a whole-map relief
backdrop). Static Ghidra RTTI-index reconnaissance on `D:\ghidra_proj2\ER` (imagebase 0x140000000, 9760
classes), 2026-07-05. **Key result: the prompt's assumed easy path — a resident/global heightmap TEXTURE
(shape A) — does NOT exist. ER's far terrain is FLVER LOD MESH geometry, which collides with the untouched
FLVER frontier (README #4).** This changes the strategy; options + a recommendation below.

---

## 0. TL;DR
- **No global terrain heightmap texture.** The 9760-class RTTI index has **zero** `*TerrainHeightMap*` /
  `*LodTerrain*` / distant-terrain-height class or non-water `*Height*` texture/param. The only height maps
  are **water** (`GXWaterHeightMap`, `GXWaveTerrain` — usable for sea level, prompt #5).
- **Terrain = FLVER meshes streamed from `.mapbnd`.** The map/terrain render path is
  `CSMapbndResCap`/`CSMapbndRepositoryImp`/`CSMapbndFileCap` (the `.mapbnd` archive loader) →
  `CSMapModelIns@CS` (map model instance) → `FlverRepository`/`FlverResCap` + `CSFlverDrawSystem` (FLVER draw).
  Distant terrain = the **low-LOD FLVER** of each map piece, streamed with no collision.
- **⇒ Shape A (resident heightmap texture read) is out** — there is no such texture. **Shape B (disk no-bake)
  = parse the low-LOD terrain FLVERs from `.mapbnd`** — i.e. the FLVER-mesh frontier that is otherwise
  untouched. There is no cheap texture-sample "ground Y at (x,z)" for the far field.

## 1. Evidence (RTTI index sweep)
```
FOUND (terrain render = FLVER/mapbnd):
  CSMapModelIns@CS            vt 0x2b32da8 (ctors 0x9ef530/0x9ef4a0)   the placed map/terrain model instance
  CSMapbndResCap@CS           vt 0x2ba5658 (ctors 0xce9ee0/0xce9e30)   .mapbnd resource (terrain archive)
  CSMapbndRepositoryImp@CS    vt 0x2ba5418                              .mapbnd repository
  CSMapbndFileCap@CS          vt 0x29d6138                              .mapbnd file
  FlverRepository/FlverResCap/CSFlverDrawSystem/CSFD4Flver2Accessor     FLVER load + draw (the mesh path)
  WorldMapPieceParam@CS       vt 0x2a5a328 (many ctors)                 map-SCREEN piece param (see §3, cheap-lead)

FOUND (water only — sea-level classification, prompt #5):
  GXWaterHeightMap@GXSR       vt 0x30370b0 (ctor 0x1b78960)
  GXWaveTerrain@GXSR          vt 0x303a1a0 (ctor 0x1b97ab0)

NOT FOUND (searched, zero hits):
  *TerrainHeightMap*, *LodTerrain*, *DistantTerrain*, *FarChunk*, *HiRes/LowLod terrain set*,
  *GITerrain*, *TerrainCluster*, *GRGrid/GRTerrain*, any non-water *Height* texture/param.
```
The minimap system (`WorldMapTile`/`WorldMapTiledLayer`/`WorldMapTileRes`, already RE'd) is the **map-screen
COLOR** tiles — flat 256×256 textured quads (`windows_worldmap_tile_rect_reach_re_findings.md`); their "3D
look" is **baked hillshade IN the color texture**, not elevation values. So it is not an elevation source
either (it's shading, and it's a mod-baked snapshot).

## 2. Why this is the answer (architecture)
ELDEN RING's overworld is `map/m60/m60_XX_YY_LL` pieces. Each piece is a `.mapbnd` with **FLVER models at
multiple LOD** (LL = LOD/hierarchy level). Near = high-LOD FLVER + a collision `.hkxbnd` (the raycast hits
this). Far = the **coarse-LOD FLVER**, rendered with NO collision (why the raycast misses it, as the prompt
observed). There is no separate heightmap representation — the "height" only exists as **FLVER vertex Y**.
Confirmed by: the rich FLVER/mapbnd infrastructure above + the total absence of any heightmap-texture class.

## 3. Realistic options for the deliverable (whole-map relief), ranked
| # | Path | Mod-agnostic? | Effort | Coverage | Verdict |
|---|------|---------------|--------|----------|---------|
| 1 | **Parse low-LOD terrain FLVERs from `.mapbnd`** (disk no-bake, Oodle/dvdbnd) → rasterize vertex Y into a heightfield | ✅ (reads active install) | **HIGH** — opens the FLVER frontier (README #4): FLVER parser + per-piece stitch | whole map (all pieces on disk) | the "correct" path; biggest brick |
| 2 | **Read resident `CSMapModelIns` vertex buffers** (GPU, draw-free, like icon-harvest) | ✅ | HIGH + fragile (find resident VBs, decode FLVER vertex fmt) | only STREAMED LOD (not whole map at once) | worse than #1 (partial coverage) |
| 3 | **`WorldMapPieceParam` cheap-probe** — check if it carries per-piece elevation/bounds (coarse) | ✅ (param) | LOW to check | per-piece (very coarse) | likely only screen-tile placement, not elevation — but cheap to rule in/out first |
| 4 | **Reuse the baked map-screen COLOR tiles as the backdrop** (already read by `maptile.cpp`) | ⚠ mod-BAKED snapshot | LOW (done) | whole map | gives a relief IMAGE (shading), NOT elevation Y; violates mod-agnostic for non-ERR |
| 5 | **Near-field raycast only; leave the far field flat** | ✅ | none | loaded radius only | honest fallback if elevation isn't worth the FLVER project |

**Recommendation:** do **#3 first (cheap)** to rule the param in/out; then, if whole-map ELEVATION is truly
wanted, it means committing to **#1 (the FLVER parser)** — that is a project of its own and is the natural
"open the FLVER frontier" milestone, not a quick add. If the goal is just a *visual* backdrop (not queryable
Y), **#4** already exists (the shaded color tiles) and is nearly free, at the cost of being an ERR bake.

## 4. Sea/water level (prompt #5) — cheap, available
`GXWaterHeightMap@GXSR` (vt 0x30370b0) + `GXWaveTerrain@GXSR` (vt 0x303a1a0) exist as real classes → a
per-region water height is readable. For a first pass a **global sea-level constant** classifies far cells
sea-vs-land (the raycast prompt's heuristic); the `GXWaterHeightMap` instance is the mod-agnostic upgrade.

## 5. Next / decision needed
This is a **strategic fork**, not a mechanical next step:
- If elevation Y is required → schedule the **FLVER-mesh parse** (option #1) as its own effort (it's the
  README-#4 frontier; pairs the far field with the near-field raycast for a seamless hillshade).
- If a visual backdrop suffices → **option #4** (shaded color tiles) is already in place; only the
  mod-agnostic caveat remains.
- Quick win regardless: **option #3** (`WorldMapPieceParam` probe, ctors 0x55ba10/0x55a610/0x559b10 +
  0xd570a0/0xd572e0/0xd57460) to confirm it has no usable elevation before deciding.

## 6. Anchors
```
terrain model instance     CSMapModelIns@CS        vt 0x2b32da8 (ctors 0x9ef530/0x9ef4a0)
.mapbnd loader             CSMapbndResCap@CS 0x2ba5658 / RepositoryImp 0x2ba5418 / FileCap 0x29d6138
FLVER draw path            FlverRepository / FlverResCap / CSFlverDrawSystem 0x2b6e750 (ctor 0xb53560)
map-screen piece param     WorldMapPieceParam@CS   vt 0x2a5a328 (cheap-probe for elevation, option #3)
water height (sea level)   GXWaterHeightMap@GXSR 0x30370b0 (ctor 0x1b78960); GXWaveTerrain 0x303a1a0
near-field raycast (pairs) FUN_140c70360, ctx=*(*(er+0x3d76060)+0x98), filter 0x5e  (loaded-only)
```
Cross-ref: `windows_terrain_raycast_heightfield_re_findings.md` (near field), `maptile.cpp` /
`windows_worldmap_tile_rect_reach_re_findings.md` (color tiles), README #4 (FLVER frontier).
```
