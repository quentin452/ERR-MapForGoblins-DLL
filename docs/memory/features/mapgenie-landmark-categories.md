# MapGenie landmark categories (live WorldMapPointParam.iconId)

Status: **shipped on `feat/mapgenie-group1-landmarks`, cross-build clean, in-game verification pending.**

MapGenie category-coverage GROUP 1 landed the 6 landmark categories that are a clean
`WorldMapPointParam.iconId` key:

| Category enum | iconId key |
|---|---|
| `WorldDivineTower`   | 23 (6 rows) |
| `WorldEvergaol`      | 9 |
| `WorldMinorErdtree`  | 30 (11 rows) |
| `WorldGrandLift`     | 21 — Dectus + Rold ONLY (NOT MapGenie's 40 in-dungeon lifts, which are not WMPP) |
| `WorldDungeon`       | union {4 Catacombs, 13 Caves, 14 Tunnels, 15 Wells, 16 Hero's Graves, 230/231/234 DLC} |
| `WorldLegacyDungeon` | per-site {50,51,55,56,58,59,60,61,66,210,211,213,218} |

## How it works

`build_live_landmarks()` in `src/worldmap/map_entry_layer.cpp` (right after `build_live_bosses()`,
called unconditionally in `build_buckets_impl`) iterates the LIVE `WorldMapPointParam`, maps each
`row.iconId` via `landmark_category_for_icon()`, and `push_marker`s it. Same live-param source +
projection + name (textId1 → PlaceName) path as the boss pass → **mod-agnostic, zero bake.** iconId
semantics are byte-identical vanilla↔ERR (ERR only decorates the point TEXT with boss-status labels),
verified off-disk by `tools/verify_worldmap_iconids.py`. Zero overlap with the boss pass — none of
these iconIds carry `textId2==5100` (verified), so a landmark is never also a live boss marker.

Plumbing touched when adding categories (all in one commit): the `Category` enum
(`src/generated/goblin_map_data.hpp`, appended after `WorldInteractables`) + every `NUM_CATEGORIES`
sentinel retargeted from `WorldInteractables) + 1` to `WorldLegacyDungeon) + 1`
(`category_meta.cpp` static_assert, `goblin_config_schema.cpp`, `goblin_loot_resolve.cpp`,
`goblin_section_visibility.cpp`, `goblin_overlay_render.cpp`, `map_entry_layer.cpp`); `category_meta`
CAT rows; `section_of` (World); `category_name` display strings; 6 `show_*` config keys (default
false); `tools/coverage_vs_mapgenie.py`. No atlas glyph → circle fallback (G_WORLD teal) for now.

Expected in-game counts (ERR, `[LANDMARKLIVE]` log): DivineTower 6, Evergaol 16, MinorErdtree 11,
GrandLift 2, Dungeon 66, LegacyDungeon 13 (~114 total).

## Deferred (user decision, 2026-07-01)

The 2 MFG-original respawn categories `WorldFarmableEnemy` + `WorldFarmableCollectible` were
**deferred**. `WorldFarmableEnemy` has no clean live per-enemy "boss" signal (npcType/teamType aren't
category keys; fog-gated bosses read `disableRespawn==0`), so a `dr==0` pass can't cleanly exclude
bosses and would flood the map. `WorldFarmableCollectible` is a routing decision (map gather nodes
already draw under their item category; farmable enemy drops are dropped at the `no-one-time-flag`
skip). Gate mechanics remain valid (`dr==0 ∧ non-boss`; `getItemFlagId==0 ∨ flag_is_repeatable`, both
already read live) — resume with a curated notability/boss filter. See
`docs/plans/mapgenie_category_coverage_plan.md` Progress + `[[handoff-loot-from-real-files]]`.
