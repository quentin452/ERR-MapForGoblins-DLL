---
name: worldmap-unsearched-fog-mask
description: "SOLVED world-map fog-of-war oracle = FUN_140886560(VM, layer, tileId) -> flags, revealed iff (flags & 0x17fff)==0 (layer0); table at VM+0x288. tileId=group*10000+gx*100+gz. For gating overlay markers on explored map area. NOT WorldMapTile (=art streaming). Full: docs/re/windows_worldmap_tile_fog_re_findings.md §7."
metadata: 
  node_type: memory
  type: project
---

**SOLVED — the reveal oracle:** `FUN_140886560(VM, char layer, ushort tileId) -> u32 flags`.
layer 0/1/10→idx 0/1/2; `cont=*(VM+0x288+idx*8)`; sorted 12-byte records {u32 tileId, u32 flags,
u32 extra} in `[cont+0x88 .. cont+0x90)` (binary search). **tileId = group*10000 + gridX*100 + gridZ.
Revealed (non-fog) iff (flags & MASK)==0**, MASK=0x17fff (overworld/DLC layer0) / 0x1f (layer10).
DLC map = layer 0, sheets = groups 0/1/2 (validated: layer0→983/2218 revealed, organic blobs matching
the in-game fog screenshot; layer10→0). AOB FUN_140886560 =
`48 83 EC 28 44 0F B6 CA 45 0F B7 D8 84 D2 74 1D 41 83 E9 01 74 10 41 83`. VM live via
`worldmap_probe::find_view_model()`. For `tile_fogged`: read VM+0x288[0] table once per map-open →
revealed-tileId set → gate markers next to `marker_fogged` in map_renderer.cpp (no fn call needed).
The `mask+0x08` 31×31 bit-grid (WorldMapUnsearchedMask @ VM+0x380) is a FROZEN coarse copy
(singleton FUN_140256350+0x7f8), built from this table by FUN_140888440 — not the live source.

RE of the per-tile world-map fog-of-war (to gate overlay markers so they don't show in unexplored
/ black map areas). Brief = `docs/re/windows_worldmap_tile_fog_re_prompt.md`; findings =
`docs/re/windows_worldmap_tile_fog_re_findings.md`. App 2.6.2.0 / ERR 2.2.9.6, er imagebase 0x140000000.

**Key correction to the brief:** `CS::WorldMapTile` is NOT the fog — it's map-ART texture streaming
(follows the VIEW, swaps wholesale on fast-travel; tileId@+0x30 u16 = group*10000+gx*100+gz,
"Tile_%d_%d"). The fog lives in **`CS::WorldMapUnsearchedMask`** ("Unsearched" = unexplored).

**The store (SHIPPABLE facts):**
- `WorldMapUnsearchedMask` is an embedded sub-object of `CS::WorldMapViewModel` at **VM+0x380**
  (VM ctor `FUN_1408855b0` sets `param_1[0x70]=vftable`; `FUN_140884bb0` wires `*(mask+8)=singleton+0x7f8`,
  `singleton=FUN_140256350()`).
- `mask+0x08` → **bit-packed buffer** in that persistent singleton = PERMANENT explored footprint
  (doesn't change walking in already-explored ground; matches the in-game organic revealed blob).
- `mask+0x20` = dims **(31,31)**. **bit 1 = unsearched/fog, bit 0 = searched/revealed.** Live ~92/961
  explored. `mask+0x10`=WorldMapTileBackReader, `mask+0x18`=inline aux bitmask, `mask+0x48/0x50` f32 (0 / 7352.3, map-space mapping TBD).

**RTTI (regen after patch):** Mask vtable RVA 0x2ad7ca0 (ctors 0x884b80/0x884b70/0x8855b0);
MaskView vtable RVA 0x2b31568 (ctors 0x9e1b30/0x9e1d00, thin MenuViewItem — draw samples mask as a
texture, no CPU per-cell vmethod).

**Separate:** map POINT discovery (graces) = `_DiscoverMapPoint` `FUN_140a84080` → SetEventFlag on
`WorldMapPointParam.openEventFlagId` — same event-flag family as fragment reveal / `marker_fogged`.
See [[nobake-coverage-scoreboard]], the c3a5e41 fog_reveal_mask correction.

**NOT YET PINNED (last mile for `tile_fogged`):** exact bit layout (row stride: contiguous-31-bit vs
byte-aligned-4B; bit order) + cell↔map-space mapping. Pin via `mask_raw.py` move-diff (walk into
GENUINE black → one bit flips 1→0; a short walk in explored area flips nothing) or the singleton
bit-accessor. Tools: `<ghidra_scripts>\{tile_fog_probe,tile_fog_probe2,mask_probe,mask_grid,mask_grid2,mask_raw}.py`.
Live mask instance was 0x1c9b2518100. Build: gate next to `marker_fogged` in `map_renderer.cpp`
(markers already projected to map-space via `worldmap_probe::project`). See [[rpm-live-memory-tooling]], [[ghidra-re-tooling]].
