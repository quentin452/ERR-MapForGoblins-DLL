# MapGenie landmark categories (live WorldMapPointParam.iconId)

Status: **shipped on `feat/mapgenie-group1-landmarks`, cross-build clean, IN-GAME CONFIRMED on ERR
(2026-07-01) — positions correct.** `[LANDMARKLIVE] built 114 markers (DivineTower 6, Evergaol 16,
MinorErdtree 11, GrandLift 2, Dungeon 66, LegacyDungeon 13)`, matching the off-disk counts exactly;
`[SIG] 29/29 clean`, `[BOSSLIVE] 217` unchanged (no overlap/interference), no crash. Not yet merged;
the non-ERR (vanilla via me3) mod-agnostic check is still open.

**Known followup — glyph (deferred, user "for later" 2026-07-01):** landmarks currently draw as the
plain teal circle (no `category_meta` icon key, no `category_gpu_iconId` entry). Each landmark marker
came from a WMPP row with a REAL iconId (23/9/30/21/4/13/…), which the disk `map_point_rect(iconId)`
path can resolve to its `SB_MapCursor` glyph (mod-agnostic). Simplest wire for the 4 single-value
categories: add `category_gpu_iconId` rows (DivineTower→23, Evergaol→9, MinorErdtree→30, GrandLift→21).
`WorldDungeon`/`WorldLegacyDungeon` are multi-iconId → they'd need the marker's OWN source iconId
plumbed through `push_marker` (the boss pass has the same limitation), so a per-marker glyph field is
the fuller fix. Circle is the correct universal fallback until then (prime directive).

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

## Miquella's Cross + GROUP 2 Portal (added same cycle)

- **`WorldMiquellaCross`** — DLC iconId **208** (13 rows), a clean WMPP key → reuses `build_live_landmarks`
  (its `per_cat` is now enum-derived `kLandmarkCount`, so the contiguous landmark block grows freely).
- **`WorldPortal`** — the FIRST MapGenie Group 2 (non-WMPP) category. A portal = an **`AEG099_510`**
  sending-gate asset whose EntityID is bound as **arg[2] of EMEVD warp template `90005605`**. That
  template binding is the mod-agnostic "actually warps the player" signal — it isolates the ~23 real
  gates from `AEG099_510`'s ~180 total placements (mostly decorative/anchor, incl. a 94-member Leyndell
  cluster). Runtime, no bake: `msbe::parse_emevd_portal_gates` harvests the gate entity set from the
  active install's `event/*.emevd` (shared with the world-feature flag scan in
  `load_emevd_world_feature_flags`); `build_disk_portal_markers` emits each `AEG099_510` disk asset
  (aegRow 99510) whose EntityID is in that set, deduped by entity across LOD tiles. Label = PlaceName
  6108700 "Sending Gate". No graying. Full RE: `docs/re/windows_portal_aeg_re_findings.md`
  (+ `tools/_probe_portal_{aeg,aeg2,emevd,verify}.py`). **Model + template facts are the reusable pattern
  for the rest of Group 2** — find the AEG model, find its EMEVD template binding, one harvest + one pass.

## Deferred (user decision, 2026-07-01)

The 2 MFG-original respawn categories `WorldFarmableEnemy` + `WorldFarmableCollectible` were
**deferred**. `WorldFarmableEnemy` has no clean live per-enemy "boss" signal (npcType/teamType aren't
category keys; fog-gated bosses read `disableRespawn==0`), so a `dr==0` pass can't cleanly exclude
bosses and would flood the map. `WorldFarmableCollectible` is a routing decision (map gather nodes
already draw under their item category; farmable enemy drops are dropped at the `no-one-time-flag`
skip). Gate mechanics remain valid (`dr==0 ∧ non-boss`; `getItemFlagId==0 ∨ flag_is_repeatable`, both
already read live) — resume with a curated notability/boss filter. See
`docs/plans/mapgenie_category_coverage_plan.md` Progress + `[[handoff-loot-from-real-files]]`.
