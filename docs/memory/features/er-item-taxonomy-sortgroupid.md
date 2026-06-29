---
name: er-item-taxonomy-sortgroupid
description: ER's OWN item classification = EquipParamGoods.goodsType(@+0x3e) + sortGroupId(@+0x72); the drift-free, mod/DLC-agnostic source for marker categories (vs the per-item ITEM_ICONS table).
metadata:
  node_type: memory
  type: reference
---

**Elden Ring classifies items via `EquipParamGoods` fields — use THEM, not a hand-maintained
per-item table.** This is the no-bake/no-drift source for marker categorization (<user>'s idea
2026-06-26: "trouver où ER fait sa classification et l'utiliser"). Two live-readable byte fields:

- **`goodsType` (u8 @ +0x3e)** — broad type. Known values seen in ERR: 0=normal goods (Golden Runes,
  Flask of Crimson Tears), 1=key/important item (maps, Letters, Medallions, Needles, crafting tools),
  2=crafting material, 8=spirit ash, 9=physick tear (Wondrous), 12=message/note, 14=upgrade material
  (Smithing Stones/Gloveworts/Seeds/Scadu), 15=Great Rune, 16/18=incantations, 17=sorceries.
- **`sortGroupId` (u8 @ +0x72)** — ER's FINE inventory-sort group; the authoritative sub-category.
  Pinned values: **runes** 100 (base Golden/Numen's/Hero's/Lord's 2900-2919) · 101 (DLC Broken/Shadow
  Realm 2002951-59) · 102 (ERR high-NG variants Golden One's/Ancient's/… goods 82909-82919); **maps**
  190 base · 191 DLC; **key items** 40/50/53/54/57; **smithing** (gType14) sg20 stones · 30 somber ·
  40 grave-glovewort · 41 ghost-glovewort · 10/15/19 seeds/scadu; **Great Runes** (gType15) sg11.

**Live reader** (goblin_inject.cpp): `goods_type_live(gid)` reads @+0x3e, `goods_sort_group(gid)` reads
@+0x72 (both via `from::params::get_param<RawGoodsRow>(L"EquipParamGoods")`, cached once_flag; RawGoodsRow
= 176 bytes). `goods_is_map(gid)` = sortGroupId∈{190,191}, refactored onto goods_sort_group. **sortGroupId
is u8 — reading 2 bytes folds in the 0x73 bitfield.** Offsets pinned via the paramdef field-layout walk.

**Where used (shipped 2026-06-26, branch feat/rune-fix-and-classification-guard):** `classify_item_live`
(the live fallback when ITEM_ICONS misses) routes sortGroupId 100/101/102→LootGoldenRunes and goodsType-1
(non-map)→QuestProgression. ITEM_ICONS stays the PRIMARY classifier (it owns the curated Low/normal split
+ the specific Key* sub-buckets); sortGroupId/goodsType is the smart FALLBACK for the table-miss tail.

**★ PHASE-3 ENDGAME — DONE (2026-06-26, branch feat/phase3-taxonomy-classifier):** the (goodsType,
sortGroupId)→Category map IS the primary classifier now; ITEM_ICONS dropped to icon-only. The FINER
curated bits (Low/normal/Rare splits, the gType1-sg50 grab-bag, Quest/Prayerbook lists) live in a
small generated `goblin_category_exceptions` table (133 ids). Non-goods need NO sort fields (the mod
never sub-divides equipment). Proven 0-drift vs the old 1818-row bake. See
[[phase3-taxonomy-map-validated]], [[nobake-endgame-roadmap]].

**Caveat — the mod's categories ≠ ER's groups 1:1.** ER has homes ER doesn't: messages (gType12, ~8 lore
notes) have NO mod category → still land in the LootCraftingMaterials catch-all (accepted, low ROI). Adding
one needs the full category-addition surface (enum in src/generated/goblin_map_data.hpp [STATIC, no
generator] + category_meta.cpp CAT[] in-enum-order + static_assert + NUM_CATEGORIES refs in goblin_inject.cpp
& goblin_config_schema.cpp + category_name switch + a show_* toggle). QuestProgression already = "Key Items".
