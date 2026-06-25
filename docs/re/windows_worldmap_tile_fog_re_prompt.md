# RE brief — per-TILE world-map fog-of-war (the real "踏破"/discovered state)

## Why this exists (supersedes the fog_reveal_mask conclusion)

`docs/re/windows_fog_reveal_mask_re_findings.md` concluded the fog reveal = `WorldMapPieceParam`
+ `openEventFlagId`. **That is WRONG for fog-of-war.** Live proof (ERR 2.2.9.6, app 2.6.2.0):

```
[FOGPIECE] layer=10 rect=(3037,4606)-(5368,6988) flag=62080 flagSet=1   ← Gravesite map FRAGMENT
[FOGPIECE] layer=10 rect=(4915,1869)-(7880,5662) flag=62081 flagSet=1   ← ScaduAltus FRAGMENT
... every piece's openEventFlagId is a 62xxx MAP-FRAGMENT flag.
```

So `WorldMapPieceParam` = the **map-fragment region reveal** (un-grays the map ART once you own
the fragment). It is REDUNDANT with our existing `require_map_fragments` item gate. It is NOT the
walk-explored fog.

Observed ground truth: with **all 24 map fragments owned**, the native DLC map STILL shows
per-tile black fog-of-war — only **discovered tiles are non-black**, every other tile is fully
black (binary, whole-tile granularity). Our overlay markers leak into those black tiles because
nothing gates on the per-tile discovered state.

**Goal of this RE:** find the per-tile "discovered / revealed" boolean so the overlay can add a
3rd gate: `if (tile_fogged(area, gridX, gridZ)) cull marker`.

## What we already have (anchors — reuse, don't re-derive)

- Live `CS::WorldMapViewModel` resolved from the active map cursor (`goblin_worldmap_probe.cpp`,
  `find_vm`). Per-page converter array + page table `base+0x2ad82f8 = {0x00,0x01,0x0A}`.
- `from::params` live param read path (used for `WorldMapPointParam`, `WorldMapPieceParam`).
- `goblin::ui::read_event_flag(flagId)` (IS_EVENT_FLAG AOB).
- Tile coordinate space = `WorldMapPointParam.areaNo / gridXNo / gridZNo` (e.g. DLC overworld
  area 61, base overworld area 60, base underground area 12). Markers already carry
  `raw_area / raw_gx / raw_gz`.

## RTTI class names confirmed present in `eldenring.exe` (start here)

```
.?AVWorldMapTile@CS@@                       ← ONE tile
.?AVWorldMapTiledLayer@CS@@                 ← the tile grid/layer (holds the tile array)
.?AVWorldMapTileRes@CS@@                    ← tile resource (texture/streaming)
.?AVWorldMapTileBackReader@CS@@             ← reads tile data
._FrontActivateWaitJob@WorldMapTiledLayer  ← tiles ACTIVATE near player (streaming/discover?)
.?AVWorldMapArea@CS@@  .?AVWorldMapAreaConverter@CS@@
.?AVCSWorldMapDiscoveryPointIns@CS@@        ← "discovery" instance (point-level, maybe tile too)
CS::CSWorldMapPointManImplement::_DiscoverMapPoint   ← the discover WRITE path
DiscoveryMapPointName                       ← string near the discovery system
```

## Questions to answer (in priority order)

1. **The tile array.** From `CS::WorldMapTiledLayer` (find its vtable RTTI → static anchor, or
   reach it from the WorldMapViewModel/CSWorldMapMenu object we already resolve), where is the
   array/grid of `CS::WorldMapTile`? Size, stride, how it is indexed (by area? by linear
   gridX,gridZ within an area? one layer per page 0/1/10?).

2. **The per-tile coord key.** Inside `CS::WorldMapTile`, which fields hold its (area, gridX,
   gridZ) — so we can match a marker's `raw_area/raw_gx/raw_gz` to its tile.

3. **THE FLAG.** Which field on `CS::WorldMapTile` (or a parallel bitmask in the layer) is the
   **discovered / revealed / activated** boolean that drives the black fog? Offset + type. Verify
   by reading it live on a partially-explored save: player's current tile reads "revealed", a
   known-black far tile reads "fogged".
   - Distinguish "discovered" (permanent, persists after leaving) from "currently
     streamed/active" (`_FrontActivateWaitJob`). The fog is permanent → we want the persistent
     discovered bit, not the transient streaming state.

4. **A query function (preferred over raw struct walk).** Is there a
   `IsTileDiscovered(area,gx,gz)` / `GetTile(area,gx,gz)->discovered` we can call or mirror?
   Look at xrefs of `_DiscoverMapPoint` / `CSWorldMapDiscoveryPointIns` and whatever the fog
   draw pass reads to decide black-vs-show per tile (the tile rasterizer — compare with the
   `FUN_140884c50` piece rasterizer from the old findings doc; the per-tile fog draw is a
   DIFFERENT pass keyed on this bit).

5. **Persistence.** Is the discovered bitmask in save data (the `.err` map block) or pure
   runtime rebuilt on load? (We only need to READ it live, but knowing tells us when it's valid.)

## Deliverable

`docs/re/windows_worldmap_tile_fog_re_findings.md` with:
- The reach: static base → `WorldMapTiledLayer` → tile array → tile (area,gx,gz) + discovered
  flag offset, OR a callable `IsTileDiscovered`. AOBs for any function used; offsets for any
  struct walk (version-stability note).
- A `tile_fogged(area,gx,gz)` recipe the overlay can implement next to `marker_fogged`, plus the
  one calibration check (player tile = revealed, far tile = fogged) that proves it.

## Mod-side integration (already scaffolded — just needs the data)

`map_renderer.cpp` gate already computes `areaIdx` + has `raw_area/gx/gz` per marker. Add:
```cpp
if (goblin::config::requireMapFragments && goblin::tile_fogged(m.raw_area, m.raw_gx, m.raw_gz))
    continue;   // tile not yet discovered → black fog → hide marker
```
(Keep the existing item-fragment gate; this is the SEPARATE walk-fog gate the param read can't
provide.)
