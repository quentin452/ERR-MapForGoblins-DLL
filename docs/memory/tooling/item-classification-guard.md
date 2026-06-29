---
name: item-classification-guard
description: The [ITEMCLASS] runtime census + docs/item_classification.md = regression guard for item→category classification (the live-fallback surface). Complements the provenance scoreboard.
metadata:
  node_type: memory
  type: project
---

**The provenance scoreboard ([[nobake-coverage-scoreboard]]) tracks WHERE a marker comes from per
category, but NOT WHICH category each item lands in — that blind spot let DLC/ERR runes + region maps
silently fall into the wrong bucket.** Closed 2026-06-26 (branch feat/rune-fix-and-classification-guard).

**Mechanism:** `build_buckets_impl` emits **`[ITEMCLASS]`** once per build (next to `[COVERAGE]`):
every DISTINCT item that reached its category via the **live fallback** (`classify_item_live` — i.e.
absent from the static ITEM_ICONS table = the drift-prone surface), with the category it landed in.
Built straight from `g_buckets` (`Marker.name_id` + `live_classified`), no push_marker change, sorted
by key (stable diff). `tools/item_classification.py` parses the latest block → **`docs/item_classification.md`**
grouped by category, names via FMG. **Regression guard: regenerate after a classify change + `git diff`
the doc — a row moves only when an item's category changes.** Table-backed items aren't here (pinned in
goblin_item_icons.cpp, diff that). Run order: game + open map → `py tools/item_classification.py`.

**What it found on its first run (all fixed, runtime-validated):**
- **24 region Maps double-bucketed** — the general loot pass (build_disk_loot_markers) classified
  disk-placed map fragments into LootCraftingMaterials via the live fallback while the dedicated
  build_disk_maps_markers pass ALSO emitted them to WorldMaps → each map drawn twice. Fix: skip
  `goods_is_map` goods in build_disk_loot_markers (the dedicated pass owns them).
- **16 key/quest items in CraftingMaterials** — Haligtree Medallion / Unalloyed Gold Needle / Irina's
  Letter / Black Knifeprint… = ER goodsType-1 items the curated ITEM_ICONS table MISSES (gaps: 8175
  absent while 8174/8176 mapped). Fix: classify_item_live routes the goodsType-1 (non-map) tail →
  the EXISTING QuestProgression category (its 'medallions/keys/Needles' intent), NOT a new category.
- **ERR rune variants (goods 82909-82919)** classified by ER's sortGroupId (102) → GoldenRunes (latent:
  not placed by any pass, so 0 visible, but correct + generic). See [[er-item-taxonomy-sortgroupid]].

**RESULT: Crafting Materials live-classified items 56→17** (the 17 = 3 real DLC mats + 8 messages
[gType12, no mod category, accepted] + flask + 1 EMEVD-granted Altus map leak + 5 unnamed). Zero baked
regression (TOTAL baked stayed 27; only live/disk markers re-bucketed). MERGED to master 2026-06-26.

**LESSON: a category's COUNT looking right (provenance scoreboard) doesn't mean its CONTENTS are right.
Per-item classification needs its own versioned artifact — the catch-all (CraftingMaterials) silently
absorbs anything the table misses.** [[er-item-taxonomy-sortgroupid]] is the drift-free fix direction.
