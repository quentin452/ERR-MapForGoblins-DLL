---
name: worldmap-tile-fog-re
description: "REAL per-tile walk-fog lives in CS::WorldMapTiledLayer, NOT WorldMapPieceParam (that's fragment reveal); RE brief written, hunt pending"
metadata: 
  node_type: memory
  type: project
---

DLC "require map fragment doesn't work" investigation (2026-06-25) RESOLVED to a mislabel, not a bug.

**Root finding:** overlay has TWO marker gates — (a) item-fragment `read_event_flag` + (b) `marker_fogged` reading `WorldMapPieceParam.openEventFlagId`. Live dump (`[FOGPIECE]` diag) proved **every** piece flag is a 62xxx MAP-FRAGMENT flag (layer10 DLC = 62080-84). So gate (b) is REDUNDANT with (a) — both fire on fragment ownership. `WorldMapPieceParam` = map-fragment REGION reveal (un-grays art), NOT walk-explored fog. The old `windows_fog_reveal_mask_re_findings.md` "踏破エリア" label was WRONG (now annotated).

**The real fog** = per-TILE binary (whole tile black unless discovered; only discovered/active tiles non-black). Lives in `CS::WorldMapTiledLayer` (array of `CS::WorldMapTile`, activate near player via `_FrontActivateWaitJob`). Other exe RTTI anchors: `WorldMapTileRes`, `WorldMapTileBackReader`, `CSWorldMapDiscoveryPointIns`, `CSWorldMapPointManImplement::_DiscoverMapPoint`. Tile key = WorldMapPointParam (area,gridX,gridZ); markers carry raw_area/gx/gz.

**Decision:** RE the per-tile discovered bit (robust) over GPU backbuffer-readback black-pixel cull (fragile: sRGB/HDR fmt, dark-terrain false-pos; readback infra DOES exist — g_frames[].render_target backbuffer + record_sprite_copy fence pattern — kept as fallback).

**Pending:** RE brief at `docs/re/windows_worldmap_tile_fog_re_prompt.md` → find `WorldMapTiledLayer`→tile array→(area,gx,gz)+discovered-flag offset OR an `IsTileDiscovered` fn → wire `tile_fogged()` as 3rd gate in map_renderer.cpp. Needs Ghidra (not on Linux box). Diags `[DLCGATE]`+`[FOGPIECE]` still IN map_renderer.cpp/goblin_inject.cpp marked REMOVE-after. Tested on save with all 24 maps owned (can't reproduce frag-gate; proved fog-gate redundant). See [[overlay-rendered-markers]] [[page-transition-flicker]].
