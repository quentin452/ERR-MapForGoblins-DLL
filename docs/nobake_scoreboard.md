# No-bake scoreboard — markers by provenance

**Goal: zero baked.** Every marker should come from the live mod files (`DiskMSB`) or live game memory (`Live`), never the static `goblin_map_data` bake. This doc is the versioned baseline — after a change, rerun `tools/nobake_scoreboard.py` and `git diff` this file to see **regressions (baked ↑)** or **progress (baked ↓)**. Rows sorted by category name (stable) so a count change touches only its own row.

- **Source**: runtime `[COVERAGE]` log (ERR profile), build 2026-06-26 11:54:37.712
- **`live-cls`** = category resolved via the live `classify_item_live` fallback (item the baked table didn't know).
- `disk`/`live` counts are **per-placement** (collectibles emit one marker per world node) → `total` is not directly comparable to deduped baked counts. For the migration what matters is **does a category still have baked>0**.
- **`drawn`** = real markers the renderer draws (= total). **`census`** = the ImGui badge denominator (completable spots) — distinct collect flags for flag-based categories, row count for geom/SFX pieces, 0 for graces; it EXCLUDES respawnable flag-less gather, so `census < drawn` wherever markers share a flag or respawn.
- **`flag`** = collect-flag coverage `flagged/drawn`: markers carrying a collect/cleared flag (can be collect-tracked / gray out) vs not. The flag-less split into **`respawn`** (lot-backed respawnable gather, no permanent done-state) and **`nonloot`** (TYPES with no collect flag — NPC/stake/spring/region). A big `nonloot` flags a feature whose objects can never complete on the map.
- **`icon`** = how the category's marker is drawn: **`symbol`** = native GPU map symbol (crisp); **`atlas N%`** = baked atlas CPU cell, N% non-transparent (a trailing ⚠ = faint/small <25%, renders but easy to miss — the Stakes class of "looks broken but isn't"; bump `overlay_icon_scale`); **`circle`** = flat coloured disc; **`none`** = key set but absent from the atlas. Item categories (Equipment/Key/Loot/Magic) also draw the live GPU inventory icon, so their atlas % is only the fallback — low % matters most for atlas-only World features.
- Graces are `Live` (BonfireWarpParam) but tallied separately in GraceLayer — not in this table.

## ▶ Baked markers remaining

# **50**  ← drive this to **0**

| | baked | disk | live | live-cls | total |
|---|--:|--:|--:|--:|--:|
| **all categories** | **50** | 7909 | 469 | 188 | 8428 |

🔴 baked-only: **0**  ·  🟡 partial: **14**  ·  🟢 off-bake: **47**  (of 61 active categories)

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
| Equipment - Armaments | 0 | 289 | 0 | 0 | 289 | atlas 69% | 🟢 off-bake |
| Equipment - Armour | 1 | 243 | 0 | 0 | 244 | atlas 69% | 🟡 partial |
| Equipment - Ashes of War | 2 | 77 | 0 | 0 | 79 | atlas 69% | 🟡 partial |
| Equipment - Spirits | 2 | 71 | 0 | 0 | 73 | atlas 69% | 🟡 partial |
| Equipment - Talismans | 2 | 136 | 0 | 0 | 138 | circle | 🟡 partial |
| Key - Celestial Dew | 0 | 9 | 0 | 0 | 9 | atlas 69% | 🟢 off-bake |
| Key - Cookbooks | 0 | 87 | 0 | 0 | 87 | atlas 69% | 🟢 off-bake |
| Key - Crystal Tears | 0 | 38 | 0 | 0 | 38 | atlas 69% | 🟢 off-bake |
| Key - Great Runes | 0 | 0 | 6 | 0 | 6 | atlas 69% | 🟢 off-bake |
| Key - Imbued Sword Keys | 0 | 4 | 0 | 0 | 4 | atlas 69% | 🟢 off-bake |
| Key - Larval Tears | 4 | 16 | 0 | 0 | 20 | atlas 69% | 🟡 partial |
| Key - Lost Ashes | 0 | 81 | 0 | 0 | 81 | atlas 69% | 🟢 off-bake |
| Key - Pots n Perfumes | 0 | 40 | 0 | 0 | 40 | atlas 69% | 🟢 off-bake |
| Key - Scadutree Fragments | 0 | 45 | 0 | 0 | 45 | atlas 30% | 🟢 off-bake |
| Key - Seeds Tears Ashes | 1 | 74 | 0 | 0 | 75 | atlas 69% | 🟡 partial |
| Key - Whetblades | 0 | 5 | 0 | 0 | 5 | circle | 🟢 off-bake |
| Loot - Ammo | 0 | 81 | 0 | 81 | 81 | atlas 69% | 🟢 off-bake |
| Loot - Bell-Bearings | 0 | 50 | 0 | 0 | 50 | atlas 69% | 🟢 off-bake |
| Loot - Consumables | 0 | 186 | 0 | 6 | 186 | atlas 69% | 🟢 off-bake |
| Loot - Crafting Materials | 0 | 1697 | 0 | 99 | 1697 | atlas 69% | 🟢 off-bake |
| Loot - Dragon Hearts | 0 | 20 | 0 | 0 | 20 | atlas 69% | 🟢 off-bake |
| Loot - Gestures | 0 | 7 | 0 | 0 | 7 | atlas 69% | 🟢 off-bake |
| Loot - Gloveworts | 0 | 271 | 0 | 0 | 271 | atlas 69% | 🟢 off-bake |
| Loot - Golden Runes | 9 | 217 | 0 | 0 | 226 | atlas 69% | 🟡 partial |
| Loot - Golden Runes (Low) | 14 | 430 | 0 | 0 | 444 | atlas 69% | 🟡 partial |
| Loot - Greases | 1 | 138 | 0 | 0 | 139 | atlas 69% | 🟡 partial |
| Loot - Great Gloveworts | 0 | 23 | 0 | 0 | 23 | atlas 69% | 🟢 off-bake |
| Loot - MP-Fingers | 0 | 9 | 0 | 0 | 9 | atlas 69% | 🟢 off-bake |
| Loot - Prattling Pates | 0 | 9 | 0 | 0 | 9 | atlas 69% | 🟢 off-bake |
| Loot - Rada Fruit | 0 | 14 | 0 | 0 | 14 | atlas 69% | 🟢 off-bake |
| Loot - Reusables | 1 | 10 | 0 | 0 | 11 | atlas 69% | 🟡 partial |
| Loot - Rune Arcs | 0 | 75 | 0 | 0 | 75 | atlas 69% | 🟢 off-bake |
| Loot - Smithing Stones | 1 | 322 | 0 | 2 | 323 | atlas 69% | 🟡 partial |
| Loot - Smithing Stones (Low) | 0 | 402 | 0 | 0 | 402 | atlas 69% | 🟢 off-bake |
| Loot - Smithing Stones (Rare) | 0 | 11 | 0 | 0 | 11 | atlas 69% | 🟢 off-bake |
| Loot - Stat Boosts | 0 | 82 | 0 | 0 | 82 | atlas 69% | 🟢 off-bake |
| Loot - Stonesword Keys | 0 | 48 | 0 | 0 | 48 | atlas 69% | 🟢 off-bake |
| Loot - Throwables | 0 | 123 | 0 | 0 | 123 | circle | 🟢 off-bake |
| Loot - Utilities | 0 | 46 | 0 | 0 | 46 | circle | 🟢 off-bake |
| Magic - Incantations | 0 | 70 | 0 | 0 | 70 | atlas 69% | 🟢 off-bake |
| Magic - Memory Stones | 0 | 6 | 0 | 0 | 6 | atlas 69% | 🟢 off-bake |
| Magic - Prayerbooks | 0 | 17 | 0 | 0 | 17 | atlas 69% | 🟢 off-bake |
| Magic - Sorceries | 0 | 71 | 0 | 0 | 71 | atlas 69% | 🟢 off-bake |
| Quest - Deathroot | 0 | 9 | 0 | 0 | 9 | atlas 69% | 🟢 off-bake |
| Quest - Progression | 0 | 44 | 0 | 0 | 44 | atlas 69% | 🟢 off-bake |
| Quest - Seedbed Curses | 0 | 6 | 0 | 0 | 6 | atlas 69% | 🟢 off-bake |
| Reforged - Ember Pieces | 0 | 302 | 0 | 0 | 302 | atlas 30% | 🟢 off-bake |
| Reforged - Fortunes | 0 | 58 | 0 | 0 | 58 | atlas 69% | 🟢 off-bake |
| Reforged - Items | 0 | 74 | 0 | 0 | 74 | atlas 69% | 🟢 off-bake |
| Reforged - Rune Pieces | 9 | 1235 | 0 | 0 | 1244 | atlas 30% | 🟡 partial |
| World - Bosses | 0 | 0 | 217 | 0 | 217 | symbol | 🟢 off-bake |
| World - Hostile NPC | 0 | 50 | 0 | 0 | 50 | atlas 83% | 🟢 off-bake |
| World - Imp Statues | 0 | 37 | 0 | 0 | 37 | atlas 30% | 🟢 off-bake |
| World - Interactables | 2 | 100 | 0 | 0 | 102 | atlas 74% | 🟡 partial |
| World - Kindling Spirits | 0 | 5 | 0 | 0 | 5 | atlas 69% | 🟢 off-bake |
| World - Maps | 1 | 23 | 0 | 0 | 24 | circle | 🟡 partial |
| World - Paintings | 0 | 11 | 0 | 0 | 11 | atlas 65% | 🟢 off-bake |
| World - Spirit Springs | 0 | 72 | 0 | 0 | 72 | atlas 96% | 🟢 off-bake |
| World - Spiritspring Hawks | 0 | 14 | 0 | 0 | 14 | atlas 69% | 🟢 off-bake |
| World - Stakes of Marika | 0 | 219 | 0 | 0 | 219 | atlas 19% ⚠ | 🟢 off-bake |
| World - Summoning Pools | 0 | 0 | 246 | 0 | 246 | atlas 69% | 🟢 off-bake |

## Baked residual by provenance — why each baked marker survives

Every surviving baked **loot** row (not replaced by any disk pass), tallied by its bake `loot_source`. This is the **recovery lever per category** — and the result of the 2026-06-25 deep-dive into the residual (see `memory/msbe-enemy-loot-offsets.md`):

- **`treasure`** — an MSB Treasure the disk pass didn't reproduce: the **corpse debake-gap**, items whose ItemLotParam chain is absent from the mod's loot linkage. Accepted residual (~0.4% of the treasure slice).
- **`enemy`** — ✅ **INVESTIGATED & CLOSED: a bake MIS-LABEL, not recoverable here.** These lots are referenced by **NO `NpcParam.itemLotId`** — proven irrefutably against every parsed enemy, the FULL NpcParam table, the paramdef-authoritative offline scan, AND the **vanilla** regulation (all **0** matches). mapgenie shows them on *corpses/bodies* → they are corpse/EMEVD-scripted loot the bake wrongly tagged `Enemy`. The NpcParam enemy pass is COMPLETE; the items appear elsewhere via the treasure/emevd passes (or are phantom dupes). A bake regen would re-tag them.
- **`emevd`** — an EMEVD award the disk EMEVD pass didn't reproduce: a **still-open, genuinely recoverable** lever (extend the EMEVD template coverage).
- **`unknown`** — pre-provenance bake rows (the `loot_source` field predates the tagging and wasn't regenerated); could be any source. A regen reclassifies them.

Residual loot total **38** = unknown 22 · treasure 1 (accepted) · enemy 15 (bake mis-label) · emevd 0 (recoverable).

| category | unknown | treasure (accepted) | enemy (mis-label) | emevd (recoverable) |
|---|--:|--:|--:|--:|
| Equipment - Armour | 0 | 0 | 1 | 0 |
| Equipment - Ashes of War | 1 | 0 | 1 | 0 |
| Equipment - Spirits | 2 | 0 | 0 | 0 |
| Equipment - Talismans | 2 | 0 | 0 | 0 |
| Key - Larval Tears | 2 | 0 | 2 | 0 |
| Key - Seeds Tears Ashes | 0 | 0 | 1 | 0 |
| Loot - Golden Runes | 3 | 0 | 6 | 0 |
| Loot - Golden Runes (Low) | 11 | 0 | 3 | 0 |
| Loot - Greases | 0 | 1 | 0 | 0 |
| Loot - Reusables | 0 | 0 | 1 | 0 |
| Loot - Smithing Stones | 1 | 0 | 0 | 0 |

## Census (badge vs drawn) + collect-flag coverage

`drawn` = markers drawn · `census` = completable-spots badge · `flag` = flagged/drawn · `respawn`/`nonloot` = the flag-less split. A large `nonloot` marks a feature whose objects carry no collect flag (can't gray/complete). Categories missing here weren't in the census log.

| category | drawn | census | flag (have/drawn) | respawn | nonloot |
|---|--:|--:|--:|--:|--:|
| Equipment - Armaments | 289 | 264 | 289/289 | 0 | 0 |
| Equipment - Armour | 244 | 117 | 244/244 | 0 | 0 |
| Equipment - Ashes of War | 79 | 79 | 79/79 | 0 | 0 |
| Equipment - Spirits | 73 | 73 | 73/73 | 0 | 0 |
| Equipment - Talismans | 138 | 135 | 138/138 | 0 | 0 |
| Key - Celestial Dew | 9 | 8 | 9/9 | 0 | 0 |
| Key - Cookbooks | 87 | 85 | 87/87 | 0 | 0 |
| Key - Crystal Tears | 38 | 38 | 38/38 | 0 | 0 |
| Key - Great Runes | 6 | 6 | 6/6 | 0 | 0 |
| Key - Imbued Sword Keys | 4 | 4 | 4/4 | 0 | 0 |
| Key - Larval Tears | 20 | 20 | 20/20 | 0 | 0 |
| Key - Lost Ashes | 81 | 81 | 81/81 | 0 | 0 |
| Key - Pots n Perfumes | 40 | 40 | 40/40 | 0 | 0 |
| Key - Scadutree Fragments | 45 | 45 | 45/45 | 0 | 0 |
| Key - Seeds Tears Ashes | 75 | 74 | 75/75 | 0 | 0 |
| Key - Whetblades | 5 | 5 | 5/5 | 0 | 0 |
| Loot - Ammo | 81 | 81 | 81/81 | 0 | 0 |
| Loot - Bell-Bearings | 50 | 49 | 50/50 | 0 | 0 |
| Loot - Consumables | 186 | 182 | 186/186 | 0 | 0 |
| Loot - Crafting Materials | 1697 | 592 | 594/1697 | 1103 | 0 |
| Loot - Dragon Hearts | 20 | 20 | 20/20 | 0 | 0 |
| Loot - Gestures | 7 | 6 | 7/7 | 0 | 0 |
| Loot - Gloveworts | 271 | 39 | 39/271 | 232 | 0 |
| Loot - Golden Runes | 226 | 222 | 226/226 | 0 | 0 |
| Loot - Golden Runes (Low) | 444 | 441 | 444/444 | 0 | 0 |
| Loot - Greases | 139 | 135 | 139/139 | 0 | 0 |
| Loot - Great Gloveworts | 23 | 20 | 20/23 | 3 | 0 |
| Loot - MP-Fingers | 9 | 9 | 9/9 | 0 | 0 |
| Loot - Prattling Pates | 9 | 9 | 9/9 | 0 | 0 |
| Loot - Rada Fruit | 14 | 14 | 14/14 | 0 | 0 |
| Loot - Reusables | 11 | 11 | 11/11 | 0 | 0 |
| Loot - Rune Arcs | 75 | 73 | 75/75 | 0 | 0 |
| Loot - Smithing Stones | 323 | 242 | 248/323 | 75 | 0 |
| Loot - Smithing Stones (Low) | 402 | 346 | 354/402 | 48 | 0 |
| Loot - Smithing Stones (Rare) | 11 | 11 | 11/11 | 0 | 0 |
| Loot - Stat Boosts | 82 | 82 | 82/82 | 0 | 0 |
| Loot - Stonesword Keys | 48 | 48 | 48/48 | 0 | 0 |
| Loot - Throwables | 123 | 121 | 123/123 | 0 | 0 |
| Loot - Utilities | 46 | 46 | 46/46 | 0 | 0 |
| Magic - Incantations | 70 | 68 | 70/70 | 0 | 0 |
| Magic - Memory Stones | 6 | 6 | 6/6 | 0 | 0 |
| Magic - Prayerbooks | 17 | 14 | 17/17 | 0 | 0 |
| Magic - Sorceries | 71 | 68 | 71/71 | 0 | 0 |
| Quest - Deathroot | 9 | 9 | 9/9 | 0 | 0 |
| Quest - Progression | 44 | 33 | 44/44 | 0 | 0 |
| Quest - Seedbed Curses | 6 | 5 | 6/6 | 0 | 0 |
| Reforged - Ember Pieces | 302 | 302 | 23/302 | 0 | 279 |
| Reforged - Fortunes | 58 | 8 | 58/58 | 0 | 0 |
| Reforged - Items | 74 | 58 | 74/74 | 0 | 0 |
| Reforged - Rune Pieces | 1244 | 1244 | 126/1244 | 0 | 1118 |
| World - Bosses | 217 | 215 | 217/217 | 0 | 0 |
| World - Hostile NPC | 50 | 26 | 27/50 | 0 | 23 |
| World - Imp Statues | 37 | 37 | 37/37 | 0 | 0 |
| World - Interactables | 102 | 102 | 102/102 | 0 | 0 |
| World - Kindling Spirits | 5 | 5 | 5/5 | 0 | 0 |
| World - Maps | 24 | 24 | 24/24 | 0 | 0 |
| World - Paintings | 11 | 11 | 11/11 | 0 | 0 |
| World - Spirit Springs | 72 | 0 | 0/72 | 0 | 72 |
| World - Spiritspring Hawks | 14 | 14 | 14/14 | 0 | 0 |
| World - Stakes of Marika | 219 | 0 | 0/219 | 0 | 219 |
| World - Summoning Pools | 246 | 246 | 246/246 | 0 | 0 |

