# No-bake scoreboard — markers by provenance

**Goal: zero baked.** Every marker should come from the live mod files (`DiskMSB`) or live game memory (`Live`), never the static `goblin_map_data` bake. This doc is the versioned baseline — after a change, rerun `tools/nobake_scoreboard.py` and `git diff` this file to see **regressions (baked ↑)** or **progress (baked ↓)**. Rows sorted by category name (stable) so a count change touches only its own row.

- **Source**: runtime `[COVERAGE]` log (ERR profile), build 2026-06-27 03:50:02.845
- **`live-cls`** = category resolved via the live `classify_item_live` fallback (item the baked table didn't know).
- `disk`/`live` counts are **per-placement** (collectibles emit one marker per world node) → `total` is not directly comparable to deduped baked counts. For the migration what matters is **does a category still have baked>0**.
- **`drawn`** = real markers the renderer draws (= total). **`census`** = the ImGui badge denominator (completable spots) — distinct collect flags for flag-based categories, row count for geom/SFX pieces, 0 for graces; it EXCLUDES respawnable flag-less gather, so `census < drawn` wherever markers share a flag or respawn.
- **`flag`** = collect-flag coverage `flagged/drawn`: markers carrying a collect/cleared flag (can be collect-tracked / gray out) vs not. The flag-less split into **`respawn`** (lot-backed respawnable gather, no permanent done-state) and **`nonloot`** (TYPES with no collect flag — NPC/stake/spring/region). A big `nonloot` flags a feature whose objects can never complete on the map.
- **`icon`** = how the category's marker is drawn: **`symbol`** = native GPU map symbol (crisp); **`atlas N%`** = baked atlas CPU cell, N% non-transparent (a trailing ⚠ = faint/small <25%, renders but easy to miss — the Stakes class of "looks broken but isn't"; bump `overlay_icon_scale`); **`circle`** = flat coloured disc; **`none`** = key set but absent from the atlas. Item categories (Equipment/Key/Loot/Magic) also draw the live GPU inventory icon, so their atlas % is only the fallback — low % matters most for atlas-only World features.
- Graces are `Live` (BonfireWarpParam) but tallied separately in GraceLayer — not in this table.

## ▶ Baked markers remaining

# **0**  ← drive this to **0**

| | baked | disk | live | live-cls | total |
|---|--:|--:|--:|--:|--:|
| **all categories** | **0** | 8392 | 469 | 74 | 8861 |

🔴 baked-only: **0**  ·  🟡 partial: **0**  ·  🟢 off-bake: **61**  (of 61 active categories)

## Tile coverage (`_00`-only parser)

The disk pass parses only **`_00`** tiles (LOD0). It reads **651 / 964** tiles; the **313** non-`_00` are skipped. `_01`/`_02` are LOD connect-proxies (proxy objects at a 128/256 offset → the Stakes/Imp phantom source); `_10`/`_11`/`_12` hold mostly GED-tier **duplicates** of `_00` (e.g. the 3 Hostile-NPC invaders the bake double-counted), so skipping them loses ~no unique markers. `tools/tier_coverage.py` audits per-tier unique content.

| tier | files | role |
|---|--:|---|
| `_00` | 651 | parsed (LOD0 content) |
| `_01` | 180 | skipped — LOD proxy |
| `_02` | 61 | skipped — LOD proxy |
| `_10` | 36 | skipped — mostly _00 dupes |
| `_11` | 11 | skipped — mostly _00 dupes |
| `_12` | 5 | skipped — mostly _00 dupes |
| `_99` | 20 | skipped — lighting |

## Per category

| category | baked | disk | live | live-cls | total | icon | status |
|---|--:|--:|--:|--:|--:|---|---|
| Equipment - Armaments | 0 | 329 | 0 | 0 | 329 | atlas 69% | 🟢 off-bake |
| Equipment - Armour | 0 | 398 | 0 | 0 | 398 | atlas 69% | 🟢 off-bake |
| Equipment - Ashes of War | 0 | 80 | 0 | 0 | 80 | atlas 69% | 🟢 off-bake |
| Equipment - Spirits | 0 | 86 | 0 | 0 | 86 | atlas 69% | 🟢 off-bake |
| Equipment - Talismans | 0 | 147 | 0 | 0 | 147 | circle | 🟢 off-bake |
| Key - Celestial Dew | 0 | 9 | 0 | 0 | 9 | atlas 69% | 🟢 off-bake |
| Key - Cookbooks | 0 | 88 | 0 | 0 | 88 | atlas 69% | 🟢 off-bake |
| Key - Crystal Tears | 0 | 38 | 0 | 0 | 38 | atlas 69% | 🟢 off-bake |
| Key - Great Runes | 0 | 0 | 6 | 0 | 6 | atlas 69% | 🟢 off-bake |
| Key - Imbued Sword Keys | 0 | 4 | 0 | 0 | 4 | atlas 69% | 🟢 off-bake |
| Key - Larval Tears | 0 | 23 | 0 | 0 | 23 | atlas 69% | 🟢 off-bake |
| Key - Lost Ashes | 0 | 81 | 0 | 0 | 81 | atlas 69% | 🟢 off-bake |
| Key - Pots n Perfumes | 0 | 40 | 0 | 0 | 40 | atlas 69% | 🟢 off-bake |
| Key - Scadutree Fragments | 0 | 49 | 0 | 0 | 49 | atlas 30% | 🟢 off-bake |
| Key - Seeds Tears Ashes | 0 | 79 | 0 | 0 | 79 | atlas 69% | 🟢 off-bake |
| Key - Whetblades | 0 | 5 | 0 | 0 | 5 | circle | 🟢 off-bake |
| Loot - Ammo | 0 | 82 | 0 | 0 | 82 | atlas 69% | 🟢 off-bake |
| Loot - Bell-Bearings | 0 | 65 | 0 | 0 | 65 | atlas 69% | 🟢 off-bake |
| Loot - Consumables | 0 | 233 | 0 | 0 | 233 | atlas 69% | 🟢 off-bake |
| Loot - Crafting Materials | 0 | 1749 | 0 | 47 | 1749 | atlas 69% | 🟢 off-bake |
| Loot - Dragon Hearts | 0 | 20 | 0 | 0 | 20 | atlas 69% | 🟢 off-bake |
| Loot - Gestures | 0 | 7 | 0 | 0 | 7 | atlas 69% | 🟢 off-bake |
| Loot - Gloveworts | 0 | 286 | 0 | 0 | 286 | atlas 69% | 🟢 off-bake |
| Loot - Golden Runes | 0 | 226 | 0 | 0 | 226 | atlas 69% | 🟢 off-bake |
| Loot - Golden Runes (Low) | 0 | 446 | 0 | 0 | 446 | atlas 69% | 🟢 off-bake |
| Loot - Greases | 0 | 139 | 0 | 0 | 139 | atlas 69% | 🟢 off-bake |
| Loot - Great Gloveworts | 0 | 24 | 0 | 0 | 24 | atlas 69% | 🟢 off-bake |
| Loot - MP-Fingers | 0 | 10 | 0 | 0 | 10 | atlas 69% | 🟢 off-bake |
| Loot - Prattling Pates | 0 | 9 | 0 | 0 | 9 | atlas 69% | 🟢 off-bake |
| Loot - Rada Fruit | 0 | 21 | 0 | 0 | 21 | atlas 69% | 🟢 off-bake |
| Loot - Reusables | 0 | 11 | 0 | 0 | 11 | atlas 69% | 🟢 off-bake |
| Loot - Rune Arcs | 0 | 75 | 0 | 0 | 75 | atlas 69% | 🟢 off-bake |
| Loot - Smithing Stones | 0 | 324 | 0 | 0 | 324 | atlas 69% | 🟢 off-bake |
| Loot - Smithing Stones (Low) | 0 | 403 | 0 | 0 | 403 | atlas 69% | 🟢 off-bake |
| Loot - Smithing Stones (Rare) | 0 | 11 | 0 | 0 | 11 | atlas 69% | 🟢 off-bake |
| Loot - Stat Boosts | 0 | 86 | 0 | 0 | 86 | atlas 69% | 🟢 off-bake |
| Loot - Stonesword Keys | 0 | 48 | 0 | 0 | 48 | atlas 69% | 🟢 off-bake |
| Loot - Throwables | 0 | 125 | 0 | 0 | 125 | circle | 🟢 off-bake |
| Loot - Utilities | 0 | 47 | 0 | 0 | 47 | circle | 🟢 off-bake |
| Magic - Incantations | 0 | 77 | 0 | 0 | 77 | atlas 69% | 🟢 off-bake |
| Magic - Memory Stones | 0 | 6 | 0 | 0 | 6 | atlas 69% | 🟢 off-bake |
| Magic - Prayerbooks | 0 | 17 | 0 | 0 | 17 | atlas 69% | 🟢 off-bake |
| Magic - Sorceries | 0 | 80 | 0 | 0 | 80 | atlas 69% | 🟢 off-bake |
| Quest - Deathroot | 0 | 9 | 0 | 0 | 9 | atlas 69% | 🟢 off-bake |
| Quest - Progression | 0 | 75 | 0 | 27 | 75 | atlas 69% | 🟢 off-bake |
| Quest - Seedbed Curses | 0 | 7 | 0 | 0 | 7 | atlas 69% | 🟢 off-bake |
| Reforged - Ember Pieces | 0 | 302 | 0 | 0 | 302 | atlas 30% | 🟢 off-bake |
| Reforged - Fortunes | 0 | 62 | 0 | 0 | 62 | atlas 69% | 🟢 off-bake |
| Reforged - Items | 0 | 75 | 0 | 0 | 75 | atlas 69% | 🟢 off-bake |
| Reforged - Rune Pieces | 0 | 1244 | 0 | 0 | 1244 | atlas 30% | 🟢 off-bake |
| World - Bosses | 0 | 0 | 217 | 0 | 217 | symbol | 🟢 off-bake |
| World - Hostile NPC | 0 | 50 | 0 | 0 | 50 | atlas 83% | 🟢 off-bake |
| World - Imp Statues | 0 | 37 | 0 | 0 | 37 | atlas 30% | 🟢 off-bake |
| World - Interactables | 0 | 103 | 0 | 0 | 103 | atlas 74% | 🟢 off-bake |
| World - Kindling Spirits | 0 | 5 | 0 | 0 | 5 | atlas 69% | 🟢 off-bake |
| World - Maps | 0 | 24 | 0 | 0 | 24 | circle | 🟢 off-bake |
| World - Paintings | 0 | 11 | 0 | 0 | 11 | atlas 65% | 🟢 off-bake |
| World - Spirit Springs | 0 | 72 | 0 | 0 | 72 | atlas 96% | 🟢 off-bake |
| World - Spiritspring Hawks | 0 | 14 | 0 | 0 | 14 | atlas 69% | 🟢 off-bake |
| World - Stakes of Marika | 0 | 219 | 0 | 0 | 219 | atlas 19% ⚠ | 🟢 off-bake |
| World - Summoning Pools | 0 | 0 | 246 | 0 | 246 | atlas 69% | 🟢 off-bake |

## Census (badge vs drawn) + collect-flag coverage

`drawn` = markers drawn · `census` = completable-spots badge · `flag` = flagged/drawn · `respawn`/`nonloot` = the flag-less split. A large `nonloot` marks a feature whose objects carry no collect flag (can't gray/complete). Categories missing here weren't in the census log.

`flag-src` (#3) = of the flagged markers, where the live collect-flag came from: **live** (drift-proof game memory) · **disk** (read from the mod's real files) · **baked** (the static-bake fallback = stale-risk). `flag-baked` should trend to 0 alongside the baked total — it IS the residual that hasn't migrated.

| category | drawn | census | flag (have/drawn) | respawn | nonloot | flag-live | flag-disk | flag-baked |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| Equipment - Armaments | 329 | 289 | 329/329 | 0 | 0 | 0 | 329 | 0 |
| Equipment - Armour | 398 | 153 | 398/398 | 0 | 0 | 0 | 398 | 0 |
| Equipment - Ashes of War | 80 | 80 | 80/80 | 0 | 0 | 0 | 80 | 0 |
| Equipment - Spirits | 86 | 80 | 86/86 | 0 | 0 | 0 | 86 | 0 |
| Equipment - Talismans | 147 | 142 | 147/147 | 0 | 0 | 0 | 147 | 0 |
| Key - Celestial Dew | 9 | 8 | 9/9 | 0 | 0 | 0 | 9 | 0 |
| Key - Cookbooks | 88 | 86 | 88/88 | 0 | 0 | 0 | 88 | 0 |
| Key - Crystal Tears | 38 | 38 | 38/38 | 0 | 0 | 0 | 38 | 0 |
| Key - Great Runes | 6 | 6 | 6/6 | 0 | 0 | 6 | 0 | 0 |
| Key - Imbued Sword Keys | 4 | 4 | 4/4 | 0 | 0 | 0 | 4 | 0 |
| Key - Larval Tears | 23 | 23 | 23/23 | 0 | 0 | 0 | 23 | 0 |
| Key - Lost Ashes | 81 | 81 | 81/81 | 0 | 0 | 0 | 81 | 0 |
| Key - Pots n Perfumes | 40 | 40 | 40/40 | 0 | 0 | 0 | 40 | 0 |
| Key - Scadutree Fragments | 49 | 49 | 49/49 | 0 | 0 | 0 | 49 | 0 |
| Key - Seeds Tears Ashes | 79 | 78 | 79/79 | 0 | 0 | 0 | 79 | 0 |
| Key - Whetblades | 5 | 5 | 5/5 | 0 | 0 | 0 | 5 | 0 |
| Loot - Ammo | 82 | 82 | 82/82 | 0 | 0 | 0 | 82 | 0 |
| Loot - Bell-Bearings | 65 | 57 | 65/65 | 0 | 0 | 0 | 65 | 0 |
| Loot - Consumables | 233 | 230 | 232/233 | 1 | 0 | 0 | 232 | 0 |
| Loot - Crafting Materials | 1749 | 624 | 640/1749 | 1109 | 0 | 0 | 640 | 0 |
| Loot - Dragon Hearts | 20 | 20 | 20/20 | 0 | 0 | 0 | 20 | 0 |
| Loot - Gestures | 7 | 6 | 7/7 | 0 | 0 | 0 | 7 | 0 |
| Loot - Gloveworts | 286 | 53 | 54/286 | 232 | 0 | 0 | 54 | 0 |
| Loot - Golden Runes | 226 | 222 | 226/226 | 0 | 0 | 0 | 226 | 0 |
| Loot - Golden Runes (Low) | 446 | 443 | 446/446 | 0 | 0 | 0 | 446 | 0 |
| Loot - Greases | 139 | 135 | 139/139 | 0 | 0 | 0 | 139 | 0 |
| Loot - Great Gloveworts | 24 | 21 | 21/24 | 3 | 0 | 0 | 21 | 0 |
| Loot - MP-Fingers | 10 | 9 | 10/10 | 0 | 0 | 0 | 10 | 0 |
| Loot - Prattling Pates | 9 | 9 | 9/9 | 0 | 0 | 0 | 9 | 0 |
| Loot - Rada Fruit | 21 | 21 | 21/21 | 0 | 0 | 0 | 21 | 0 |
| Loot - Reusables | 11 | 11 | 11/11 | 0 | 0 | 0 | 11 | 0 |
| Loot - Rune Arcs | 75 | 73 | 75/75 | 0 | 0 | 0 | 75 | 0 |
| Loot - Smithing Stones | 324 | 243 | 249/324 | 75 | 0 | 0 | 249 | 0 |
| Loot - Smithing Stones (Low) | 403 | 346 | 355/403 | 48 | 0 | 0 | 355 | 0 |
| Loot - Smithing Stones (Rare) | 11 | 11 | 11/11 | 0 | 0 | 0 | 11 | 0 |
| Loot - Stat Boosts | 86 | 86 | 86/86 | 0 | 0 | 0 | 86 | 0 |
| Loot - Stonesword Keys | 48 | 48 | 48/48 | 0 | 0 | 0 | 48 | 0 |
| Loot - Throwables | 125 | 122 | 125/125 | 0 | 0 | 0 | 125 | 0 |
| Loot - Utilities | 47 | 47 | 47/47 | 0 | 0 | 0 | 47 | 0 |
| Magic - Incantations | 77 | 72 | 77/77 | 0 | 0 | 0 | 77 | 0 |
| Magic - Memory Stones | 6 | 6 | 6/6 | 0 | 0 | 0 | 6 | 0 |
| Magic - Prayerbooks | 17 | 14 | 17/17 | 0 | 0 | 0 | 17 | 0 |
| Magic - Sorceries | 80 | 75 | 80/80 | 0 | 0 | 0 | 80 | 0 |
| Quest - Deathroot | 9 | 9 | 9/9 | 0 | 0 | 0 | 9 | 0 |
| Quest - Progression | 75 | 59 | 75/75 | 0 | 0 | 0 | 75 | 0 |
| Quest - Seedbed Curses | 7 | 6 | 7/7 | 0 | 0 | 0 | 7 | 0 |
| Reforged - Ember Pieces | 302 | 302 | 23/302 | 0 | 279 | 0 | 23 | 0 |
| Reforged - Fortunes | 62 | 9 | 62/62 | 0 | 0 | 0 | 62 | 0 |
| Reforged - Items | 75 | 58 | 75/75 | 0 | 0 | 0 | 75 | 0 |
| Reforged - Rune Pieces | 1244 | 1244 | 126/1244 | 0 | 1118 | 0 | 126 | 0 |
| World - Bosses | 217 | 215 | 217/217 | 0 | 0 | 217 | 0 | 0 |
| World - Hostile NPC | 50 | 26 | 27/50 | 0 | 23 | 0 | 27 | 0 |
| World - Imp Statues | 37 | 37 | 37/37 | 0 | 0 | 0 | 37 | 0 |
| World - Interactables | 103 | 102 | 102/103 | 0 | 1 | 0 | 102 | 0 |
| World - Kindling Spirits | 5 | 5 | 5/5 | 0 | 0 | 0 | 5 | 0 |
| World - Maps | 24 | 24 | 24/24 | 0 | 0 | 0 | 24 | 0 |
| World - Paintings | 11 | 11 | 11/11 | 0 | 0 | 0 | 11 | 0 |
| World - Spirit Springs | 72 | 0 | 0/72 | 0 | 72 | 0 | 0 | 0 |
| World - Spiritspring Hawks | 14 | 14 | 14/14 | 0 | 0 | 0 | 14 | 0 |
| World - Stakes of Marika | 219 | 0 | 0/219 | 0 | 219 | 0 | 0 | 0 |
| World - Summoning Pools | 246 | 246 | 246/246 | 0 | 0 | 246 | 0 | 0 |
| **all** | | | | | | **469** | **5212** | **0** |

_Collect-flag provenance: **469** live · **5212** disk · **0** baked (baked = the un-migrated residual — should equal the baked-loot total)._

## Skipped — disk placements parsed but NOT drawn (#2)

The inverse of the coverage table: of everything the disk passes parsed from the mod's files, how many became markers vs were filtered, grouped by **why**. `unclassified` + `no_anchor` are the **real coverage gaps** (an item we couldn't categorise / place); the rest are **correct** skips (already placed, by-design suppressed, bake phantoms, cut assets).

**TOTAL skipped 510087** vs **drawn 8861**.

| reason | count | kind | meaning |
|---|--:|---|---|
| `unclassified` | 883 | ⚠️ gap | item type unknown → no category |
| `no_anchor` | 8810 | ⚠️ gap | no lot / no positionable MSB entity |
| `dedup` | 973 | ✅ correct | already placed by another pass |
| `by_design` | 499086 | ✅ correct | rune/ember GEOM-tracked, anti-flood clutter, respawnable no-flag enemy drops |
| `merchant_phantom` | 19 | ✅ correct | shop-∞ bake fallback, dropped |
| `dummy_inert` | 316 | ✅ correct | cut DummyAsset, no EntityID |

## MapGenie coverage gaps — categories not parsed (#1)

Vs MapGenie (base+DLC). Full table: `docs/coverage_vs_mapgenie.md`. **31 categories NOT WIRED** (no mod category at all) · **17 DRIFT** (drawn < MapGenie, partly ERR≠vanilla).

**❌ NOT WIRED:** Martyr Effigy (212), Elite Enemy (184), Landmark (172), Character (127), Key Item (generic) (85), Enemy (82), Dungeon (64), Hidden Passage (59), Ghost (57), Merchant (43), Elevator (40), Crimson Scarab (40), Portal (39), Tool (30), Remembrance (25), Cerulean Scarab (21), Miquella's Cross (13), Item (generic) (13), Evergaol (11), Minor Erdtree (11), Miscellaneous (9), Legacy Dungeon (7), Wandering Mausoleum (7), Quest (steps) (7), Divine Tower (6), Lore (6), Stone Cairn (5), Talisman Pouch (3), Dragon Shrine (2), Smithing Table (1), Trainer (1)

