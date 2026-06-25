# No-bake scoreboard — markers by provenance

**Goal: zero baked.** Every marker should come from the live mod files (`DiskMSB`) or live game memory (`Live`), never the static `goblin_map_data` bake. This doc is the versioned baseline — after a change, rerun `tools/nobake_scoreboard.py` and `git diff` this file to see **regressions (baked ↑)** or **progress (baked ↓)**. Rows sorted by category name (stable) so a count change touches only its own row.

- **Source**: runtime `[COVERAGE]` log (ERR profile), build 2026-06-25 14:20:10.432
- **`live-cls`** = category resolved via the live `classify_item_live` fallback (item the baked table didn't know).
- `disk`/`live` counts are **per-placement** (collectibles emit one marker per world node) → `total` is not directly comparable to deduped baked counts. For the migration what matters is **does a category still have baked>0**.
- **`drawn`** = real markers the renderer draws (= total). **`census`** = the ImGui badge denominator (completable spots) — distinct collect flags for flag-based categories, row count for geom/SFX pieces, 0 for graces; it EXCLUDES respawnable flag-less gather, so `census < drawn` wherever markers share a flag or respawn.
- **`flag`** = collect-flag coverage `flagged/drawn`: markers carrying a collect/cleared flag (can be collect-tracked / gray out) vs not. The flag-less split into **`respawn`** (lot-backed respawnable gather, no permanent done-state) and **`nonloot`** (TYPES with no collect flag — NPC/stake/spring/region). A big `nonloot` flags a feature whose objects can never complete on the map.
- **`icon`** = how the category's marker is drawn: **`symbol`** = native GPU map symbol (crisp); **`atlas N%`** = baked atlas CPU cell, N% non-transparent (a trailing ⚠ = faint/small <25%, renders but easy to miss — the Stakes class of "looks broken but isn't"; bump `overlay_icon_scale`); **`circle`** = flat coloured disc; **`none`** = key set but absent from the atlas. Item categories (Equipment/Key/Loot/Magic) also draw the live GPU inventory icon, so their atlas % is only the fallback — low % matters most for atlas-only World features.
- Graces are `Live` (BonfireWarpParam) but tallied separately in GraceLayer — not in this table.

## ▶ Baked markers remaining

# **1174**  ← drive this to **0**

| | baked | disk | live | live-cls | total |
|---|--:|--:|--:|--:|--:|
| **all categories** | **1174** | 7205 | 463 | 186 | 8842 |

🔴 baked-only: **11**  ·  🟡 partial: **32**  ·  🟢 off-bake: **20**  (of 63 active categories)

## Per category

| category | baked | disk | live | live-cls | total | icon | status |
|---|--:|--:|--:|--:|--:|---|---|
| Equipment - Armaments | 34 | 356 | 0 | 78 | 390 | atlas 69% | 🟡 partial |
| Equipment - Armour | 129 | 117 | 0 | 0 | 246 | atlas 69% | 🟡 partial |
| Equipment - Ashes of War | 3 | 76 | 0 | 0 | 79 | atlas 69% | 🟡 partial |
| Equipment - Spirits | 2 | 78 | 0 | 0 | 80 | atlas 69% | 🟡 partial |
| Equipment - Talismans | 7 | 142 | 0 | 0 | 149 | circle | 🟡 partial |
| Key - Celestial Dew | 0 | 9 | 0 | 0 | 9 | atlas 69% | 🟢 off-bake |
| Key - Cookbooks | 3 | 85 | 0 | 0 | 88 | atlas 69% | 🟡 partial |
| Key - Crystal Tears | 8 | 31 | 0 | 0 | 39 | atlas 69% | 🟡 partial |
| Key - Great Runes | 6 | 0 | 0 | 0 | 6 | atlas 69% | 🔴 baked-only |
| Key - Imbued Sword Keys | 0 | 4 | 0 | 0 | 4 | atlas 69% | 🟢 off-bake |
| Key - Larval Tears | 5 | 15 | 0 | 0 | 20 | atlas 69% | 🟡 partial |
| Key - Lost Ashes | 29 | 52 | 0 | 0 | 81 | atlas 69% | 🟡 partial |
| Key - Pots n Perfumes | 1 | 39 | 0 | 0 | 40 | atlas 69% | 🟡 partial |
| Key - Scadutree Fragments | 0 | 45 | 0 | 0 | 45 | atlas 30% | 🟢 off-bake |
| Key - Seeds Tears Ashes | 1 | 74 | 0 | 0 | 75 | atlas 69% | 🟡 partial |
| Key - Whetblades | 0 | 5 | 0 | 0 | 5 | circle | 🟢 off-bake |
| Loot - Ammo | 3 | 0 | 0 | 0 | 3 | atlas 69% | 🔴 baked-only |
| Loot - Bell-Bearings | 0 | 50 | 0 | 0 | 50 | atlas 69% | 🟢 off-bake |
| Loot - Consumables | 1 | 185 | 0 | 6 | 186 | atlas 69% | 🟡 partial |
| Loot - Crafting Materials | 5 | 1695 | 0 | 100 | 1700 | atlas 69% | 🟡 partial |
| Loot - Dragon Hearts | 1 | 20 | 0 | 0 | 21 | atlas 69% | 🟡 partial |
| Loot - Gestures | 7 | 0 | 0 | 0 | 7 | atlas 69% | 🔴 baked-only |
| Loot - Gloveworts | 11 | 271 | 0 | 0 | 282 | atlas 69% | 🟡 partial |
| Loot - Golden Runes | 9 | 217 | 0 | 0 | 226 | atlas 69% | 🟡 partial |
| Loot - Golden Runes (Low) | 14 | 430 | 0 | 0 | 444 | atlas 69% | 🟡 partial |
| Loot - Greases | 1 | 138 | 0 | 0 | 139 | atlas 69% | 🟡 partial |
| Loot - Great Gloveworts | 0 | 23 | 0 | 0 | 23 | atlas 69% | 🟢 off-bake |
| Loot - Material Nodes | 11 | 0 | 0 | 0 | 11 | atlas 69% | 🔴 baked-only |
| Loot - MP-Fingers | 2 | 7 | 0 | 0 | 9 | atlas 69% | 🟡 partial |
| Loot - Prattling Pates | 0 | 9 | 0 | 0 | 9 | atlas 69% | 🟢 off-bake |
| Loot - Rada Fruit | 0 | 14 | 0 | 0 | 14 | atlas 69% | 🟢 off-bake |
| Loot - Reusables | 1 | 10 | 0 | 0 | 11 | atlas 69% | 🟡 partial |
| Loot - Rune Arcs | 0 | 75 | 0 | 0 | 75 | atlas 69% | 🟢 off-bake |
| Loot - Smithing Stones | 8 | 316 | 0 | 2 | 324 | atlas 69% | 🟡 partial |
| Loot - Smithing Stones (Low) | 20 | 385 | 0 | 0 | 405 | atlas 69% | 🟡 partial |
| Loot - Smithing Stones (Rare) | 0 | 11 | 0 | 0 | 11 | atlas 69% | 🟢 off-bake |
| Loot - Stat Boosts | 14 | 70 | 0 | 0 | 84 | atlas 69% | 🟡 partial |
| Loot - Stonesword Keys | 0 | 48 | 0 | 0 | 48 | atlas 69% | 🟢 off-bake |
| Loot - Throwables | 2 | 122 | 0 | 0 | 124 | circle | 🟡 partial |
| Loot - Utilities | 0 | 46 | 0 | 0 | 46 | circle | 🟢 off-bake |
| Magic - Incantations | 1 | 69 | 0 | 0 | 70 | atlas 69% | 🟡 partial |
| Magic - Memory Stones | 4 | 2 | 0 | 0 | 6 | atlas 69% | 🟡 partial |
| Magic - Prayerbooks | 0 | 17 | 0 | 0 | 17 | atlas 69% | 🟢 off-bake |
| Magic - Sorceries | 5 | 67 | 0 | 0 | 72 | atlas 69% | 🟡 partial |
| Quest - Deathroot | 0 | 9 | 0 | 0 | 9 | atlas 69% | 🟢 off-bake |
| Quest - Progression | 0 | 44 | 0 | 0 | 44 | atlas 69% | 🟢 off-bake |
| Quest - Seedbed Curses | 0 | 6 | 0 | 0 | 6 | atlas 69% | 🟢 off-bake |
| Reforged - Ember Pieces | 19 | 279 | 0 | 0 | 298 | atlas 30% | 🟡 partial |
| Reforged - Fortunes | 54 | 7 | 0 | 0 | 61 | atlas 69% | 🟡 partial |
| Reforged - Items | 29 | 47 | 0 | 0 | 76 | atlas 69% | 🟡 partial |
| Reforged - Rune Pieces | 114 | 1118 | 0 | 0 | 1232 | atlas 30% | 🟡 partial |
| World - Bosses | 0 | 0 | 217 | 0 | 217 | symbol | 🟢 off-bake |
| World - Hostile NPC | 53 | 0 | 0 | 0 | 53 | atlas 83% | 🔴 baked-only |
| World - Imp Statues | 0 | 37 | 0 | 0 | 37 | atlas 30% | 🟢 off-bake |
| World - Interactables | 88 | 14 | 0 | 0 | 102 | atlas 74% | 🟡 partial |
| World - Kindling Spirits | 5 | 0 | 0 | 0 | 5 | atlas 69% | 🔴 baked-only |
| World - Maps | 24 | 0 | 0 | 0 | 24 | circle | 🔴 baked-only |
| World - Paintings | 11 | 0 | 0 | 0 | 11 | atlas 65% | 🔴 baked-only |
| World - Quest NPC | 344 | 0 | 0 | 0 | 344 | circle | 🔴 baked-only |
| World - Spirit Springs | 71 | 0 | 0 | 0 | 71 | atlas 96% | 🔴 baked-only |
| World - Spiritspring Hawks | 14 | 0 | 0 | 0 | 14 | atlas 69% | 🔴 baked-only |
| World - Stakes of Marika | 0 | 219 | 0 | 0 | 219 | atlas 19% ⚠ | 🟢 off-bake |
| World - Summoning Pools | 0 | 0 | 246 | 0 | 246 | atlas 69% | 🟢 off-bake |

## Census (badge vs drawn) + collect-flag coverage

`drawn` = markers drawn · `census` = completable-spots badge · `flag` = flagged/drawn · `respawn`/`nonloot` = the flag-less split. A large `nonloot` marks a feature whose objects carry no collect flag (can't gray/complete). Categories missing here weren't in the census log.

| category | drawn | census | flag (have/drawn) | respawn | nonloot |
|---|--:|--:|--:|--:|--:|
| Equipment - Armaments | 390 | 341 | 369/390 | 21 | 0 |
| Equipment - Armour | 246 | 116 | 243/246 | 3 | 0 |
| Equipment - Ashes of War | 79 | 76 | 76/79 | 3 | 0 |
| Equipment - Spirits | 80 | 68 | 76/80 | 4 | 0 |
| Equipment - Talismans | 149 | 121 | 132/149 | 17 | 0 |
| Key - Celestial Dew | 9 | 8 | 9/9 | 0 | 0 |
| Key - Cookbooks | 88 | 85 | 88/88 | 0 | 0 |
| Key - Crystal Tears | 39 | 38 | 39/39 | 0 | 0 |
| Key - Great Runes | 6 | 6 | 6/6 | 0 | 0 |
| Key - Imbued Sword Keys | 4 | 3 | 3/4 | 1 | 0 |
| Key - Larval Tears | 20 | 12 | 12/20 | 8 | 0 |
| Key - Lost Ashes | 81 | 74 | 74/81 | 7 | 0 |
| Key - Pots n Perfumes | 40 | 40 | 40/40 | 0 | 0 |
| Key - Scadutree Fragments | 45 | 28 | 28/45 | 17 | 0 |
| Key - Seeds Tears Ashes | 75 | 67 | 68/75 | 7 | 0 |
| Key - Whetblades | 5 | 5 | 5/5 | 0 | 0 |
| Loot - Ammo | 3 | 3 | 3/3 | 0 | 0 |
| Loot - Bell-Bearings | 50 | 45 | 46/50 | 4 | 0 |
| Loot - Consumables | 186 | 173 | 176/186 | 10 | 0 |
| Loot - Crafting Materials | 1700 | 502 | 506/1700 | 1194 | 0 |
| Loot - Dragon Hearts | 21 | 20 | 21/21 | 0 | 0 |
| Loot - Gestures | 7 | 6 | 7/7 | 0 | 0 |
| Loot - Gloveworts | 282 | 40 | 40/282 | 242 | 0 |
| Loot - Golden Runes | 226 | 209 | 213/226 | 13 | 0 |
| Loot - Golden Runes (Low) | 444 | 425 | 428/444 | 16 | 0 |
| Loot - Greases | 139 | 127 | 131/139 | 8 | 0 |
| Loot - Great Gloveworts | 23 | 9 | 9/23 | 14 | 0 |
| Loot - Material Nodes | 11 | 11 | 0/11 | 0 | 11 |
| Loot - MP-Fingers | 9 | 9 | 9/9 | 0 | 0 |
| Loot - Prattling Pates | 9 | 9 | 9/9 | 0 | 0 |
| Loot - Rada Fruit | 14 | 14 | 14/14 | 0 | 0 |
| Loot - Reusables | 11 | 9 | 9/11 | 2 | 0 |
| Loot - Rune Arcs | 75 | 70 | 72/75 | 3 | 0 |
| Loot - Smithing Stones | 324 | 203 | 209/324 | 115 | 0 |
| Loot - Smithing Stones (Low) | 405 | 349 | 357/405 | 48 | 0 |
| Loot - Smithing Stones (Rare) | 11 | 11 | 11/11 | 0 | 0 |
| Loot - Stat Boosts | 84 | 79 | 80/84 | 4 | 0 |
| Loot - Stonesword Keys | 48 | 48 | 48/48 | 0 | 0 |
| Loot - Throwables | 124 | 111 | 113/124 | 11 | 0 |
| Loot - Utilities | 46 | 44 | 44/46 | 2 | 0 |
| Magic - Incantations | 70 | 61 | 63/70 | 7 | 0 |
| Magic - Memory Stones | 6 | 6 | 6/6 | 0 | 0 |
| Magic - Prayerbooks | 17 | 14 | 17/17 | 0 | 0 |
| Magic - Sorceries | 72 | 65 | 69/72 | 3 | 0 |
| Quest - Deathroot | 9 | 9 | 9/9 | 0 | 0 |
| Quest - Progression | 44 | 33 | 44/44 | 0 | 0 |
| Quest - Seedbed Curses | 6 | 5 | 6/6 | 0 | 0 |
| Reforged - Ember Pieces | 298 | 298 | 19/298 | 0 | 279 |
| Reforged - Fortunes | 61 | 8 | 61/61 | 0 | 0 |
| Reforged - Items | 76 | 58 | 76/76 | 0 | 0 |
| Reforged - Rune Pieces | 1232 | 1232 | 114/1232 | 0 | 1118 |
| World - Bosses | 217 | 215 | 217/217 | 0 | 0 |
| World - Hostile NPC | 53 | 42 | 53/53 | 0 | 0 |
| World - Imp Statues | 37 | 37 | 37/37 | 0 | 0 |
| World - Interactables | 102 | 102 | 102/102 | 0 | 0 |
| World - Kindling Spirits | 5 | 5 | 5/5 | 0 | 0 |
| World - Maps | 24 | 24 | 24/24 | 0 | 0 |
| World - Paintings | 11 | 11 | 11/11 | 0 | 0 |
| World - Quest NPC | 344 | 0 | 0/344 | 0 | 344 |
| World - Spirit Springs | 71 | 0 | 0/71 | 0 | 71 |
| World - Spiritspring Hawks | 14 | 14 | 14/14 | 0 | 0 |
| World - Stakes of Marika | 219 | 0 | 0/219 | 0 | 219 |
| World - Summoning Pools | 246 | 246 | 246/246 | 0 | 0 |

