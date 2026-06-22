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

## Runtime evidence so far (2026-06-22, why we pivot to static RE)
We built a live correlator (`[ICONFIND]`, goblin_inject.cpp): each time the menu DRAWS an item icon
we capture `MENU_FL_<N>` and test whether `N` appears in any `EquipParam*` u16 column. Findings:
- `40144` (a drawn **Goods** item) → appears at **EquipParamGoods offset `0x20`** — a CANDIDATE iconId
  offset, but UNCONFIRMED (1 anchor; ~19 random occurrences of any 16-bit value are expected across
  the 7126-row table, so a single hit may be coincidence).
- **Only 3 distinct `MENU_FL_<N>` ever fire** through `CSScaleformImageCreator::CreateImage`
  (`40144,40147,40172` = the "Recent Items"); the inventory GRID thumbnails do NOT go through this
  hook, and re-hovering caches → we cannot generate the many varied anchors needed to CONFIRM by
  convergence. **`40147` and `40172` appear in NO scanned param** (Weapon/Protector/Accessory/Goods/
  Gem/Magic) at any u16 offset → either they are non-EquipParam items (spirit ash / key / etc.) OR
  `MENU_FL_<id>` is a TRANSFORMED id for them (id ≠ raw EquipParam.iconId).
⇒ Runtime correlation is stuck. We need static RE of the **hovered/selected-item structure** (the
detail panel source) — what does it actually hold? If it carries the item ID + a RESOLVED iconId,
that (a) bypasses the offset for displayed items and (b) lets us confirm `Goods 0x20` by mapping
`40144` back to its row. PLUS the populator read offset per param (below) for the un-displayed case.

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
1. **The hovered/selected-item structure ("real cursor") — and DUMP what it holds.** In the
   inventory/equipment menu, where in memory is the CURRENTLY-HOVERED item's data? Give a static
   pointer path / AOB / offsets so the DLL can read it live. **Enumerate the struct's fields** — the
   detail panel renders the item's name + big icon + count + effect, so SOMETHING feeds it: report
   whether it carries (a) the item **ID** (category + row id), (b) a **resolved iconId** or icon
   handle directly, (c) a pointer to the `EquipParam` row. If a resolved iconId sits right there, we
   read it with zero offset for displayed items AND can map `40144`→its Goods row to confirm `0x20`.
   (The user's framing: "see what's hidden in the hovered item.")
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
