# RE findings — WorldMapTile placement: decode `{col,row,suffix}` → map-space rect + LOD

Answers `docs/re/windows_worldmap_tile_placement_re_prompt.md`. Static Ghidra RE on `D:\ghidra_proj2\ER`
(App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`, read-only). Builds on
`windows_world_to_mapspace_projection_re_findings.md` (the solved map-space→world converter, live off
`CS::WorldMapViewModel`) and `windows_worldmap_tile_fog_re_findings.md` (which first pinned the per-tile
sorted table at `VM+0x288` and CELLSIZE 256). Scripts: `D:\ghidra_scripts_mfg\mfg_tile.java` +
`D:\ghidra_scripts\query.java`.

---

## 0. TL;DR — the tile grid is a UNIFORM 256-map-space sparse grid, not a variable-size quadtree

- A `CS::WorldMapTile` is drawn as a **constant `256 × 256` map-space quad**. The size is a hard global
  constant (`DAT_143b37d00..d0c = {0, 0, 256, 256}`), added to a per-tile **origin** the placement code
  passes in. There is **no variable-depth quadtree of differing tile sizes** — every tile at every LOD is
  256×256 map-space. The apparent "variable depth" is just a **sparse** grid: ocean/empty blocks hold 1
  coarse tile, dense blocks hold many populated 256-cells.
- The archive filename `MENU_MapTile_M{MM}_L{L}_{col}_{row}_{suffix}` encodes the cell position as
  **`suffix = 8 · morton(subX, subY)`** (Z-order interleave; low 3 bits reserved/zero) within a
  **64×64-cell block** `(col,row)`. So:

  ```
  m      = suffix >> 3
  subX   = deinterleave_even(m)     // bits 0,2,4,… of m
  subY   = deinterleave_odd(m)      // bits 1,3,5,… of m
  gridX  = col * 64 + subX
  gridZ  = row * 64 + subY
  mapU0  = (gridX - gridXbase) * 256      // map-space, CELLSIZE 256
  mapV0  = (gridZ - gridZbase) * 256
  rect   = (mapU0, mapV0, mapU0 + 256, mapV0 + 256)
  ```

  `gridXbase`/`gridZbase` are the per-page converter bases already read live in the projection findings
  (converter `+0x0A gridXbase`, `+0x09 gridZbase`); the same `world = gridXZ·256 + local`, scale-1
  overworld transform then maps `mapU/mapV` to world. **Verified:** `morton(52,38)·8 = 0x69c0`, exactly the
  observed max suffix of block `02_03`; global max `~0x7800 < morton(63,63)·8 = 0x7ff8`, i.e. subX/subY
  stay `< 64` ⇒ block width `W = 64`.
- **No `WorldMapTileParam`.** Q4 is settled: layout is **pure name-decode**, not param-driven. The only
  tile-adjacent regulation is `WorldMapPieceParam` (fog-reveal rects + `openEventFlagId`, see the fog
  findings) — it does NOT drive tile geometry. So `native-from-name → circle`; no baked grid table needed.
- **LOD is a per-LOD layer stack, zoom-gated.** The tiled layer manager holds an array of
  `CS::WorldMapTiledLayer` (one per `L`), each with a zoom range; a `layer.minZoom <= currentZoom` test
  selects which LOD draws. Mechanism is pinned; exact per-L thresholds + the coarse-LOD cell scale are the
  one runtime-pinnable gap (see §4).

> This means MFG can render the overworld seamlessly by decoding each archive name with the formula above —
> no game call required, and it is **mod-agnostic** (works for any archive whose names follow the scheme;
> `gridXbase/gridZbase` come from the active install's live converter, not a bake).

---

## 1. Class + function map (the tile subsystem)

RTTI (`tools/ghidra/rtti_index.txt`, er-base-relative RVAs):

| Class | vtable | ctor(s) |
|-------|--------|---------|
| `CS::WorldMapTile` | `0x2b31108` | `0x9df560` (build), `0x9dfa70` (dtor) |
| `CS::WorldMapTiledLayer` | `0x2b2caf0` | `0x9cbef0`, `0x9e0cc0` (ctor), `0x9e0fa0` (dtor) |
| `CS::WorldMapTileRes` | `0x2ad7be8` | `0x8842b0` (ctor), `0x884360` (dtor) |
| `CS::WorldMapTileBackReader` | `0x2ad7b10` | `0x882500` |
| `CS::WorldMapViewModel` | `0x2ad82e0` | (projection findings) — owns the tile tables + converters |

Key functions (RVA = er-base + offset):

- **`FUN_1409df560` — `WorldMapTile` ctor / build.** The heart. Signature (recovered):
  ```c
  WorldMapTile* build(WorldMapTile* self, void* owner /*=VM, at +0x08*/, Alloc* a, void* p4,
                      float* origin /*{mapU0,mapV0}*/, u32 dimA, u32 dimB, u16 tileId,
                      u32 baseA, u32 baseB);
  ```
  - `self+0x20 = *(u64*)origin` (stores the 2-float origin).
  - **rect** written to `self+0x98..0xA4`: `(u0+0, v0+0, u0+256, v0+256)` — the constant-256 quad.
  - `self+0x38 (u16) = tileId`; label built `snprintf("Tile_%d_%d", (id%10000)/100, id%100)` — a **display
    label only**, NOT the placement coordinate (see §3).
  - Resolves **two texture layers** (dimA + dimB): `FUN_140886640(VM,dim,tileId)` (exists?) →
    `FUN_140882b30(VM+0x390,…)` (fetch resident) → else `FUN_140886560(VM,dim,tileId)` (resource value) →
    `FUN_140884460(&res, dim, tileId, value & base)` (make the CSTextureImage). Two layers stored at
    `self+0xA8` (`+0x15`) and `self+0x118` (`+0x23`).
- **`FUN_1409e0cc0` — `WorldMapTiledLayer` ctor.** Allocates the tile container; `FUN_1408867d0(mgr, code)`
  resolves the per-dimension key. Holds the two live tile RB-trees at `layer+0x230` / `layer+0x248` (keyed
  by u16 tileId), current gridX/gridZ at `layer+0x214`/`+0x218`, resolved bases at `+0x21c`/`+0x220`.
- **`FUN_1409e1550` — layer streamer (vtable slot 1).** Iterates the visible tile-key vector
  (`layer+0xd8..+0xe0`), skips the 3 cached current keys (`+0xf0/+0xf8/+0x100`), fade-updates each
  (`FUN_1409da680`). `param_2` = the LOD/level.
- **`FUN_1409d8c30` — region rebuild.** `(gridX,gridZ)` in → caches `+0x214/+0x218`, resolves the two
  per-dim bases via `FUN_1408867d0`, drives the visit callback `FUN_1409dad90` through `FUN_1409d74a0`.
- **`FUN_1409da9f0` — per-cell create tail** (un-analyzed region; recovered by hand). Calls the tile ctor
  with `origin = RBP+0x40`, `tileId = RBP+0x38`, `dimA/dimB = layer+0x214/+0x218`,
  `baseA/baseB = layer+0x21c/+0x220`, then inserts the new tile into the RB-tree by u16 id
  (`FUN_1409d7550`/`FUN_1409d7880`). **The origin is computed by the parent region-walk and passed down.**
- **`FUN_1408867d0` — dimension selector.** `code 0 → mgr+0x39c`, `1 → +0x3a0`, `0x0a → +0x3a4`, else 0.
  Same page code set `{0,1,0x0a}` as the projection page table `[00 01 0a]`.

Format string (name builder), UTF-16 at **`er+0x2ad7c40`**:
`MENU_MapTile_M%02d_L%u_%02d_%02d_%08x` — so `M`,`L` are `%02d`/`%u`, **col/row are `%02d` (decimal)**,
**suffix is `%08x` (hex, 32-bit)**. Per-dimension prefixes `MENU_MapTile_M00 / M01 / M10 / M03 / M04 / M05`
follow at `+0x2ad7cb2` (the archive sub-file set).

---

## 2. The `{col,row,suffix}` → grid decode (the answer to Q1/Q2)

Empirically derived from the block-`02_03` suffix dump and confirmed exact:

- **All suffixes are multiples of 8** — the low 3 bits are reserved (always 0).
- `suffix >> 3` is a standard **Morton / Z-order index**: even bits → `subX`, odd bits → `subY`.
- The first 16 suffixes `0x00,0x08,…,0x78` decode to a full `4×4` sub-grid; higher suffixes fill the block
  sparsely at consistent Morton coordinates. `subX ∈ [0,52]`, `subY ∈ [0,38]` seen in `02_03`.
- **`morton(52,38)·8 = 0x69c0`** = the observed block-`02_03` max ⇒ decode confirmed.
- Global max `~0x7800`; `morton(63,63)·8 = 0x7ff8` ⇒ `subX,subY < 64` ⇒ **block width `W = 64` cells**.

So (see the TL;DR formula): `gridX = col·64 + subX`, `gridZ = row·64 + subY`, tile map-space origin
`= ((gridX − gridXbase)·256, (gridZ − gridZbase)·256)`, rect `= origin + (0,0,256,256)`.

Reference C (drop-in for `src/worldmap/maptile.cpp`):

```cpp
// suffix (the %08x field) -> sub-cell within the 64x64 block
static void decode_suffix(uint32_t suffix, int& subX, int& subY) {
    uint32_t m = suffix >> 3;            // low 3 bits reserved
    subX = subY = 0;
    for (int i = 0; i < 8; ++i) {        // 6 bits each suffice for W=64
        subX |= ((m >> (2*i    )) & 1) << i;
        subY |= ((m >> (2*i + 1)) & 1) << i;
    }
}
// name "..._{col}_{row}_{suffix}" -> map-space rect (origin corner) in CELLSIZE-256 units.
// gridXbase/gridZbase from the live converter (projection findings); base = the page's cell origin.
static void tile_map_rect(int col, int row, uint32_t suffix, int gridXbase, int gridZbase,
                          float& u0, float& v0, float& u1, float& v1) {
    int sx, sz; decode_suffix(suffix, sx, sz);
    int gridX = col*64 + sx, gridZ = row*64 + sz;
    u0 = float((gridX - gridXbase) * 256); v0 = float((gridZ - gridZbase) * 256);
    u1 = u0 + 256.0f; v1 = v0 + 256.0f;
}
```

**Runtime cross-check to run on Linux/Proton (owns deploy+RPC):** (a) confirm the **axis order** — whether
Morton even-bits = the col-axis (`gridX`) or the row-axis (`gridZ`), and whether `subY` runs +down or +up
(one screenshot of a known tile, e.g. `M00_L0_02_03_00000000`, placed vs. a landmark marker). (b) confirm
`W = 64` holds for the ACTIVE archive by asserting `max(subX,subY) < 64` and that blocks tile without
overlap across the full name set (MFG already parses all names). (c) `gridXbase/gridZbase` — read live off
the converter, don't hardcode.

---

## 3. Why `tileId` ("Tile_%d_%d") is NOT the placement coordinate

The ctor stores a u16 `tileId` and formats `Tile_{(id%10000)/100}_{id%100}`. It is a **sorted lookup key**,
not a grid coordinate: `gridZ` reaches ~230 in a populated column, which cannot survive `id%100`. The
resolvers `FUN_140886560` (resource value) and `FUN_140886640` (exists flag) **binary-search** a
per-dimension **sorted table of 12-byte records `{u32 tileId, u32 value, u8 flag}`** at:

```
tables = VM + 0x288                     // 3 slots, dimension index {0:OW, 1:UG, 2:DLC} from {0,1,0x0a}
table  = tables[dim]                    // 8-aligned
recs   = [table+0x88 .. table+0x90)     // stride 0x0C, sorted ascending by tileId
```

(Same `VM+0x288` the fog findings pinned as the authoritative per-tile state.) Placement uses the origin
passed to the ctor (§1/§2), independent of this key. MFG places by name-decode → origin, so the key is
irrelevant to rendering.

---

## 4. LOD selection (Q3) — per-LOD layer stack, zoom-gated

The tiled-layer **manager** (dtor object size `0x5d8`) holds a per-LOD array of `CS::WorldMapTiledLayer`
(stride `0x110`) at `mgr+0x390 .. +0x398`. Its per-frame tick `FUN_1409cd390(mgr, dt)` iterates the layers
and, per layer, evaluates a **zoom-range test** against the view (`mgr+0x368`, a WorldMapArea-like object
with zoom at `+0x1c/+0x20`) and passes the resulting bool to each layer's activate vmethod
(`layer.vtable[1]`). So **which `L` draws is gated by the current map zoom** — a per-LOD zoom threshold,
not a param table. `WorldMapArea` zoom is live-readable at `+0x380` (projection findings).

Open (runtime-pinnable) sub-points, not needed to render but needed to STREAM the exactly-right level:
- the exact per-`L` zoom thresholds (read the `+0x1c/+0x20` range on each of the ~5 layers live), and
- the coarse-LOD world-cell scale. The tile map-space quad is a constant 256, so coarser LODs must scale
  their local grid to world in the LAYER transform (each `L`'s cell = `256 · k^L` world). The tile COUNTS
  are **not** a clean ¼ pyramid (M00 L0=6873 L1=4660 L2=2166 L3=561 **L4=4517**); L4 ≈ L1, so L4 is a
  distinct set (likely a full-coverage low-detail base layer, not the top of the pyramid). Confirm by
  reading a couple of L4 tile origins live and comparing cell spacing to L0.

For MFG's first pass, pick the LOD by name prefix (`_L{n}_`) matching a chosen zoom bucket and place with
the §2 formula; the per-tile rect math is identical across LODs (always 256 map-space), only the set of
populated cells differs.

---

## 5. Per-dimension origin (Q5)

Confirmed: dimensions map through the **same** `CS::WorldMapViewModel` converter array as markers
(projection findings). `FUN_1408867d0` selects the per-dimension resource root by page code
`{0 overworld → +0x39c, 1 underground → +0x3a0, 0x0a DLC → +0x3a4}`, i.e. the same `[00 01 0a]` page table.
Base-UG shares the overworld converter and DLC-overworld equals overworld (per the projection findings), so
the existing solved transform already covers M01/M10/M11 — no tile-specific per-dimension offset beyond the
converter's own `gridXbase/gridZbase`. The name prefixes seen in the exe are `M00/M01/M10/M03/M04/M05`
(the archive sub-file set); map each to its page code the same way markers do.

---

## 6. Anchors / pinned sites (this build)

```
WorldMapTile::vftable            er+0x2b31108   ctor er+0x9df560   dtor er+0x9dfa70
WorldMapTiledLayer::vftable      er+0x2b2caf0   ctor er+0x9e0cc0   streamer(slot1) er+0x9e1550
WorldMapTileRes::vftable         er+0x2ad7be8   ctor er+0x8842b0
WorldMapTileBackReader::vftable  er+0x2ad7b10   ctor er+0x882500
rect-size consts {0,0,256,256}   er+0x3b37d00 .. 0x3b37d0c
dimension selector               er+0x8867d0    ({0,1,0x0a} -> mgr +0x39c/+0x3a0/+0x3a4)
per-tile resource value          er+0x886560    exists-flag er+0x886640   (binary search VM+0x288)
per-cell create tail             er+0x9da9f0    (calls tile ctor at the UNCONDITIONAL_CALL er+0x9daa3f)
region rebuild                   er+0x9d8c30    visit callback er+0x9dad90   driver er+0x9d74a0
layer-manager tick / zoom gate   er+0x9cd390    (per-LOD layer array mgr+0x390, stride 0x110)
name format string (utf16)       er+0x2ad7c40   "MENU_MapTile_M%02d_L%u_%02d_%02d_%08x"
tile records / per-dim tables    VM+0x288       (3 slots; 0x0C-stride sorted {u32 id, u32 val, u8 flag})
```

RVAs are build-specific (2.6.2.0). None of the above is on the pinned-AOB path yet; pin an AOB only if MFG
ends up calling into the engine (it should not — the name-decode in §2 is self-contained).

## 7. What remains (all runtime, Linux/DLL — matches the brief's split)

- Axis-order + `W=64` confirmation against the active archive (§2 cross-check) — cheap, one session.
- Per-`L` zoom thresholds + the L4 anomaly + coarse-LOD cell scale (§4) — only needed for exact streaming.
- SRV heap recycling past the 256 cap + byte-range tile reads — the brief already scopes these as separate
  Linux DLL work; unchanged by this RE.
