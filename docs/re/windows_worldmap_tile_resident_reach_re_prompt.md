# RE brief — reach the LIVE resident WorldMapTile tree (fix the harvest) + what makes tiles resident

**Goal (narrow):** the mod needs to enumerate the game's **live resident `CS::WorldMapTile` instances**
(their `{tileId@+0x30, map-space rect@+0x98}`) to place map ART correctly (A3: overworld + underground +
DLC tiles, offset-free). We have a live-read walk but it reaches the WRONG container and returns 0 tiles.
This brief resolves the exact pointer chain + the residency condition. Static Ghidra on `D:\ghidra_proj2\ER`,
App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`, read-only. Supersedes the "reach the rects" prompt for
the RESIDENT-enumeration path (`windows_worldmap_tile_rect_reach_re_findings.md` §3 is partly wrong live —
see below).

## Context — what's solved
- Tile ART loads/decodes from `menu/71_MapTile` (BHF4), `MENU_MapTile_M{MM}_L{L}_{col}_{row}_{suffix}`.
- map-space→world SOLVED live: `worldX = mapU+7040`, `worldZ = −mapV+16512`.
- Tile grid formula SOLVED (`..._tile_rect_reach_re_findings.md` §1/§4): `gridX=floor(mapU/cs)`,
  `gridZ=(N−1)−floor(mapV/cs)` (Z flipped), tier `cs/N = 256/41, 342/31, 1288/9`; `tileId = dim*10000 +
  gridX*100 + gridZ` @ `WorldMapTile+0x30`; rect `{u0,v0,u1,v1}` @ `WorldMapTile+0x98`.
- We reach the live map objects: active map cursor → `WorldMapDialog` (cursor−0x2DB0) → scan for
  `WorldMapArea` (vtable `er+0x2b2cb08`). All confirmed live.

## THE GAP — live recon (Linux/Proton, 2026-07-05) proved the harvest walks the wrong object
Drove the OPEN, panned+zoomed overworld map and `mem_dump`'d the live objects. Findings that CORRECT
`..._tile_rect_reach_re_findings.md` §3:
- **`WorldMapArea+0x390` = `std::vector<WorldMapTiledLayer>` of INLINE 0x110-byte layer objects**
  (`{_First,_Last,_End}` = layer0 / +0x110 / +0x220; the `er+0x2b2caf0` layer vtable sits at each 0x110
  boundary). The overworld had **1 active layer** (capacity 2). CONFIRMED by dumping.
- **⇒ `+0x230` CANNOT be a tile tree inside a 0x110 layer** — it reads into the NEXT layer's bytes. The §3
  "layer+0x230 std::map" is wrong for the live inline layout; our harvest read garbage there (saw a null
  head) and returned 0.
- **PRIMARY HYPOTHESIS (from §3's OWN cell-walk chain):** the tile tree is NOT on the layer — it is on a
  per-LOD **DESCRIPTOR** the layer points to. §3 records the per-frame walk:
  `FUN_1409e1230(layer, lodByte, &viewRect) → descriptor = layer+0xd8[lodByte]`, then
  `FUN_1409d8fa0(descriptor,…) → FUN_1409d9ba0(descriptor,&viewRect,0)` cell-walk, and
  `FUN_1409da900(descriptor, tileId)` = **RB-tree get-or-create ON THE DESCRIPTOR**. My layer dump shows
  `layer+0xd8 = 0x3117c1e98` (a live pointer) — the descriptor (or descriptor array) candidate.

## What to deliver (in order of value)
1. **The exact layer→descriptor→tile-tree chain (the fix).** Decompile `FUN_1409e1230` (picks the
   descriptor from `layer+0xd8` by `lodByte`) and `FUN_1409da900` (get-or-create): confirm
   - is `layer+0xd8` a **pointer to a descriptor**, or a **base of an array** indexed by `lodByte` (stride?)
     — give the exact offset+index math to reach the active descriptor;
   - the **descriptor's tile-tree offset** (the std::map `FUN_1409da900` inserts into) + the node layout
     (left/parent/right/isNil/key(u16)/value(WorldMapTile*) offsets) so we can DFS it;
   - the **descriptor's `dimByte`** field (§1 said `descriptor+0xa8`) — confirm, so we tag dim per tree.
   With this the mod fixes `harvest_resident_tiles` (walk `layer+0xd8`→descriptor→tree, not `layer+0x230`).
2. **What makes `WorldMapTile`s RESIDENT (the empty-tree question).** Even reading the guessed `+0x230` as an
   empty tree, we never confirmed tiles populate. When does `FUN_1409da900` actually INSERT tiles — every
   frame the map is open (view-rect driven), or gated on the ART texture being streamed / a specific LOD tier
   / region dwell? Trace `FUN_1409d9ba0`'s caller conditions and `FUN_1409cd390(area,dt)` (per-frame tick):
   under what state is the descriptor's tree non-empty? (We drove pan+zoom across LOD tiers and saw no
   populated tree at the guessed offset — need to know if that's the wrong-offset misread or a real
   residency gate. If a gate, name the flag/condition so the mod can satisfy it or read the right tier.)
3. **(If cheap) the descriptor's resolved region bases.** §1 mentioned `+0x21c/+0x220` region bases on the
   layer/descriptor. If reading them live gives the tile map-space origin directly (no per-tile walk),
   report the offsets — it's a simpler calibration than enumerating the tree.

## Live anchors (this build, er-relative, 0x140000000) — confirmed unless noted
```
WorldMapArea        vtable er+0x2b2cb08   +0x390 vector<WorldMapTiledLayer inline 0x110> {_First,_Last,_End}
WorldMapTiledLayer  vtable er+0x2b2caf0   ctor FUN_1409e0cc0   size 0x110 (inline in the area vector)
   +0xd8  -> descriptor (or descriptor[] by lodByte)  [live: layer+0xd8 = a valid ptr]  <-- CONFIRM
WorldMapTile        vtable er+0x2b31108   ctor FUN_1409df560   +0x30 tileId(u16)  +0x98 rect{u0,v0,u1,v1}
per-frame tick      FUN_1409cd390(area, dt)
pick descriptor     FUN_1409e1230(layer, lodByte, &viewRect)      <-- decompile for the +0xd8 index math
cell walk           FUN_1409d9ba0(descriptor, &viewRect, 0)       <-- residency conditions
get-or-create tile  FUN_1409da900(descriptor, tileId)             <-- the tree offset + node layout
calibration fn      FUN_1408849e0(&range, descriptor+0xa8/*dimByte*/, &viewRect)
```
Cross-ref: `windows_worldmap_tile_rect_reach_re_findings.md` (§3 partly-wrong live), our live dump in
`hf_hook_scout`/`vmap tile_recon`. Mod side ready: `harvest_resident_tiles` + `vmap tile_recon` (correlates
the resident grid vs the archive name-grid) — both work once the walk reaches the real tree.
```
