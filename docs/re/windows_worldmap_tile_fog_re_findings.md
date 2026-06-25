# RE findings — per-TILE world-map FOG-OF-WAR (the real "踏破"/discovered state)

Answers `docs/re/windows_worldmap_tile_fog_re_prompt.md`. Static Ghidra (`D:\ghidra_proj2\ER`,
`tools/ghidra/query.java` + `rtti_index.txt`) + live RPM (`D:\ghidra_scripts\*.py`).
App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. **Status: SOLVED — the per-tile reveal oracle is
`FUN_140886560` (§7). The bit-grid (§3) is a derived/coarse copy; the authoritative per-tile state is
a sorted flags table at `VM+0x288` (§7).**

## TL;DR — the brief's premise was half-wrong, but the real store is found

- The brief guessed the fog lives in `CS::WorldMapTile`. **It does NOT.** `WorldMapTile` is the
  **map-ART texture streaming** grid (§1). The fog-of-war lives in **`CS::WorldMapUnsearchedMask`**
  ("Unsearched" = not-yet-explored), a **sub-object of `CS::WorldMapViewModel`** (§2).
- The discovered/explored state is a **bit-packed grid** in a persistent singleton: bit `1` =
  unsearched (fog/black), bit `0` = searched (revealed). ~92/961 cells explored on the test save (§3).
- Engine "discovery" of map POINTS (graces etc.) is separate and event-flag based (§5).

## §1 — `WorldMapTile` is texture streaming, NOT fog (ruled out)

Tile ctor `FUN_1409df560`:
- `+0x30` (u16) = **tileId = `gridX*100 + gridZ`** (name `"Tile_%d_%d"`, `(id%10000)/100`, `id%100`).
  Full live ids carry a leading group: **`tileId = group*10000 + gridX*100 + gridZ`** (group = map
  sheet, 0/1/2 seen for DLC).
- `+0x08` = owning `WorldMapViewModel`; `+0x20/+0x24` = map-space centre (f32); `+0x98..+0xa4` = tile
  rect; `+0x28/+0x2c` = two detail-layer ids (live 10/11); fetches 2 art layers from `VM+0x390`.
- Live: only ~12–54 tiles resident = the **streamed neighbourhood of the current VIEW**, and the set
  **swaps completely on fast-travel** (group 0 → groups 1+2). So tiles follow the view/zoom, are
  destroyed when you leave, and cannot be a persistent per-tile fog. `+0x33`/`+0x34` vary per tile =
  streaming/fade state, NOT a reveal flag (no clean player-tile signal across a move-diff).

## §2 — the fog store: `WorldMapUnsearchedMask`

RTTI (rebuild after a patch): vtable RVA `0x2ad7ca0`, ctors `0x884b80/0x884b70/0x8855b0`.
`MaskView` (the draw): vtable RVA `0x2b31568`, ctors `0x9e1b30/0x9e1d00`.

The mask is an **embedded sub-object of `WorldMapViewModel`**: the VM ctor `FUN_1408855b0` sets
`param_1[0x70] = WorldMapUnsearchedMask::vftable` → mask lives at **`VM + 0x380`** (0x70 qwords).
The mask's own ctor `FUN_140884b80` is a stub (just the vtable); its data is wired by the VM ctor:
`FUN_140884bb0(mask, singleton+0x7f8)` which is literally `*(mask+8) = singleton+0x7f8`
(`singleton = FUN_140256350()`). So **`mask+0x08` points into a persistent global**, i.e. the
permanent explored footprint (survives leaving an area; doesn't change walking in already-explored
ground — confirmed: a walk in revealed terrain flipped 0 bits).

### Live struct layout (instance `0x1c9b2518100` on the test session)

| off | type | meaning |
|----|----|----|
| `+0x00` | ptr | vtable (`er+0x2ad7ca0`) |
| `+0x08` | ptr | **bit buffer** → `singleton(FUN_140256350)+0x7f8`. bit 1 = fog, 0 = searched |
| `+0x10` | ptr | `WorldMapTileBackReader` |
| `+0x18` | u64 | inline bitmask (`0x001fbfffffffffff` live — one bit clear; coarse/aux grid?) |
| `+0x20` | i32×2 | **grid dims = (31, 31)** (`width`,`height`) |
| `+0x48` | f32 | `0.0` (origin?) |
| `+0x50` | f32 | `7352.3` (map-space extent/offset — mapping TBD) |

## §3 — bit semantics CONFIRMED live

Buffer is **bit-packed** (121 bytes for 31×31=961 bits). Live popcount = 876 ones / 968 →
**~92 cells with bit 0 = explored** (~9.6%), the rest fog. First ~27 bytes are `0xff` (solid fog) and
the explored bits form a localised region lower in the grid — matches the in-game screenshot (one
organic revealed blob, ~10% of the full DLC sheet; the on-screen view is zoomed into the dense part).
Both the inline `+0x18` field and the buffer's first byte (`0xfe`) carry single-bit-clear patterns,
consistent with "1 = unsearched".

## §4 — tools (re-runnable)

- `D:\ghidra_scripts\tile_fog_probe.py` / `tile_fog_probe2.py` — resident `WorldMapTile` dump (ruled
  out tiles as fog; pins tileId = group*10000+gx*100+gz).
- `D:\ghidra_scripts\mask_probe.py` — find `UnsearchedMask`/`MaskView` instances, dump fields.
- `D:\ghidra_scripts\mask_grid.py` / `mask_grid2.py` — render the 31×31 grid (layout candidates).
- `D:\ghidra_scripts\mask_raw.py` — raw-buffer move-diff (snapshot, then walk into NEW fog, re-run →
  the flipped bit pins the exact layout + cell). **The deterministic calibration.**

## §5 — map POINT discovery (separate, event-flag based)

`_DiscoverMapPoint` = `FUN_140a84080`: reads a `CSWorldMapPointIns`'s `WorldMapPointParam.openEventFlagId`
(`point+0x80 → +0x04`) and, gated by `FUN_14080d860(point+0x80+0x30)`, `SetEventFlag(flagId,1)`
(`FUN_1405d2110`); tested by `IsEventFlag` (`FUN_1405d1330`). This is the per-point (grace etc.)
discovery — the same `openEventFlagId` family as the fragment-region reveal already handled by
`marker_fogged` / `require_map_fragments`. NOT the per-tile walk-fog.

## §6 — why the `mask+0x08` bit-grid was a dead end for live calibration

`mask+0x08` → `singleton(FUN_140256350)+0x7f8` is **frozen during play** (byte-identical popcount 876
across fast-travel, walking, and crossing fog). It's a coarse/persistent copy written at save/init,
NOT updated per step → a live move-diff flips nothing. The authoritative per-tile state is the table
in §7, which `FUN_140888440` reads to BUILD the render mask (`VM+0x380`).

## §7 — SOLVED: the per-tile reveal oracle `FUN_140886560`

```c
// FUN_140886560(VM, char layer, ushort tileId) -> u32 flags
//   layer 0->idx0, 1->idx1, 10('\n')->idx2, else return 0.
//   container = *(VM + 0x288 + idx*8); table = [container+0x88 .. container+0x90)
//   records are 12 bytes: { u32 tileId, u32 flags, u32 extra }, sorted by tileId (binary search).
//   returns the matched record's `flags` (u32 at rec+4), or 0 if tileId absent.
```
- **`tileId = group*10000 + gridX*100 + gridZ`** (same scheme as `WorldMapTile`; `group` = map sheet).
- **Revealed (NOT fog) iff `(flags & MASK) == 0`.** `MASK = 0x17fff` for the overworld/DLC sheets
  (layer 0), `0x1f` for layer 10. (Masks lifted from the mask builder `FUN_140888440`, which loops
  41×41 tiles calling `FUN_140886560` and marks the render mask where `(flags & MASK)==0`.)
- `FUN_140886640` is the sibling (returns the `extra` byte at rec+8 / a found-bool).
- **Live validation (shape):** layer 0 returned 983/2218 revealed across groups 0/1/2 forming
  **organic connected blobs** (geography, not noise) matching the in-game fog screenshot. The DLC map
  the player views = **layer 0**, sheets = groups 0/1/2 (layer 10 had 0 revealed → not the DLC path).
- AOB (`FUN_140886560`): `48 83 EC 28 44 0F B6 CA 45 0F B7 D8 84 D2 74 1D 41 83 E9 01 74 10 41 83`.
- Callers (confirm role): tile streamer `FUN_1409df560` (WorldMapTile ctor), mask builder
  `FUN_140888440`, `FUN_140883040`, `FUN_140888b70`.

### `tile_fogged` recipe (two options)

VM is live via `worldmap_probe::find_view_model()`. Per-marker we have `raw_gx/raw_gz` + page/group.

- **Option A (call the oracle):** resolve `FUN_140886560` by AOB; `flags = fn(VM, 0, group*10000 +
  raw_gx*100 + raw_gz)`; fogged if `(flags & 0x17fff) != 0`.
- **Option B (read the table, no call — preferred, matches our RPM style):** once per map-open read
  `cont = *(VM+0x288)`; iterate `[cont+0x88 .. cont+0x90)` 12-byte records into a map
  `tileId -> (flags & 0x17fff)==0`; a marker is fogged if its tileId is absent OR maps to false.

**Last integration detail:** the marker's **group** (which DLC sheet). The table records carry the
full tileId (group included); resident `WorldMapTile`s + their map-space `cx` cluster per group
(g0 cx~4352-5120, g1 ~2816-4352, g2 ~0-1792), so derive `group` from the marker's projected map-space
u (or try g∈{0,1,2} and take the one present in the table — what `reveal_correlate.py` does).
Gate next to the existing `marker_fogged` in `map_renderer.cpp`.

### tools added
`D:\ghidra_scripts\{mask_enum,reveal_oracle,reveal_correlate}.py`, `find_unsearched.java`.
