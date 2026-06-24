# AEG collectible source — RE findings (2026-06-24)

How MapForGoblins places + identifies the mod's **collectible / gather assets** (Runic
Trace, Ember Trace, fireflies, butterflies, mosses, smithing stones, golden ore, ...)
**at runtime, from the active mod's real files — no bake, no manual model→item table.**
Shipped as config `loot_collectibles` (`src/worldmap/loot_disk`, `map_entry_layer`,
`goblin_inject::aeg_pickup_lot`).

## Pickup vs non-pickup asset
Every placed world object is an **MSB `Parts.Assets`** entry referencing a geometry
model named **`AEG{AAA}_{BBB}`** (e.g. `AEG099_821`, `AEG003_456`). The regulation param
**`AssetEnvironmentGeometryParam`** (ParamType `ASSET_GEOMETORY_PARAM_ST`, 320-byte rows,
24226 rows in ERR) has **one row per asset model**, row id = `AAA*1000 + BBB`
(`AEG099_821` → `99821`). It defines that model's geometry/physics/hit + pickup behaviour.

- **Pickup asset** — its row's field **`pickUpItemLotParamId` (s32 @ +0xb8)** is a real
  `ItemLotParam_map` id (`> 0`, `!= -1`). Interacting with / gathering the asset awards
  that lot. These are the collectibles, gather nodes, and ground items.
- **Non-pickup asset** — `pickUpItemLotParamId == -1` (`0xFFFFFFFF`; the default. `0` also
  = none). Trees, walls, rocks, structures, props — the vast majority. In ERR: of ~482k
  placed AEG assets, only ~19k are pickups, ~463k are non-pickups.

⚠️ The sentinel is **-1, not 0** — a reader that only rejects 0 treats every non-pickup
asset as a pickup with a bogus lot. (Cost us a bug: 467903 "unclassified". Reject both.)

## The deterministic identity chain (any mod, no manual matching)
```
placed Asset, part name "AEG{AAA}_{BBB}_xxxx"
  → AssetEnvironmentGeometryParam row (AAA*1000 + BBB)
  → pickUpItemLotParamId (@ +0xb8)                       [-1/0 ⇒ not a pickup, skip]
  → ItemLotParam_map[lot] slot-1 goods + category        (LIVE; resolve_loot_item_textid)
  → encode_live_item → ITEM_ICONS → MFG category + icon
```
Position = the placed Asset's block-local pos (`Part+0x20` X/Z, same transform as
treasures). Part-name prefix == model for all collectibles (no substitution), so no
modelIndex read is needed. Offset 0xb8 confirmed: paramdef field-sum = 0xb8 and matches
`DetectedSize=320`; raw row dump of pickup rows (99800/99821/99850) has the value at 0xb8,
non-pickup rows read 0xFFFFFFFF there. Ghidra: strings `AssetEnvironmentGeometryParam`
(er+0x2bb3070) + `ItemLotParam_map` (er+0x2bb29a8) present; asset-row range bound 99999
(0x1869f) as `CMP/MOV [reg+0x34],0x1869f`.

## Emit PER PLACEMENT (not per lot)
A single collectible **lot is shared across many world nodes** — e.g. all 505 Gaseous
Stones award lot `998500`. Each node is a distinct pickup at its own position, so the
builder emits **one marker per placement** and must NOT dedup by lot (that collapses 505 →
1; the original bug = only 119 markers). It only skips a placement whose lot is a real MSB
**Treasure** lot (the ground-item assets the treasure path already places). Each emitted
lot is still added to the lotId-coverage set so the baked-row replace drops any baked
duplicate.

## Hide-on-pickup
Via the standard **live loot flag** (`push_marker` lotType 1 → `resolve_loot_flag` →
`ItemLotParam.getItemFlagId`), same as disk treasures — NOT the GEOF piece-tracking
(`goblin_collected.cpp`, which is built from baked MAP_ENTRIES and doesn't see runtime
markers). Respawning nodes (`isEnableRepick == 1`, 74/285 models) have no persistent
collected flag → stay shown (correct, they respawn).

## Scope / coverage
ERR: 46 distinct `AEG099_8xx` collectible models, 3819 placed; the old pipeline mapped only
2 (rune 821 / ember 822) and even hardcoded the wrong goods (`800010` vs the param's real
`998210` "Runic Trace"). The runtime path covers ALL pickup models for ANY mod. Items not
in the baked `ITEM_ICONS` classifier are skipped (logged "unclassified") — extend the
classifier or add a default collectible category if those should show.

## Files / offline cross-check
`src/worldmap/msbe_parser.{hpp,cpp}` (wantAssets pass), `goblin_inject.cpp`
(`aeg_pickup_lot`), `src/worldmap/loot_disk.{hpp,cpp}` (`DiskCollectible`,
`disk_source_enabled`), `src/worldmap/map_entry_layer.cpp` (`build_disk_collectible_markers`).
Offline probes (diagnostic only, not wired): `tools/extract_err_collectibles.py` (positions),
`tools/extract_aeg099_mapping.py` (model→item, 285 rows). See [[aeg-collectible-source]],
[[handoff-loot-from-real-files]].
