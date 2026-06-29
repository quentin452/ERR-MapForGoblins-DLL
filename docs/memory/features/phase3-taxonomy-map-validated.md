---
name: phase3-taxonomy-map-validated
description: Phase-3 (gType,sg)->Category live-classifier mapping is PROVEN offline (zero drift vs the 1818-row ITEM_ICONS bake). Probe + equivalence oracle + the validated scheme to port to C++.
metadata:
  node_type: memory
  type: project
---

**Phase-3 = replace the per-item ITEM_ICONS *category* column with ER's own live taxonomy
`(goodsType@+0x3e, sortGroupId@+0x72)`. The mapping is DESIGNED + PROVEN OFFLINE (2026-06-26),
not yet ported to C++.** See [[er-item-taxonomy-sortgroupid]], [[nobake-endgame-roadmap]].

**Tooling (committed):**
- `tools/_probe_taxonomy_map.py` — reads ALL 7126 EquipParamGoods rows (SoulsFormats, via the
  pipeline env: `PYTHONPATH=mfg_aux MFG_PROFILE=err py -3.14`), replays the curated
  `LOOT_CATEGORIES` classifier per item, aggregates by `(gType,sg)` cell. Writes
  `data/goods_type_sortgroup.json` {id:[gType,sg]} + `docs/taxonomy_map_probe.md` (183 cells,
  14 conflicts).
- `tools/_validate_taxonomy_map.py` — pure-json equivalence ORACLE: computes the proposed
  category for every baked item and diffs vs `item_icon_table.json`. **Result: 0 mismatches /
  1818** (1685 map-handled, 133 curated-exception). This is the C++ port's regression guard.

**The validated 3-tier scheme (reproduces the bake byte-for-byte):**
1. **Non-goods** (encoded key <500M) → ItemLotParam category by key-range: gem(+400M)→AshesOfWar,
   accessory(+300M)→Talismans, protector(+200M)→Armour, weapon(+100M)→Armaments, with `id≥50M`
   (key≥150M, or legacy raw-50M in the old table) → Ammo. **No Weapon/Protector/Accessory/Gem sort
   field needed — the mod never sub-divides equipment** (blocker (b) is a non-issue).
2. **Curated exceptions** (133 ids, generated table id→Category) — the splits/grab-bags ER doesn't
   encode: Golden Runes (Low) 11, Smithing (Low) 12 + (Rare) 2, Great Gloveworts 2, Rune Arcs[150],
   Pates 9, MP-Fingers 9, Rada[2020001], the gType1-sg50 grab-bag (Celestial Dew/Imbued Sword/
   Stonesword/Deathroot/Seedbed) + Whetblades 5, gType14-sg3/15 (Larval/Lost Ashes/Dragon Heart/
   Scadu/Seeds), Memory Stones[10030], Reforged Items/Fortunes 12/Sealed Curios 9, **Quest-Progression
   29 + Prayerbooks 14** (these two stay full id-lists so cross-cell members — note 8714/8716,
   Secret Rite 2008014, Three Fingers 8867 — classify right; the map rows below are generic fallback).
3. **(gType,sg) live map** (the bulk): cells —
   `(0,20)/(0,61)`Consumables, `(0,50)`Throwables, `(0,70)`Greases, `(0,60)`Reusables,
   `(0,80)`Utilities, `(0,10)`StatBoosts, `(0,100)/(0,101)`GoldenRunes, `(0,15)`Spirits(ERR renumber),
   `(1,80)/(1,90)`BellBearings, `(1,200)/(1,205)`Cookbooks, `(1,100)`Prayerbooks, `(10,20)`CrystalTears,
   `(11,30)`PotsNPerfumes, `(14,40)/(14,41)`Gloveworts, `(14,19)/(14,20)/(14,30)`SmithingStones.
   goodsType fallback — 2→Crafting, 5/17→Sorceries, 16/18→Incantations, 7/8→Spirits, **gType1
   default→QuestProgression**. Else → catch-all LootCraftingMaterials.

**Design decisions (<user>, 2026-06-26):** (1) exceptions in a NEW generated `CATEGORY_EXCEPTIONS`
table (id→Category, profile-aware) built from the LOOT_CATEGORIES id-lists; ITEM_ICONS keeps ONLY the
icon column. (2) Low/normal/Rare splits = explicit id-lists (faithful), NOT id-thresholds.

**Blockers resolved:** (a) Low/normal NOT in sortGroupId (runes share sg100/101, smithing sg20/30) →
stays curated id-list, confirmed. (b) non-goods need NO new offsets. (c) gType12 messages → catch-all,
accepted. Win: **93% of 1818 per-item category rows collapse to ~30 rules**; all future mod/DLC items
classified live for free.

**C++ PORT SHIPPED (2026-06-26, branch feat/phase3-taxonomy-classifier, commit db63ed8, built
clang-cl + deployed; awaiting <user> runtime-test/merge):** `item_marker_category` IS the primary
classifier now (non-goods key-range → `lookup_category_exception` → `category_from_taxonomy`
(goodsType,sortGroupId) → -1 tail). New `goblin_category_exceptions.{hpp,cpp}` (133 ids, generated
by `generate_category_exceptions_cpp` from item_icon_table + CATEGORY_EXCEPTION_CATS). ITEM_ICONS
struct dropped to `{key, iconId}` (icon-only). `classify_item_live` simplified to the tail (gType1→
QuestProgression, else LootCraftingMaterials) and now flags ONLY that catch-all/key tail as
live_classified → [ITEMCLASS]/item_classification.md census stays small + focused. `category_from_taxonomy`
adds rune sg102 (ERR high-NG variants) beyond the oracle's 100/101. CMakeLists + 4-profile header-copy
updated. Commit 094db6f = the two probe scripts + docs/taxonomy_map_probe.md.

**Caught a PRE-EXISTING bug:** the committed bake was STALE vs the committed generators — ammo keyed
raw-50M in goblin_item_icons.cpp/item_icon_table.json while generate_loot_massedit had the +100M fix.
The regen corrects 37 ammo keys 50M→150M (ammo icons now resolve at runtime). Benign, bundled.

**RUNTIME-VALIDATED 2026-06-26 (build 14:31 log):** no crash; `[COVERAGE]` baked stayed **27**
(zero provenance regression); `[ITEMCLASS]` census shrank **75→34 items / 6→2 categories** (18 Crafting
Materials catch-all + 16 Quest-Progression key tail) — the bulk (ammo/armaments/sorceries/etc.) now
classified confidently, exactly as designed. `docs/item_classification.md` refreshed to this baseline.

**Log-recheck bug caught + fixed (commit 4a2e039):** the `(gType0, sg15)` cell I mapped to Spirits is
contaminated by 4 non-spirit goods (Codex of the All-Knowing, Spectral Steed Whistle ×2, Memory of
Grace) the oracle never tested (unbaked) — Codex showed under the Spirits toggle. Fix: classify ERR
spirits by the **id range 300000-399999** (what generate_loot_massedit uses), not the sg15 cell.
Oracle still 0/1818. **LESSON: the oracle only covers BAKED/placed items; cells with an unbaked
contaminant tail (sg15 spirits, sg200 MP-fingers) need the id-precise signal, not the raw cell.**

**Refactor (commit b09fdde):** the Python mirror of the DLL classifier lives in the shared
`tools/taxonomy_classifier.py` (used by both `_validate_taxonomy_map.py` and the new
`tools/unplaced_items.py`). **Keep it in lock-step with goblin_inject.cpp on any map change.**

**Item 1 — known-but-unplaced report (commit b09fdde):** `tools/unplaced_items.py` → `docs/unplaced_items.md`
= per F1 category, the named goods that classify into it but are never placed (universe = all
EquipParamGoods; placed = item_icon_table.json). Real gaps: 88 incantations / 67 sorceries / 31
cookbooks never world-placed in ERR. Caveat: Spirits / Stat-Boosts counts loose (upgrade +N variants).

See [[overlay-item-search-bar]] for the session's item-2 (F1 search bar). Item 3 (randomizer support)
deferred — but the live classifier + content-agnostic search are a foundation for it.
