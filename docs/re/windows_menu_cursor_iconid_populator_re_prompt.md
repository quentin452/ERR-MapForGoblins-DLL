# RE brief — menu hovered-item ("real cursor") + the item-icon populator (authoritative iconId offset)

Goal: get the AUTHORITATIVE `EquipParam*.iconId` byte-offset per param, from the actual instruction
ER uses — not a static-struct guess (those were wrong, see below). Entry point = the inventory/menu
**hovered-item** structure (the "real cursor"), which the user proposed. App 2.6.2.0 / ERR 2.2.9.6,
imagebase `0x140000000`. Read-only RE (Ghidra static + light CT confirm).

## Why the prior offset is WRONG (don't trust static struct analysis)
- `windows_menu_item_icon_re_findings.md` claimed Weapon `iconId = 0xBF`/`0xC0`. **DISPROVED LIVE.**
- Our `[CALIB]` density scan over all 6643 EquipParamWeapon rows: offset `0xC0` has only
  **`distinct=2` nonzero values** (range[100,999]) → a near-constant field, **NOT iconId** (a real
  iconId column has hundreds of distinct values — icons vary per weapon family). The "Dagger row
  1000 = 100 @ 0xC0, max=999" that we treated as confirmation was a **coincidence** (some other field
  = 100 for daggers). So the Ghidra static offset was unreliable (wrong field / pre-SOTE struct).
- ⇒ The deliverable must be the offset read by the ACTUAL icon-populator instruction, LIVE-verified
  against a known item's true iconId (from our `[ICONMAP]` capture), not a paramdef byte-sum.

## What we already have (anchors)
- Param walk: any row's absolute base is logged (`[EQUIPADDR] <param> row_id base=0x… stride=0x…`),
  via `from::params` (`param_list_address` → `ParamResCap.param_name`@+0x18 → `ParamHeader`@+0x78 →
  `ParamTable`; rows `ParamRowInfo{id@+0, param_offset@+8}`, 24B).
- `CreateImage` hook (`FUN_140d6bbc0`): when the menu draws an item icon we log
  `[ICONMAP] 'MENU_FL_<N>_ptl'` → `N` = that item's TRUE iconId (e.g. 40144). This is our live
  ground-truth value for verification.
- From prior findings: formatter `FUN_14073d9d0` (`0x73d9d0`) builds `MENU_ItemIcon_%05d` from an
  iconRef `{u8 type@+0, s32 id@+4}`; generic image widget `FUN_14074bcc0` (`0x74bcc0`). The id in
  that iconRef IS the iconId — but it's already extracted; we want WHERE it's read from the row.

## Asks
1. **The hovered/selected-item structure ("real cursor").** In the inventory/equipment menu, where in
   memory is the CURRENTLY-HOVERED item's **ID** (and ideally its `EquipParam` row pointer)? Give a
   static pointer path / AOB / offsets so the DLL can read "what is hovered" live. (The user's idea:
   hover an item → know which item the icon belongs to → anchor it to the `[ICONMAP]` iconId.)
2. **The item-icon populator + the offset (the real prize).** Find the function that, for a list/
   hovered item, READS its `EquipParam.iconId` and builds the iconRef `{type,id}` that feeds
   `FUN_14073d9d0`. Report its read instruction(s) — `movzx r32, word ptr [rowReg + 0xXX]` — for each
   item type: **Weapon, Protector (iconIdM/iconIdF), Accessory, Goods, Gem (and Magic if it differs)**.
   `0xXX` = the authoritative offset. VERIFY: for a known item (e.g. a Dagger) the value read = the
   `MENU_FL_<N>` we capture live. (Different inventory tabs / param types likely have distinct read
   sites with distinct displacements — capture each.)
3. **Confirm iconId→sprite-rect is GFX-only.** We believe the rect lives in the Scaleform `.gfx` /
   TPF atlas, reachable only via the `CreateImage` capture (reach-limited) or FFDEC — i.e. the C++
   side ends at the iconId VALUE. Confirm or correct.

## Deliverable
- Hovered-item ID location (path / AOB / offset) — the "real cursor".
- Per-param `iconId` read offsets from the populator's ACTUAL instructions, live-verified vs
  `[ICONMAP]`. (RVAs of the read sites + the populator fn.)
- GFX-only confirmation for the iconId→rect step.

## Reach note (so the offsets are used correctly)
The hovered-item read + the populator + `CreateImage` are all RUNTIME-DISPLAY hooks → they only see
items the menu DRAWS. The per-item-loot-icon feature needs iconId for items the player has NOT seen
(loot in a chest before pickup) → that requires reading the offset (deliverable #2) against the
STATIC `EquipParam` table for ANY row. So #2 is the generalizing prize; #1 is the calibration entry
point + a "seen items" cache. Sprite PIXELS for un-displayed items still need FFDEC regardless.
