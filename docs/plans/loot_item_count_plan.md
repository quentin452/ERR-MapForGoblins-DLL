# Plan: loot item count (undercount fix + ×N stacking)

Status: DONE 2026-06-30 on branch `feat/loot-item-count` (builds + deploys clean, runtime-confirmed).
`goblin::lot_item_count()` sums slots 01–08 (`lotItemNum01 @ +0x8A` — +0x89 was lotItem_Rarity, an
off-by-one that read 0xFF=255), `Marker.count` threads it through push_marker, and the hover tooltip
shows the item name + ` xN` quantity suffix (no separate on-map badge — UX call this iteration).
Acceptance met in-game: "Below The Well" → Sliver of Meat x3.

Step 5 (cluster pile = sum of member counts) intentionally NOT done: the pile glyph stays a MARKER
count (one per lot); per-lot quantity lives in each marker's tooltip. Avoids overloading the pile
number with two meanings.

Note (not a bug): Formic Rock node `AEG099_852` → lot `998520` has slots (20852 num1)+(20852 num2)
= 3, so it correctly reads x3. Vanilla/Mapgenie's "4" is a vanilla-tuned value; ERR retuned the lot.
The live read is right by design (mod-agnostic).

## The two bugs (same root → one feature)

- **Undercount.** "Below The Well" Site of Grace shows **1** Sliver of Meat; Mapgenie lists **3×**.
- **No stacking.** Several adjacent identical items (e.g. "Formic Rock") draw as separate icons instead
  of one icon with a count — wanted even with clustering off.

Both come from the loot reader collapsing a multi-item lot to a single slot-1 marker.

## Root cause (confirmed)

`ItemLotParam` rows have **8 item slots** + per-slot quantities:
- `lotItemId01..08`, `lotItemCategory01..08`, `lotItemNum01..08` (u8, 0–99) —
  `tools/paramdefs/ItemLotParam.xml` (ids @9–72, nums @325–372).

But the readers fetch **slot 01 only** and **ignore every quantity**:
- `src/goblin_inject.cpp:4772-4777` — `resolve_loot_item_textid()` reads `lotItemId01` (+0x00) / `lotItemCategory01` (+0x20) only.
- `src/goblin_inject.cpp:4809-4815` — `lot_row_in_table()` same, slot 01 only.
- `src/worldmap/map_entry_layer.cpp:241-284` — `emit_lot_siblings()` walks contiguous sibling ROWS
  (base+1, base+2…) and emits one marker each, but each still reads only its own slot 01.

`Marker` (`src/worldmap/marker_layer.hpp:26-108`) has **no count/quantity field** at all; no same-item
dedup anywhere (collectibles "EMIT PER PLACEMENT", map_entry_layer.cpp:419-424).

So: 1 lot row → 1 marker → slot-01 item, quantity 1. A lot carrying 3 items (slots 01–03, or qty 3)
shows as 1.

## Implementation (≈5–6 edits)

1. **Loot reader → per-lot item count.** Extend `resolve_loot_item_textid` / `lot_row_in_table` (or add a
   sibling `lot_item_count(lotId, lotType)`) to iterate slots 01–08: count non-zero `lotItemId0N`,
   summing `lotItemNum0N` (treat 0 as 1). Decide the count semantics:
   - simplest useful: **total item count at this lot** (Σ over slots of max(num,1) for non-empty slots).
   - the marker's displayed icon/name stays slot-01 (the primary), count is the badge number.
2. **`Marker.count`** — add `int count = 1;` at the END of the struct (positional aggregate init —
   set by assignment in `push_marker`, like `item_icon_id`/`worldY`).
3. **Thread it** — in `push_marker` (map_entry_layer.cpp), set `m.count = lot_item_count(...)` for
   lot-backed markers (mirror the per-item icon resolution already there).
4. **Render badge** — in `draw_marker` (map_renderer.cpp), when `m.count > 1` draw a small "×N" badge
   (bottom-right corner; primitives + the numeric font glyphs load fine, no tofu). Reuse the corner-badge
   idiom from the altitude cue. Config-gate? probably always-on (it's information, not clutter).
5. **Clustering interaction** — piles already show a member count; a single marker with `count>1` is the
   "stack" case. Decide if a pile's number should sum member `count`s (likely yes) — check `draw_clusters`
   pile count (currently `idxs.size()`), maybe sum `items[i].m->count`.

## Verify first (cheap)

Inspect the actual "Below The Well" `ItemLotParam_map` row(s) to confirm the shape — 3 items in slots
01–03 of ONE row vs 3 sibling rows vs qty=3. (If it's sibling rows, `emit_lot_siblings` should already
make 3 markers — so it's almost certainly slots/qty within one row.) That tells you whether step 1 needs
slot iteration, quantity, or both. Likely **both**.

## Acceptance

- "Below The Well" shows the Sliver of Meat icon with **×3** (or 3 stacked correctly).
- A lot with multiple distinct items at one node shows its real count.
- Mod-agnostic: reads the active install's live `ItemLotParam` (no bake).
