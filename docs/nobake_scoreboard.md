# No-bake scoreboard — markers by provenance

**Goal: zero baked.** Every marker should come from the live mod files (`DiskMSB`) or live game memory (`Live`), never the static `goblin_map_data` bake. This doc is the versioned baseline — after a change, rerun `tools/nobake_scoreboard.py` and `git diff` this file to see **regressions (baked ↑)** or **progress (baked ↓)**. Rows sorted by category name (stable) so a count change touches only its own row.

- **Source**: runtime `[COVERAGE]` log (ERR profile), build 2026-06-25 10:32:26.858
- **`live-cls`** = category resolved via the live `classify_item_live` fallback (item the baked table didn't know).
- `disk`/`live` counts are **per-placement** (collectibles emit one marker per world node) → `total` is not directly comparable to deduped baked counts. For the migration what matters is **does a category still have baked>0**.
- Graces are `Live` (BonfireWarpParam) but tallied separately in GraceLayer — not in this table.

## ▶ Baked markers remaining

# **2806**  ← drive this to **0**

| | baked | disk | live | live-cls | total |
|---|--:|--:|--:|--:|--:|
| **all categories** | **2806** | 8398 | 217 | 217 | 11421 |

🔴 baked-only: **15**  ·  🟡 partial: **31**  ·  🟢 off-bake: **17**  (of 63 active categories)

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
| Loot - Consumables | 1 | 231 | 0 | 52 | 232 | 🟡 partial |
| Loot - Crafting Materials | 5 | 2754 | 0 | 85 | 2759 | 🟡 partial |
| Loot - Dragon Hearts | 1 | 20 | 0 | 0 | 21 | 🟡 partial |
| Loot - Gestures | 7 | 0 | 0 | 0 | 7 | 🔴 baked-only |
| Loot - Gloveworts | 11 | 39 | 0 | 0 | 50 | 🟡 partial |
| Loot - Golden Runes | 9 | 217 | 0 | 0 | 226 | 🟡 partial |
| Loot - Golden Runes (Low) | 14 | 430 | 0 | 0 | 444 | 🟡 partial |
| Loot - Greases | 1 | 138 | 0 | 0 | 139 | 🟡 partial |
| Loot - Great Gloveworts | 0 | 20 | 0 | 0 | 20 | 🟢 off-bake |
| Loot - Material Nodes | 909 | 0 | 0 | 0 | 909 | 🔴 baked-only |
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
| Reforged - Ember Pieces | 19 | 410 | 0 | 0 | 429 | 🟡 partial |
| Reforged - Fortunes | 54 | 7 | 0 | 0 | 61 | 🟡 partial |
| Reforged - Items | 29 | 47 | 0 | 0 | 76 | 🟡 partial |
| Reforged - Rune Pieces | 114 | 1579 | 0 | 0 | 1693 | 🟡 partial |
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
| World - Stakes of Marika | 439 | 0 | 0 | 0 | 439 | 🔴 baked-only |
| World - Summoning Pools | 245 | 0 | 0 | 0 | 245 | 🔴 baked-only |

