# RE brief — reach the LIVE WorldMapTile rects (position only) to calibrate tile placement

**Goal (narrow — RECTS ONLY, no textures):** find how to reach a live `CS::WorldMapTile` instance (or its
`CS::WorldMapTiledLayer`) from something we can already resolve, and read its **map-space rect**
(`self+0x98..0xA4`) + the layer's resolved region bases. That rect is the region-walk output (the tile's
TRUE map-space origin) — we need it to CALIBRATE our name-decode placement, which is currently offset.
**Textures are explicitly OUT OF SCOPE for this brief** (separate follow-up). Static Ghidra on
`D:\ghidra_proj2\ER`, App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`, read-only.

## Context — what's solved and what's the gap
- Tile ART loads + decodes: name `MENU_MapTile_M{MM}_L{L}_{col}_{row}_{suffix}`, col/row DECIMAL,
  `suffix = 8·morton(subX,subY)`, `gridX = col*64+subX`, `gridZ = row*64+subY`
  (`windows_worldmap_tile_placement_re_findings.md`). Tiles are a uniform 256-map-space grid.
- map-space→world is SOLVED live (`windows_world_to_mapspace_projection_re_findings.md`): `worldX = mapU+7040`,
  `worldZ = -mapV+16512` (from projecting markers on `CS::WorldMapViewModel`).
- **THE GAP:** the tile NAME-grid does NOT map to the converter grid by `gridXbase`. Live data: land tiles
  sit at name-`gridX ~64` but the converter/marker grid puts that land at `~43` (offset ~21 in X, ~14 in Z,
  looks NON-uniform). The placement findings said the tile map-space origin is computed by the
  **REGION-WALK** (`FUN_1409d8c30` / `FUN_1409da9f0`, "origin passed to the tile ctor") but did NOT decode
  it. That origin is the missing calibration.

## What to deliver (in order of value)
1. **A live path to a `WorldMapTiledLayer` / `WorldMapTile`.** From the `CS::WorldMapViewModel`
   (vtable `er+0x2ad82e0`, we resolve it live from the map cursor / `WorldMapDialog` = cursor−0x2DB0) OR
   from the `WorldMapDialog`: what field/offset reaches the **tiled-layer manager** (dtor size `0x5d8`;
   per-LOD layer array at `mgr+0x390`, stride `0x110`; per-frame tick `FUN_1409cd390(mgr,dt)`)? Give the
   pointer chain (offsets) VM/dialog → mgr → layer. (We resolve VM/dialog already; we just need the next hop.)
2. **The layer's region fields.** Confirm `WorldMapTiledLayer` (vtable `er+0x2b2caf0`, ctor `FUN_1409e0cc0`):
   `+0x214`/`+0x218` = current gridX/gridZ, `+0x21c`/`+0x220` = **resolved bases**, tile RB-trees at
   `+0x230`/`+0x248`. **What exactly are `+0x21c/+0x220`** — are they the region-walk base that makes
   `rect.u0 = (name_gridX − base)*256`? If so, reading them live IS the calibration (no per-tile walk needed).
   Also give the **std::map node layout** for the `+0x230` tree (offsets of left/parent/right + the
   `WorldMapTile*` value) so we can enumerate tiles if the bases aren't enough.
3. **Confirm the tile rect.** `WorldMapTile` (vtable `er+0x2b31108`, ctor `FUN_1409df560`): rect at
   `self+0x98..0xA4` = `{u0,v0,u1,v1}`. Confirm it is **map-space in the SAME units as the projection
   output** (`FUN_140876140` / `WorldMapPointParam.posX`), i.e. our `worldX=u0+7040` would place it. Note
   any per-LOD scale (coarse LODs: is `rect` still 256, with the layer applying a `256·k^L` world scale?).
4. **Decode the region-walk origin (the actual fix).** In `FUN_1409d8c30` (region rebuild) /
   `FUN_1409da9f0` (per-cell create tail; tile ctor call at `er+0x9daa3f`): how is the `origin`
   (`float[2]` passed as the ctor's 5th arg) computed from the region (col,row) + the resolved bases?
   Give the formula `name(col,row,suffix) → rect.u0/v0`. This explains the ~21/~14 offset and lets us place
   ALL archive tiles correctly (not just resident ones).

## Anchors (this build, er-relative)
```
WorldMapViewModel vtable   er+0x2ad82e0   (VM; converter array VM+0xF8, page table er+0x2ad82f8)
WorldMapTile vtable        er+0x2b31108   ctor er+0x9df560   rect self+0x98..0xA4
WorldMapTiledLayer vtable  er+0x2b2caf0   ctor er+0x9e0cc0   trees +0x230/+0x248  grid +0x214/+0x218  bases +0x21c/+0x220
layer-manager tick         er+0x9cd390    (layers mgr+0x390 stride 0x110; view mgr+0x368)
region rebuild             er+0x9d8c30    per-cell create tail er+0x9da9f0 (ctor call er+0x9daa3f)  visit er+0x9dad90
dim selector               er+0x8867d0    (page {0,1,0x0a} → mgr +0x39c/+0x3a0/+0x3a4)
```

## Deliverable
Either (best) the **region-walk origin formula** `name → rect.u0/v0` (item 4) — then no live read is needed
at all — OR the **VM/dialog → layer pointer chain + the `+0x21c/+0x220` base semantics** (items 1–2) so the
DLL reads the live base and calibrates. Textures are a SEPARATE later brief. Findings →
`docs/re/windows_worldmap_tile_rect_reach_re_findings.md`.
