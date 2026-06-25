# No-bake scoreboard — markers by provenance

**Goal: zero baked.** Every marker should come from the live mod files (`DiskMSB`) or live game memory (`Live`), never the static `goblin_map_data` bake. This doc is the versioned baseline — after a change, rerun `tools/nobake_scoreboard.py` and `git diff` this file to see **regressions (baked ↑)** or **progress (baked ↓)**. Rows sorted by category name (stable) so a count change touches only its own row.

- **Source**: runtime `[COVERAGE]` log (ERR profile), build 2026-06-25 12:17:58.977
- **`live-cls`** = category resolved via the live `classify_item_live` fallback (item the baked table didn't know).
- `disk`/`live` counts are **per-placement** (collectibles emit one marker per world node) → `total` is not directly comparable to deduped baked counts. For the migration what matters is **does a category still have baked>0**.
- **`drawn`** = real markers the renderer draws (= total). **`census`** = the ImGui badge denominator (completable spots) — distinct collect flags for flag-based categories, row count for geom/SFX pieces, 0 for graces; it EXCLUDES respawnable flag-less gather, so `census < drawn` wherever markers share a flag or respawn.
- **`flag`** = collect-flag coverage `flagged/drawn`: markers carrying a collect/cleared flag (can be collect-tracked / gray out) vs not. The flag-less split into **`respawn`** (lot-backed respawnable gather, no permanent done-state) and **`nonloot`** (TYPES with no collect flag — NPC/stake/spring/region). A big `nonloot` flags a feature whose objects can never complete on the map.
- Graces are `Live` (BonfireWarpParam) but tallied separately in GraceLayer — not in this table.

## ▶ Baked markers remaining

# **1469**  ← drive this to **0**

| | baked | disk | live | live-cls | total |
|---|--:|--:|--:|--:|--:|
| **all categories** | **1469** | 7157 | 217 | 186 | 8843 |

🔴 baked-only: **14**  ·  🟡 partial: **31**  ·  🟢 off-bake: **18**  (of 63 active categories)

## Per category

| category | baked | disk | live | live-cls | total | status |
|---|--:|--:|--:|--:|--:|---|
| Equipment - Armaments | 34 | 356 | 0 | 78 | 390 | 🟡 partial |
| Equipment - Armour | 129 | 117 | 0 | 0 | 246 | 🟡 partial |
| Equipment - Ashes of War | 3 | 76 | 0 | 0 | 79 | 🟡 partial |
| Equipment - Spirits | 2 | 78 | 0 | 0 | 80 | 🟡 partial |
| Equipment - Talismans | 7 | 142 | 0 | 0 | 149 | 🟡 partial |
| Key - Celestial Dew | 0 | 9 | 0 | 0 | 9 | 🟢 off-bake |
| Key - Cookbooks | 3 | 85 | 0 | 0 | 88 | 🟡 partial |
| Key - Crystal Tears | 8 | 31 | 0 | 0 | 39 | 🟡 partial |
| Key - Great Runes | 6 | 0 | 0 | 0 | 6 | 🔴 baked-only |
| Key - Imbued Sword Keys | 0 | 4 | 0 | 0 | 4 | 🟢 off-bake |
| Key - Larval Tears | 5 | 15 | 0 | 0 | 20 | 🟡 partial |
| Key - Lost Ashes | 29 | 52 | 0 | 0 | 81 | 🟡 partial |
| Key - Pots n Perfumes | 1 | 39 | 0 | 0 | 40 | 🟡 partial |
| Key - Scadutree Fragments | 0 | 45 | 0 | 0 | 45 | 🟢 off-bake |
| Key - Seeds Tears Ashes | 1 | 74 | 0 | 0 | 75 | 🟡 partial |
| Key - Whetblades | 0 | 5 | 0 | 0 | 5 | 🟢 off-bake |
| Loot - Ammo | 3 | 0 | 0 | 0 | 3 | 🔴 baked-only |
| Loot - Bell-Bearings | 0 | 50 | 0 | 0 | 50 | 🟢 off-bake |
| Loot - Consumables | 1 | 185 | 0 | 6 | 186 | 🟡 partial |
| Loot - Crafting Materials | 5 | 1697 | 0 | 100 | 1702 | 🟡 partial |
| Loot - Dragon Hearts | 1 | 20 | 0 | 0 | 21 | 🟡 partial |
| Loot - Gestures | 7 | 0 | 0 | 0 | 7 | 🔴 baked-only |
| Loot - Gloveworts | 11 | 271 | 0 | 0 | 282 | 🟡 partial |
| Loot - Golden Runes | 9 | 217 | 0 | 0 | 226 | 🟡 partial |
| Loot - Golden Runes (Low) | 14 | 430 | 0 | 0 | 444 | 🟡 partial |
| Loot - Greases | 1 | 138 | 0 | 0 | 139 | 🟡 partial |
| Loot - Great Gloveworts | 0 | 23 | 0 | 0 | 23 | 🟢 off-bake |
| Loot - Material Nodes | 11 | 0 | 0 | 0 | 11 | 🔴 baked-only |
| Loot - MP-Fingers | 2 | 7 | 0 | 0 | 9 | 🟡 partial |
| Loot - Prattling Pates | 0 | 9 | 0 | 0 | 9 | 🟢 off-bake |
| Loot - Rada Fruit | 0 | 14 | 0 | 0 | 14 | 🟢 off-bake |
| Loot - Reusables | 1 | 10 | 0 | 0 | 11 | 🟡 partial |
| Loot - Rune Arcs | 0 | 75 | 0 | 0 | 75 | 🟢 off-bake |
| Loot - Smithing Stones | 8 | 316 | 0 | 2 | 324 | 🟡 partial |
| Loot - Smithing Stones (Low) | 20 | 386 | 0 | 0 | 406 | 🟡 partial |
| Loot - Smithing Stones (Rare) | 0 | 11 | 0 | 0 | 11 | 🟢 off-bake |
| Loot - Stat Boosts | 14 | 70 | 0 | 0 | 84 | 🟡 partial |
| Loot - Stonesword Keys | 0 | 48 | 0 | 0 | 48 | 🟢 off-bake |
| Loot - Throwables | 2 | 122 | 0 | 0 | 124 | 🟡 partial |
| Loot - Utilities | 0 | 46 | 0 | 0 | 46 | 🟢 off-bake |
| Magic - Incantations | 1 | 69 | 0 | 0 | 70 | 🟡 partial |
| Magic - Memory Stones | 4 | 2 | 0 | 0 | 6 | 🟡 partial |
| Magic - Prayerbooks | 0 | 17 | 0 | 0 | 17 | 🟢 off-bake |
| Magic - Sorceries | 5 | 67 | 0 | 0 | 72 | 🟡 partial |
| Quest - Deathroot | 0 | 9 | 0 | 0 | 9 | 🟢 off-bake |
| Quest - Progression | 0 | 44 | 0 | 0 | 44 | 🟢 off-bake |
| Quest - Seedbed Curses | 0 | 6 | 0 | 0 | 6 | 🟢 off-bake |
| Reforged - Ember Pieces | 19 | 279 | 0 | 0 | 298 | 🟡 partial |
| Reforged - Fortunes | 54 | 7 | 0 | 0 | 61 | 🟡 partial |
| Reforged - Items | 29 | 47 | 0 | 0 | 76 | 🟡 partial |
| Reforged - Rune Pieces | 114 | 1118 | 0 | 0 | 1232 | 🟡 partial |
| World - Bosses | 0 | 0 | 217 | 0 | 217 | 🟢 off-bake |
| World - Hostile NPC | 53 | 0 | 0 | 0 | 53 | 🔴 baked-only |
| World - Imp Statues | 36 | 0 | 0 | 0 | 36 | 🔴 baked-only |
| World - Interactables | 102 | 0 | 0 | 0 | 102 | 🔴 baked-only |
| World - Kindling Spirits | 5 | 0 | 0 | 0 | 5 | 🔴 baked-only |
| World - Maps | 24 | 0 | 0 | 0 | 24 | 🔴 baked-only |
| World - Paintings | 11 | 0 | 0 | 0 | 11 | 🔴 baked-only |
| World - Quest NPC | 344 | 0 | 0 | 0 | 344 | 🔴 baked-only |
| World - Spirit Springs | 71 | 0 | 0 | 0 | 71 | 🔴 baked-only |
| World - Spiritspring Hawks | 14 | 0 | 0 | 0 | 14 | 🔴 baked-only |
| World - Stakes of Marika | 0 | 219 | 0 | 0 | 219 | 🟢 off-bake |
| World - Summoning Pools | 245 | 0 | 0 | 0 | 245 | 🔴 baked-only |

## Census (badge vs drawn) + collect-flag coverage

`drawn` = markers drawn · `census` = completable-spots badge · `flag` = flagged/drawn · `respawn`/`nonloot` = the flag-less split. A large `nonloot` marks a feature whose objects carry no collect flag (can't gray/complete). Categories missing here weren't in the census log.

| category | drawn | census | flag (have/drawn) | respawn | nonloot |
|---|--:|--:|--:|--:|--:|
| Equipment - Armaments | 390 | 362 | 390/390 | 0 | 0 |
| Equipment - Armour | 246 | 119 | 246/246 | 0 | 0 |
| Equipment - Ashes of War | 79 | 79 | 79/79 | 0 | 0 |
| Equipment - Spirits | 80 | 72 | 80/80 | 0 | 0 |
| Equipment - Talismans | 149 | 138 | 149/149 | 0 | 0 |
| Key - Celestial Dew | 9 | 8 | 9/9 | 0 | 0 |
| Key - Cookbooks | 88 | 85 | 88/88 | 0 | 0 |
| Key - Crystal Tears | 39 | 38 | 39/39 | 0 | 0 |
| Key - Great Runes | 6 | 6 | 6/6 | 0 | 0 |
| Key - Imbued Sword Keys | 4 | 4 | 4/4 | 0 | 0 |
| Key - Larval Tears | 20 | 20 | 20/20 | 0 | 0 |
| Key - Lost Ashes | 81 | 81 | 81/81 | 0 | 0 |
| Key - Pots n Perfumes | 40 | 40 | 40/40 | 0 | 0 |
| Key - Scadutree Fragments | 45 | 45 | 45/45 | 0 | 0 |
| Key - Seeds Tears Ashes | 75 | 74 | 75/75 | 0 | 0 |
| Key - Whetblades | 5 | 5 | 5/5 | 0 | 0 |
| Loot - Ammo | 3 | 3 | 3/3 | 0 | 0 |
| Loot - Bell-Bearings | 50 | 49 | 50/50 | 0 | 0 |
| Loot - Consumables | 186 | 183 | 186/186 | 0 | 0 |
| Loot - Crafting Materials | 1702 | 592 | 597/1702 | 1105 | 0 |
| Loot - Dragon Hearts | 21 | 20 | 21/21 | 0 | 0 |
| Loot - Gestures | 7 | 6 | 7/7 | 0 | 0 |
| Loot - Gloveworts | 282 | 50 | 50/282 | 232 | 0 |
| Loot - Golden Runes | 226 | 222 | 226/226 | 0 | 0 |
| Loot - Golden Runes (Low) | 444 | 441 | 444/444 | 0 | 0 |
| Loot - Greases | 139 | 135 | 139/139 | 0 | 0 |
| Loot - Great Gloveworts | 23 | 20 | 20/23 | 3 | 0 |
| Loot - Material Nodes | 11 | 11 | 0/11 | 0 | 11 |
| Loot - MP-Fingers | 9 | 9 | 9/9 | 0 | 0 |
| Loot - Prattling Pates | 9 | 9 | 9/9 | 0 | 0 |
| Loot - Rada Fruit | 14 | 14 | 14/14 | 0 | 0 |
| Loot - Reusables | 11 | 11 | 11/11 | 0 | 0 |
| Loot - Rune Arcs | 75 | 73 | 75/75 | 0 | 0 |
| Loot - Smithing Stones | 324 | 243 | 249/324 | 75 | 0 |
| Loot - Smithing Stones (Low) | 406 | 349 | 357/406 | 49 | 0 |
| Loot - Smithing Stones (Rare) | 11 | 11 | 11/11 | 0 | 0 |
| Loot - Stat Boosts | 84 | 83 | 84/84 | 0 | 0 |
| Loot - Stonesword Keys | 48 | 48 | 48/48 | 0 | 0 |
| Loot - Throwables | 124 | 122 | 124/124 | 0 | 0 |
| Loot - Utilities | 46 | 46 | 46/46 | 0 | 0 |
| Magic - Incantations | 70 | 68 | 70/70 | 0 | 0 |
| Magic - Memory Stones | 6 | 6 | 6/6 | 0 | 0 |
| Magic - Prayerbooks | 17 | 14 | 17/17 | 0 | 0 |
| Magic - Sorceries | 72 | 68 | 72/72 | 0 | 0 |
| Quest - Deathroot | 9 | 9 | 9/9 | 0 | 0 |
| Quest - Progression | 44 | 33 | 44/44 | 0 | 0 |
| Quest - Seedbed Curses | 6 | 5 | 6/6 | 0 | 0 |
| Reforged - Ember Pieces | 298 | 298 | 19/298 | 0 | 279 |
| Reforged - Fortunes | 61 | 8 | 61/61 | 0 | 0 |
| Reforged - Items | 76 | 58 | 76/76 | 0 | 0 |
| Reforged - Rune Pieces | 1232 | 1232 | 114/1232 | 0 | 1118 |
| World - Bosses | 217 | 215 | 217/217 | 0 | 0 |
| World - Hostile NPC | 53 | 42 | 53/53 | 0 | 0 |
| World - Imp Statues | 36 | 36 | 36/36 | 0 | 0 |
| World - Interactables | 102 | 102 | 102/102 | 0 | 0 |
| World - Kindling Spirits | 5 | 5 | 5/5 | 0 | 0 |
| World - Maps | 24 | 24 | 24/24 | 0 | 0 |
| World - Paintings | 11 | 11 | 11/11 | 0 | 0 |
| World - Quest NPC | 344 | 0 | 0/344 | 0 | 344 |
| World - Spirit Springs | 71 | 0 | 0/71 | 0 | 71 |
| World - Spiritspring Hawks | 14 | 14 | 14/14 | 0 | 0 |
| World - Stakes of Marika | 219 | 0 | 0/219 | 0 | 219 |
| World - Summoning Pools | 245 | 245 | 245/245 | 0 | 0 |

