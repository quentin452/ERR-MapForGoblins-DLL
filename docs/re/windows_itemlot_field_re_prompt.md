# Windows-Ghidra RE prompt — ItemLotParam field AOBs (upgrade offset-only → resolved)

## Why
Gap C ⋈ map markers: to put a CUSTOM item on the map you change an ItemLotParam lot's slot-1 item
(`lotItemId01`) to the custom goods id, and the loot marker then resolves to it (PROVEN live 2026-07-03:
`param_setf ItemLotParam_map 997230 lotItemId01 8000000` → `loot_at` = `name='Goblin Test Item'`).

The three slot-1 fields are now exposed in the param-edit registry (`goblin_param_edit.cpp` `kFields`)
as **offset-only** entries (`aob == nullptr`, `disp_pos/size` unused) — the offsets are core-stable and
already read raw all over `goblin_loot_resolve.cpp`:

| field                | offset | type |
|----------------------|--------|------|
| `lotItemId01`        | `0x00` | s32  |
| `lotItemCategory01`  | `0x20` | s32  |
| `lotItemBasePoint01` | `0x40` | u16  |

(registered for both `ItemLotParam_map` and `ItemLotParam_enemy`.) They WORK today via the fallback
offset. This sweep is to author real AOBs so the offsets resolve live from the exe (version-proof, same
as `goodsType`/`sortGroupId`/`pickUpItemLotParamId`) and the `nullptr` AOB can be replaced.

## Task (Ghidra, `D:\ghidra_proj2\ER`, imagebase 0x140000000)
Find a game code site that READS each field off an ItemLotParam row and author an AOB whose displacement
IS the field offset, in the `resolve_field_offset` form used by `re_signatures.hpp` (an access
instruction like `mov reg,[rbase+disp]`; give `disp_pos` = byte index of the displacement in the match
and `disp_size`). The lot-processing / reward-grant path (`ItemLotParam` lookup when the game rolls a
drop) is the natural read site; `lotItemBasePoint01`@0x40 is read in the weighted-roll selection, and
`lotItemId01`@0x00 / `lotItemCategory01`@0x20 right after to grant the chosen slot.

Deliverable `docs/re/windows_itemlot_field_re_findings.md` + the AOBs added to `re_signatures.hpp`
(e.g. `ITEMLOT_LOTITEMID01_ACCESS`, `…_CATEGORY01_ACCESS`, `…_BASEPOINT01_ACCESS`); then flip the three
`kFields` rows from `nullptr` to the sig with the right `disp_pos/disp_size`. Cross-check the resolved
offset == the fallback (0x00 / 0x20 / 0x40).

## Notes
- Offset-only already works; this is hardening, not a blocker. Do it opportunistically alongside other
  ItemLotParam RE.
- Slots 02..08 follow the same stride (id `+0x00 + i*4`, category `+0x20 + i*4`, basePoint `+0x40 + i*2`,
  num `+0x8A + i`), so one AOB per field-family covers all 8 by adding the stride.
- Related open item: a NEW cloned lot isn't visible to `resolve_loot_item_textid` (the `LotReader` lot
  INDEX is snapshotted at init) — only existing lots resolve. That + the drawn-marker rebuild are the
  `refresh_markers` follow-up (see HANDOFF), not this sweep.
