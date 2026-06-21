# RE prompt — Correct per-item "obtained" flag for loot markers (collected-graying / census)

**Status: WAITING on RE.** The overlay's collected-graying (dim looted items) and the F1
census badges (`<remaining>/<total>` per category) both rely on detecting whether a loot
marker's item is OBTAINED. On a **100% save** they MASSIVELY over-report "not collected"
— items that are clearly obtained read as still-existing (not grayed, counted remaining).
So the flag we resolve/check is the WRONG flag for many items.

## How it works now

- Each loot marker is lot-backed (`MapEntry.lotId`, `lotType`: 1=ItemLotParam_map, 2=_enemy).
- `goblin::resolve_loot_flag(lotId, lotType, baked)` (src/goblin_inject.cpp) reads the live
  ItemLotParam row and returns the pickup flag:
  - `@0x80` = lot-wide `getItemFlagId` (used first),
  - fallback `@0x60` = `getItemFlagId01` ONLY if single-item (`@0x04 lotItemId02 == 0`),
  - else the baked `textDisableFlagId1`.
- The overlay then checks `read_event_flag(flag)` (game's `IS_EVENT_FLAG`, AOB-resolved,
  works for any id — a known-good golden-rune flag 10007760 reads correctly).
- Collected-graying (map_renderer `marker_done`) + census (`refresh_overlay_census` in
  worldmap/map_entry_layer.cpp) share this predicate.

## The evidence ([CENSUS-UNSET] one-shot log, 100% save)

Per-category, count of collectible flags still UNSET at 100% (should be ~0):

- BROKEN (massively unset): Armaments 247/276, Talismans 111/139, Crafting 538/542,
  Golden Runes 219/223, Smithing-Low 307/350, Consumables 172/178, …
- WORKING (mostly set): Crystal Tears 35/38 set, Bosses 166/216 set, Imp Statues, etc.

Sample unset flags (the wrong ones we check):
- 8-digit `getItemFlagId`-style: `10007320 11107010 20007300 15001560 540170 …`
- **10-digit > 2³⁰ (1073741824)**: `2048447800 2050457730 1052557020 1054557010 …` →
  these are in ER's **TEMPORARY / event-local flag range** (non-persistent) →
  `read_event_flag` returns false outside that event → ALWAYS reads "not collected".

So `resolve_loot_flag` is returning, for many lots, either the lot-wide `@0x80` that
isn't the persistent obtained flag, or a TEMP flag (≥2³⁰), instead of the real per-item
persistent "you have picked this up" flag.

## The ask

Find the CORRECT persistent per-item obtained flag for a lot-backed loot marker, so a
100%-save reads ~all collected. Likely directions:
1. **Per-slot vs lot-wide:** is the real pickup flag the per-slot `getItemFlagId01..08`
   (ItemLotParam offsets — confirm the slot stride / offsets) rather than the lot-wide
   `@0x80`? For the categories that WORK (Crystal Tears, Bosses), what flag do they use vs
   the BROKEN ones (Armaments)? (Bosses use `clearedEventFlagId`; loot uses textDisable /
   lot flag.)
2. **Temp-flag exclusion:** flags ≥ 2³⁰ are temp/event-local — should those rows fall back
   to a different field (the param `textDisableFlagId1`, or the item's own acquisition
   flag) instead?
3. **The map-icon disable vs item-obtained distinction:** ER's WorldMapPointParam
   `textDisableFlagId1` (what we bake) may be a MAP-ICON gate, not an item-obtained flag —
   confirm whether the icon-hide flag and the item-acquired flag are the same per row, and
   if not, where the item-acquired flag lives (ItemLotParam? a fixed per-item flag table?).

## Deliverable

The rule/offset to compute the correct persistent obtained flag per loot marker (with the
ItemLotParam offsets confirmed, AOB-anchored), so `resolve_loot_flag` can be fixed and the
100%-save census/graying read correctly. This is the core blocker for collected/census
correctness on the overlay map.

Reference: `src/goblin_inject.cpp` (`resolve_loot_flag`, `refresh_loot_from_itemlot`,
`LotReader`, `orp_flag_set`), `src/worldmap/map_entry_layer.cpp` (`refresh_overlay_census`,
`[CENSUS-UNSET]` log), `src/worldmap/map_renderer.cpp` (`marker_done`).
