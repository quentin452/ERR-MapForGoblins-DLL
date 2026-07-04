# RE brief — WorldMapTile placement: decode `{col,row,suffix}` → map-space rect + LOD selection

**Goal:** find how the game turns a world-map tile (`MENU_MapTile_M{MM}_L{L}_{col}_{row}_{suffix}`) into a
**map-space rectangle** (where it's drawn on the Scaleform world map) and **which LOD (`L`) it draws at which
zoom**. We already load these tiles ourselves (DDS off disk) and place ART on our own virtual-map canvas; the
ONE missing piece is the authoritative tile→map-space grid so tiles tile SEAMLESSLY and align with markers.
Static Ghidra on `D:\ghidra_proj2\ER`, App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`, read-only.
Resolve as `[er_base+RVA]+offsets` / AOBs.

## What we ALREADY have (don't redo)
- **The archive is cracked + read in-game.** `menu/71_MapTile.tpfbhd`(BHF4)+`.tpfbdt`(BDF4): entry table
  {compressedSize u64@+8, dataOffset u32@+0x18, nameOffset u32@+0x20 (UTF-16LE)}, each entry = a `DCX`→`TPF`→
  256×256 `DDS` tile. Our DLL reads + decodes any tile (`src/worldmap/maptile.{hpp,cpp}`,
  `docs/plans/map_tile_loading_plan.md`). **28469 entries**: dimensions **M00 overworld / M01 underground /
  M10 DLC / M11 DLC-ug**, LODs **L0(most tiles)…L4**.
- **The map-space→world transform is SOLVED + live.** For the overworld, `worldX = mapU + 7040`,
  `worldZ = −mapV + 16512` (scale 1), derived live from the engine projection `project()` on
  `CS::WorldMapViewModel` (`docs/re/windows_world_to_mapspace_projection_re_findings.md`). So once we know a
  tile's **map-space rect (mapU0,mapV0,mapU1,mapV1)** we can place it exactly. This brief is ONLY the
  tile→map-space mapping.

## The unknown = decode `{col,row,suffix}` → map-space rect
Empirically (dumped from the tile names, 2026-07-04):
- For **M00 (overworld)**, `col ∈ [0,5]`, `row ∈ [0,5]` → a **6×6 block grid**.
- The 8-hex-digit **`{suffix}` sub-divides each block as a VARIABLE-DEPTH quadtree** (Morton/Z-order-looking):
  - block `00_00` and `05_05`: **1 tile** (suffix `00000000`) — empty/ocean, coarse.
  - block `02_03`: **78 tiles**, suffix set (structured Z-order):
    `0,8,10,18,20,28,30,38,40,48,50,58,60,68,70,78,80,c0,100,140,180,1c0, 800,840,880,8c0,900,940,980,9c0,
    2000,…,29c0, 4000,…,49c0, 6000,…,69c0` (max `0x69c0`).
  - global max suffix seen for M00_L3 ≈ `0x7800`.
  Our current best-guess (Morton deinterleave of the suffix, uniform 8×8 per block) is WRONG — the per-block
  depth varies, so tiles don't tile seamlessly. **Need the game's exact decode.**

**Answer these:**
1. **The tile loader/placement fn.** Find the WorldMapTile subsystem that parses/loads `MENU_MapTile_*`
   (string family `MENU_MapTile_M%02d_L%d_%02x_%02x_%08x` or similar — grep the exe for the format string)
   and computes each tile's on-map rectangle. Signature + how it's driven (per visible tile? per LOD level?).
2. **`{col,row,suffix}` → map-space rect.** The exact math: block origin from (col,row), and how `{suffix}`
   (Morton? quadtree path? two packed fields?) → the sub-tile's offset + SIZE within the block. Confirm
   whether it's a Morton interleave (even bits=X, odd bits=Z) and what the per-tile map-space SIZE is (does a
   deeper-quadtree tile cover a SMALLER map-space area?). Give the formula that reproduces block `02_03`'s 78
   tiles as a seamless cover.
3. **LOD selection.** Which `L` is drawn at which map ZOOM (the `WorldMapArea` zoom `+0x380`, live-readable)?
   i.e. the zoom→LOD table/threshold, so we stream the right level. Are LODs a mip pyramid (each L = ¼ the
   tiles, 2× the map-space per tile) — note the counts are NOT clean ¼ ratios (M00 L0=6873 L1=4660 L2=2166
   L3=561 **L4=4517** — L4 breaks the pyramid; what IS L4?).
4. **Is there a PARAM** (regulation) driving tile layout — a `WorldMapTileParam` / MapTile grid / a
   `WorldMapLegacyConvParam`-style table — vs pure name-decode? If a param, its id + row→rect fields (we'd
   read it live off disk, mod-agnostic — best case). If pure name-decode, the decode is enough.
5. **Per-dimension origin.** Confirm the map-space rect for M01/M10/M11 uses the SAME per-page converter as
   markers (base-UG shares overworld; DLC-OW = overworld affine per the projection findings) so our existing
   transform covers them, or note any tile-specific offset.

## Anchors / leads
- The tiles are drawn via the same **`CreateImage`** path as icons: `FUN_140d6bbc0` (our `WORLDMAP_CREATE_IMAGE`
  AOB), returns `CSTextureImage` (vtable `er+0x2bb8910`, rect `+0x74/78/7c/80`) — a tile's `CreateImage` call
  site should carry its map-space rect + the `MENU_MapTile_*` name (grep the format string, xref it).
- `CS::WorldMapViewModel` vtable `er+0x2ad82e0`; converter array `VM+0xF8` (stride 0x30, count `VM+0x280`);
  projection `FUN_140876140`; page table `er+0x2ad82f8` = `[00 01 0a]`. WorldMapArea zoom `+0x380`, pan `+0x378`.
- `CSWorldMapPointMan = [er+0x3D6E9B0]` (markers, adjacent subsystem). Grep exe for `MapTile` / `MENU_MapTile`
  UTF-16 format strings; xref → the placement fn.

## Deliverable
Enough to write, in `src/worldmap/maptile.cpp` + the vmap loader: `tile_name → (mapU0,mapV0,mapU1,mapV1)` for
any dim/LOD, plus a `zoom → LOD` rule. We already have map-space→world; with this the full overworld renders
seamlessly under the markers. Findings → `docs/re/windows_worldmap_tile_placement_re_findings.md`. NB the
Linux/DLL side (SRV heap recycling past the 256 cap + byte-range tile reads) is separate work, not this brief.
