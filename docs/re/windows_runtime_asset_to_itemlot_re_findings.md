# Findings — runtime LOADED-asset → ItemLotID link

Companion to `windows_runtime_asset_to_itemlot_re_prompt.md`. **§3 verdict: the link EXISTS, resident.**

---

## §3 — the loaded treasure's itemLotId IS resident (not open-time-only)

**One-line:** A loaded chest's `itemLotId` is held on a runtime **`FieldIns`** (field-gimmick) instance,
pooled in `CS::CSGrowableNodePool<FieldInsBase*>`, resident the whole time the tile is loaded — so a
runtime asset→lot identity is possible for loaded tiles (powers the explore-cache supplement).

**Method (external RPM, `D:\ghidra_scripts\`):**
- `asset_lot_probe.py`: walked `CSWorldGeomMan`, picked a loaded chest **`AEG099_090_9000`** in
  `m60_37_50_00` whose baked `itemLotId = 1037500100 (0x3dd6fec4)`. The lotId was **NOT** on the
  `CSWorldGeomStaticIns` instance, its `MsbPart`, nor one level of its pointers.
- `lot_resident_search.py`: full MEM_PRIVATE search for `0x3dd6fec4` → **4 resident matches**, two
  distinct families:
  - region `0x22e4…` — a sorted `{lotId, index}` array (neighbours `…fe61/…fe62/…fec4/…2570`, indices
    `0x1a2c,1a2d,1a2e…`) = **the `ItemLotParam` table itself** (expected; what `resolve_loot_item_textid`
    reads). NOT the link.
  - region `0x22d3c6e…` (the loaded-object heap) — a structured record. **This is the link.**
- `treasure_record_probe.py`: characterized it:
  - the record `{ lotId(u32)+flag, FieldIns* }` lives in an object whose RTTI (scan-back 0x220) is
    **`CS::CSGrowableNodePool<FieldInsBase*>`**.
  - its pointer → a **`FieldIns`** object (inline wide-char name `"アイテム…"` = "item"), which holds
    the **lotId again at `FieldIns+0x50`**.

**Conclusion:** ER parses MSB `Events.Treasure` at tile-load into a resident **field-gimmick
instance (`FieldIns`)** that carries the `itemLotId`. So: `geom asset (have pos+name) → its FieldIns
→ itemLotId → resolve_loot_item_textid (have) → full item identity`, all live, **for loaded tiles**.

## Caveat (keeps the supplement scope honest)
`FieldIns` are **loaded instances** — same loaded-only physics as `CSWorldGeomStaticIns`
(`windows_global_item_position_structure_re_findings.md`). This does **not** enable removing the bake;
it makes the explore-cache / added-loot-coverage layer high-quality on loaded tiles. As scoped in the
prompt.

## Remaining to wire it (next steps — not yet done)
1. **Static base** to the FieldIns owner: pointer-scan the `CSGrowableNodePool<FieldInsBase*>` (or find
   the `CSFieldInsMan`/gimmick singleton via RTTI) → `er_base+RVA`, add to `src/re_signatures.hpp`.
2. **The join key** geom-asset → FieldIns: confirm both carry the same **EntityID** (MSB
   `Parts.Asset` EntityID ↔ the FieldIns'), so a walked chest can find its FieldIns→lot. (The FieldIns
   layout: name inline @+0x00, lotId @+0x50; find its EntityID field.)
3. `lotType 2` enemy drops: that lot is on the enemy ChrIns (WorldChrMan) — secondary, not blocked on.

Tools: `D:\ghidra_scripts\asset_lot_probe.py`, `lot_resident_search.py`, `treasure_record_probe.py`.
