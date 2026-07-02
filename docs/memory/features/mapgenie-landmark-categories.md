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

## PARITY families — 9 new WMPP landmark categories (2026-07-02, `feat/mapgenie-landmark-parity`)

Origin: user asked "which native pins does the game still draw that WE don't re-draw?" (parity view,
also feeds the future native-pin suppression set). Full audit of `data/WorldMapPointParam.json`
(740 rows, ERR dump) grouped by iconId × PlaceName: **every WMPP pin is eventFlag-gated natively**
(shows only once discovered) — our overlay showing everything is the parity win. The audit's
uncovered families became 9 new categories (same `build_live_landmarks` pass, contiguous enum block
now `WorldDivineTower .. WorldUniqueSite`; the `[LANDMARKLIVE]` log is now a loop over the block):

| Category | iconIds | ~rows |
|---|---|---|
| `WorldChurch`      | 3, 20, 247, 248, 249 | 28 |
| `WorldRuins`       | 5, 47, 250–255 | 38 |
| `WorldRiseTower`   | 8, 17, 68, 258 | 21 |
| `WorldShack`       | 6, 259 | 24 |
| `WorldFort`        | 18, 242, 243 | 7 |
| `WorldCastle`      | 25–29, 241 | 6 |
| `WorldTownVillage` | 32–40, 244, 245, 246, 261 | 14 |
| `WorldColosseum`   | 24 (single → `category_gpu_iconId` glyph entry added) | 3 |
| `WorldUniqueSite`  | 10, 11, 43, 45, 46, 52, 53, 54, 57, 88, 217, 232, 240, 256, 257, 260 | 26 |

Plus: iconId **62** (Ashen Leyndell) added to `WorldLegacyDungeon`. Deliberately skipped: 41/67
(boss pass), 80 (graces), 83/84/85 (structural no-text), **42** (legacy-dungeon sub-zone nav labels,
29 rows — revisit if wanted), **87** (Volcano Manor request markers, dynamic), **0** (ERR-custom
arena rows). `coverage_vs_mapgenie.py`: MapGenie's editorial "Landmark" row now sums the 9.

**Suppression implication (HANDOFF item):** the native-pin suppression set = exactly the iconIds our
categories re-draw — now the Group 1 + parity union, so implement suppression AFTER this merges.

## Loot - Farmable Drops (`WorldFarmableCollectible`, MFG-original) — shipped 2026-07-01

Marks farm spots for notable upgrade mats. In `build_disk_enemy_markers` the respawning drops
(`resolve_loot_flag==0`, previously skipped as `no-one-time-flag`) are now emitted under
`WorldFarmableCollectible` IFF the lot contains a notable farm target (`is_notable_farmable_category`:
Smithing Stones ×3 / Golden Runes ×2 / Gloveworts ×2). Trash drops still skip → no flood. **Key gotcha:
the notable item is usually in lot SLOT 2** (slot 1 = a craft material), so a slot-1-only read
(`resolve_loot_item_textid`) found 0 — fixed by `goblin::lot_slot_item_keys` (all 8 slots). Marker is
emitted NON-lot-backed with `textId1` = the notable item's GoodsName key (kindling pattern; resolves via
the `id>=500M` decode). Deduped per lot → **70 markers on ERR** (`[LOOTDISK] … farmable-notable`).
`WorldFarmableEnemy` deliberately dropped (floods, no boss filter).

### Potential future adjustments (not done — tune if the user wants)
- **Notable set** (`is_notable_farmable_category` in `map_entry_layer.cpp`): currently Smithing Stones /
  Golden Runes / Gloveworts. Golden Runes are the noisiest — drop them if 70 feels too busy; or ADD Rune
  Arcs / Rada Fruit / specific crafting mats if the user wants more farm targets.
- **Per-item icons**: all 70 share the category rep icon (`[CATICON] iconId 145`) because the markers are
  non-lot (`item_icon_id=0`). To draw each marker's own item icon, either resolve it from the notable key
  (`item_real_icon_id(notable_key)`) and set `m.item_icon_id`, or keep it lot-backed and teach
  `push_marker` to prefer the notable slot. The NAME per marker is already correct.
- **Dedup granularity**: deduped per LOT → one marker at a representative enemy position. A common lot
  placed in several areas shows at ONE spot. For "all farm locations" switch to per-(lot,tile) or
  per-projected-cell dedup — but that risks flooding for common lots; keep the per-lot cap unless asked.
- **Map gather nodes** are NOT in this category (they already draw under Material Nodes / Crafting
  Materials). Only enemy drops. Re-routing them here was explicitly rejected (would remove them from
  their item categories).

## Deferred (user decision, 2026-07-01)

The 2 MFG-original respawn categories `WorldFarmableEnemy` + `WorldFarmableCollectible` were
**deferred**. `WorldFarmableEnemy` has no clean live per-enemy "boss" signal (npcType/teamType aren't
category keys; fog-gated bosses read `disableRespawn==0`), so a `dr==0` pass can't cleanly exclude
bosses and would flood the map. `WorldFarmableCollectible` is a routing decision (map gather nodes
already draw under their item category; farmable enemy drops are dropped at the `no-one-time-flag`
skip). Gate mechanics remain valid (`dr==0 ∧ non-boss`; `getItemFlagId==0 ∨ flag_is_repeatable`, both
already read live) — resume with a curated notability/boss filter. See
`docs/plans/mapgenie_category_coverage_plan.md` Progress + `[[handoff-loot-from-real-files]]`.
