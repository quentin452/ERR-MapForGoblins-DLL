# RE findings — WorldMapTile rect / placement calibration (position only)

Answers `docs/re/windows_worldmap_tile_rect_reach_re_prompt.md`. Static Ghidra on `D:\ghidra_proj2\ER`
(App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`, read-only). Scripts `D:\ghidra_scripts_mfg\mfg_tile.java`.
**Supersedes the grid math in `windows_worldmap_tile_placement_re_findings.md`** (which inferred the origin
instead of reading it — that inference was wrong; corrections noted there and in §5 below).

---

## 0. TL;DR — the tile grid is `floor(mapU/cellSize)` with a FLIPPED Z axis (this is the calibration)

The region walk builds tiles on a small integer grid derived DIRECTLY from the map-space view rectangle —
there is no `col·64` name grid involved in placement. For the overworld (`dimByte 0`):

```
cellSize = 256                         // map-space units per tile cell
N        = 41                          // grid is N×N  (0x29)
gridX = clamp( floor(mapU / 256), 0, N-1 )
gridZ = clamp( (N-1) - floor(mapV / 256), 0, N-1 )      // <-- Z AXIS IS FLIPPED
tileId = dimByte*10000 + gridX*100 + gridZ             // u16, stored at WorldMapTile+0x30
```

Inverse (what MFG needs) — the tile's **map-space rect** (exactly what the engine writes to
`WorldMapTile+0x98`):

```
mapU0 = gridX * 256
mapV0 = (N-1 - gridZ) * 256           // undo the flip
rect  = (mapU0, mapV0, mapU0 + 256, mapV0 + 256)
```

Then the SOLVED projection places it: `worldX = mapU0 + 7040`, `worldZ = -mapV0 + 16512`
(`windows_world_to_mapspace_projection_re_findings.md`).

**This is why the old decode was offset.** The prior findings used `gridX = col·64 + morton(suffix)` (values up
to ~180) and applied **no Z flip**. The real runtime grid is `floor(mapU/256)` ∈ `[0,40]` with Z flipped —
hence the reported "name-gridX ~64 vs converter ~43" (43 ≈ the N=41 grid) and the non-uniform look (the
missing flip skews Z differently from X).

---

## 1. `FUN_1408849e0` — map-space rect → integer cell range (the calibration fn)

```c
// int* FUN_1408849e0(int* out /*[xMin,zMin,xMax,zMax]*/, char dimByte, float* viewRect /*[u0,v0,u1,v1]*/)
cellSize = { [0]=0x100/*256*/, [1]=0x156/*342*/, [2]=0x508/*1288*/ }[dimByte];
Nx = Nz  = { [0]=0x29/*41*/, [1]=ceil(DAT_142ad7c90=30.69)=31, [2]=ceil(DAT_142ad7c8c=8.149)=9 }[dimByte];
out[0] = max(0,     floor(viewRect[0]/cellSize));          // gridX_min   (param_3[0]=mapU_min)
out[2] = min(Nx-1,  floor(viewRect[2]/cellSize));          // gridX_max   (param_3[2]=mapU_max)
out[1] = max(0,     (Nz-1) - floor(viewRect[1]/cellSize)); // gridZ_min   (param_3[1]=mapV_min) — FLIPPED
out[3] = min(Nz-1,  (Nz-1) - floor(viewRect[3]/cellSize)); // gridZ_max   (param_3[3]=mapV_max) — FLIPPED
```

The three `dimByte` values `{0,1,2}` are **three coarseness tiers**, each covering the SAME ~10.5k-unit map
extent: dim0 `256×41²`, dim1 `342×31²`, dim2 `1288×9²` (all ≈ 10496–11592). dim0 = the finest/overworld
tier. (These 3 tiers are the streaming levels; they are NOT the archive filename `L0…L4` — the archive
name↔cell mapping is the deferred TEXTURE piece, not needed for rects.)

## 2. `FUN_1409d9ba0` — the cell walk (tileId packing)

Two nested loops over the range from §1, packing the id and get-or-creating each tile:

```c
for (gridZ = out[1]; gridZ <= out[3]; ++gridZ)
  for (gridX = out[0]; gridX <= out[2]; ++gridX) {
      tileId = ((u16)dimByte * 100 + gridX) * 100 + gridZ;   // = dim*10000 + gridX*100 + gridZ
      tile   = FUN_1409da900(descriptor, tileId);            // RB-tree get-or-create by u16 id
      ... push tileId into the visible list ...
  }
```

So `"Tile_%d_%d"` in the ctor = `(id%10000)/100 = gridX`, `id%100 = gridZ`. `gridX,gridZ ∈ [0,N-1]` (< 100).

## 3. Call chain (per-frame) + the live pointer path

```
WorldMapArea::Update tick   FUN_1409cd390(area, dt)            // area = "manager", dtor size 0x5d8
  -> per layer (area+0x390 vector, stride 0x110):
  FUN_1409ce7d0(area, ...)   // builds the visible map-space rect from area view fields:
        viewRectMin/Max from area+0x340/+0x344/+0x348/+0x34c and zoom (DAT_14329e660=0.5)
  -> FUN_1409e1230(layer, lodByte, &viewRect, ...)             // pick region descriptor (layer+0xd8[lodByte])
  -> FUN_1409d8fa0(descriptor, on, &viewRect, ...)
  -> FUN_1409d9ba0(descriptor, &viewRect, 0)                   // §2 cell walk
        FUN_1408849e0(&range, descriptor+0xa8 /*dimByte*/, &viewRect)   // §1 calibration
        FUN_1409da900(descriptor, tileId) -> create tail er+0x9da9f0 -> WorldMapTile ctor FUN_1409df560
```

**Live path to read a tile rect** (all read-only; DLL-version-independent since these are game structs):

```
WorldMapArea         vtable er+0x2b2cb08   ctor FUN_1409cb9c0 (er+0x9cb9c0)   size 0x5d8
  +0x390  begin \  DLFixedVector<WorldMapTiledLayer*-or-inline>, stride 0x110  (per-LOD tier)
  +0x398  end   /  (FUN_1409cd390 iterates [+0x390 .. +0x398))
  +0x6c   back-ptr to the VM/owner (param_3 of the ctor)
WorldMapTiledLayer   vtable er+0x2b2caf0   ctor FUN_1409e0cc0 (er+0x9e0cc0)   size 0x110
  +0x230  std::map<u16 tileId, WorldMapTile*>   (primary tile RB-tree; +0x248 = a second tree)
          RB node: left@+0x00, parent@+0x08, right@+0x10, isNil@+0x19, key(u16)@+0x20, value(WorldMapTile*)@+0x28
WorldMapTile         vtable er+0x2b31108   ctor FUN_1409df560 (er+0x9df560)
  +0x30   u16 tileId  (= dim*10000 + gridX*100 + gridZ)
  +0x38   DLString "Tile_{gridX}_{gridZ}" label
  +0x98   float mapU0   +0x9c float mapV0   +0xA0 float mapU1(=U0+256)   +0xA4 float mapV1(=V0+256)
```

So the DLL can, **with zero name-decoding**, walk `area+0x390` layers → `layer+0x230` tree → each
`WorldMapTile`, and read `{ tileId@+0x30, rect@+0x98 }`. That yields the authoritative
`(dim,gridX,gridZ) → map-space rect` for every resident tile — the calibration, mod-agnostic. The
`WorldMapArea`→VM/dialog hop: `area+0x6c` is the back-ptr; the forward hop (dialog/VM → area) is a member of
the `WorldMapDialog` (vtable er+0x2b2d7d8, size 0x3ed0) — pin it live by scanning the dialog object for a
pointer to an object whose vtable == `er+0x2b2cb08` (cheap, one RPC), since the dialog ctor `FUN_1409cf8f0`
is a thin thunk that hides the member offset from static view.

## 4. Reference C for MFG (position only)

```cpp
// overworld dim 0: N=41, cellSize=256. dim 1: N=31,cs=342. dim 2: N=9,cs=1288.
static void tile_cell_to_maprect(int dim, int gridX, int gridZ, float& u0, float& v0, float& u1, float& v1) {
    const int   N[3]  = {41, 31, 9};
    const float cs[3] = {256.f, 342.f, 1288.f};
    u0 = gridX * cs[dim];
    v0 = (N[dim] - 1 - gridZ) * cs[dim];       // undo the flipped Z axis
    u1 = u0 + cs[dim];                          // engine writes +256 to WorldMapTile+0xA0/A4, but the
    v1 = v0 + cs[dim];                          // CELL stride is cellSize; use cs for a seamless cover
}
// inverse (map-space point -> cell), matches FUN_1408849e0:
static void map_to_cell(int dim, float mapU, float mapV, int& gridX, int& gridZ) {
    const int   N[3]  = {41, 31, 9};
    const float cs[3] = {256.f, 342.f, 1288.f};
    gridX = std::clamp((int)std::floor(mapU/cs[dim]), 0, N[dim]-1);
    gridZ = std::clamp((N[dim]-1) - (int)std::floor(mapV/cs[dim]), 0, N[dim]-1);
}
```

> NB the engine's per-tile texture rect (`+0x98`) is a constant `256×256` regardless of tier (const
> `DAT_143b37d00..d0c = {0,0,256,256}`); for dim1/dim2 the CELL stride (342/1288) ≠ 256, so those tiers
> scale a 256-px DDS across a larger map-space cell. For the overworld (dim0) stride == 256, so it tiles
> 1:1. Use `cellSize` (not the literal 256) for the placement stride.

## 5. Corrections to `windows_worldmap_tile_placement_re_findings.md`

That doc's §0/§2 are WRONG on the grid and must not be used for placement:
- ❌ `gridX = col·64 + morton(subX)` / block width `W=64` → ✅ `gridX = floor(mapU/cellSize)`, grid is `N×N`
  with `N={41,31,9}`, `cellSize={256,342,1288}` per tier (overworld 256/41).
- ❌ no Z flip → ✅ `gridZ = (N-1) - floor(mapV/cellSize)` (Z axis flipped).
- ❌ `(gridX − gridXbase)·256` with a converter base → ✅ base is `0`; `mapU0 = gridX·cellSize` directly.
- ✅ still correct: tiles are a uniform grid of constant `256×256` texture rects; `tileId = dim*10000 +
  gridX*100 + gridZ`; no `WorldMapTileParam` (pure runtime grid); LOD/tier = zoom-gated layer stack.

The `suffix = 8·morton(subX,subY)` observation still describes the ARCHIVE FILENAME's structure, but the
name↔cell mapping (needed only to FETCH a tile's DDS) is the deferred TEXTURE brief — it is NOT how the
engine positions tiles. For rects, use §1/§4 or read live rects (§3).

## 6. Deliverable status

RECTS/position: **DONE** (formula §1/§4 + live-read chain §3). Runtime confirm on Linux: read a few live
`WorldMapTile` `{+0x30 id, +0x98 rect}` and check `mapU0 == gridX*256`, `mapV0 == (40-gridZ)*256` for dim0,
then that markers land inside. TEXTURES (archive name ↔ cell) = separate brief, unchanged.

## 7. ⚠ LIVE-READ CHAIN DOES NOT YIELD TILES ON THIS BUILD (Linux/Proton recon, 2026-07-05)
Attempted the §3 live-read to close A3 (textured tile placement). `harvest_resident_tiles`
(`goblin_worldmap_probe.cpp`) + a new `vmap tile_recon` correlation RPC were driven against the OPEN,
panned+zoomed overworld map (3 boots, mouse pan/zoom confirmed moving via `[INPUT-DELTA]` pan `+0x378`/
zoom `+0x380`). Results — the §3 offsets are PARTLY WRONG for this runtime layout:
- **`area+0x390` IS the layer vector** — but of **INLINE `WorldMapTiledLayer` objects, 0x110 each**
  (`{begin,end,cap}` = layer0 / +0x110 / +0x220; vt `er+0x2b2caf0` at each 0x110 boundary, confirmed by
  `mem_dump`). The overworld in this state has **only 1 ACTIVE layer** (capacity 2).
- **The `+0x230` tile-tree offset is INCONSISTENT with a 0x110 inline layer** (0x230 > 0x110 → the harvest
  reads into the NEXT layer's memory, not a tile map). So §3's "layer+0x230 std::map" is wrong for an inline
  layout — it was static-Ghidra-inferred and the live object is smaller.
- **The active layer's own map is EMPTY** across all zoom levels tried (`head=0 root=0 size=0`; the two
  self-referential container heads inside the 0x110 object, at `layer+0x50` and near `+0xc8/+0xd0`, both read
  as empty/self-linked). ⇒ **No resident `WorldMapTile` objects populate in this state** — the tile streamer
  doesn't fill the tree here (possibly gated on the ART actually being drawn/streamed, which may not run the
  same way under this Proton/headless drive).
**⇒ A3 (textured tile placement) is BLOCKED on a fresh RE pass**, two sub-questions: (a) the CORRECT
tile-map offset inside the 0x110 `WorldMapTiledLayer` (candidates `+0x50` / `+0xc8`; dump a layer whose map
is NON-empty — need a state where tiles ARE resident); (b) WHAT makes tiles resident (zoom tier? region
dwell? the ART draw path?). Ghidra: re-check `WorldMapTile` ctor `FUN_1409df560`'s caller
`FUN_1409da900(descriptor, tileId)` — which container on the descriptor/layer it inserts into (that offset,
not the guessed +0x230, is the tree). The `vmap tile_recon` RPC (correlate resident grid vs archive
name-grid) is READY and correct — it just needs the harvest to return tiles first. Recon scripts:
`$CLAUDE_JOB_DIR/tmp/live_tile_*.py`, `live_area_dump.py`.

**Meanwhile the offset-broken name-grid placement (`virtual_map_load_lod`) and the position-only outline
harvest (`virtual_map_load_resident`) both stand** — neither is textured+correct. The alternative to fixing
the live read is solving the archive-name↔runtime-cell mapping (the deferred TEXTURE brief), which would let
the SOLVED formula (§4) place archive DDS directly with no live read.
