# Windows-Ghidra RE prompt — FMG slot semantics + the `fmg_set` GoodsName freeze

> **RESOLVED (static, 2026-07-03) → `docs/re/windows_fmg_slot_re_findings.md`.** Goods-name UI reads
> `menu(111)→base(10)→dlc01(319)→dlc02(419)`; inject at base slot 10. The 419 freeze is a
> `fileSize − str_data_start` size_t underflow in our `patch_fmg_in_memory` on a DLC-stub header (not the
> group loop), fixed by an O(1) offset/size guard + rejecting slots ≥300/11x. This prompt kept for context.


## Why
Gap C (custom item end-to-end) needs to inject a NAME string for a custom goods id. Live testing on
ERR/Proton (2026-07-03) found:

- **`fmg_set 10 <goodsId> <text>` works** (base GoodsName slot) — instant, `ok`, game alive, the string
  reads back via `GetMessage`.
- **`fmg_set 419 <goodsId> <text>` FREEZES the game** — `inject_fmg_entries` → `patch_fmg_in_memory`
  runs on the PRESENT thread (RPC commands marshal there); on slot 419 it never returns, the present
  thread stalls (no frames), game dead. `319` untested but same tier. A group-span sanity guard in
  `patch_fmg_in_memory` did NOT prevent it (reverted), so the hang is NOT simply the
  `for id=first_id..last_id` group expansion — it's something structural about slot 419.

We already have a working answer for the immediate goal (use base slot 10). This sweep is to (a) NAIL
which slot the game's own item-name path reads for a goods id, so the injected name actually renders on
the item — not just via our `GetMessage(10,id)` read-back — and (b) explain + de-footgun the 419 freeze.

## What the DLL knows now (anchors — verify/correct these)
- `MsgRepositoryImp` singleton: AOB-resolved (`src/goblin_messages.cpp` `setup_messages`).
  - `repo+0x08` → `base_array` (`uint8_t***`); `repo+0x14` → `count2` (i32, slot count).
  - `base_array[0]` → `sub` (the base-language sub-array of FMG pointers); `sub[slot]` → FMG buffer.
- **`GetMessage` = `FUN_14266d3c0`** (see `windows_native_msg_getter_re_findings.md`):
  `const wchar_t* GetMessage(void* repo, uint32_t group /*=0*/, uint32_t fmgId /*physical slot*/,
  uint32_t msgId)`. The DLL calls it as `g_get_message(repo, 0, fmg_slot, msg_id)`.
- FMG-v2 buffer layout the DLL's `patch_fmg_in_memory` assumes: `+0x00 version(==0x00020000)`,
  `+0x04 fileSize`, `+0x0C groupCount`, `+0x10 stringCount`, `+0x18 stringOffsetsPtr` (relative or
  absolute), groups at `+0x28` = `{ i32 string_index, i32 first_id, i32 last_id }` (stride 0xC),
  then the u64 string-offset array, then UTF-16 string data.
- Slot fallback chains the DLL's own lookup uses (`goblin_messages.cpp` ~L51-61):
  `kGoods={419,319,10}`, `kWeapon={410,310,11}`, `kPlace={429,329,19}`. PlaceName is injected at the
  BASE slot **19** at boot and works. So base tiers are 10/11/19; 3xx/4xx are DLC/menu tiers.

## Questions (Ghidra, static on the ERR eldenring.exe / `D:\ghidra_proj2\ER`)
1. **`MsgRepositoryImp` layout + slot indexing.** Decompile the ctor/loader that fills the sub-array
   (`base_array[0]`). Confirm `+0x08`/`+0x14`. What is the slot index space — is there a fixed
   `enum`/table mapping FMG NAME → physical index (GoodsName=10, WeaponName=11, PlaceName=19, and what
   319/419/410/429 are)? Is there more than one sub-array (`base_array[0..N]` = languages? DLC layers?)
   and does `sub[419]` in `base_array[0]` even hold a GoodsName FMG, or a different resource?
2. **Item-name resolution path.** Find how the inventory/menu renders a goods item's name from its id.
   Which `fmgId` does it pass to `GetMessage` (or a wrapper) for a goods name — is it 10, or does it
   consult 419/319 first and fall through? Trace `EquipParamGoods` → `msgId` → `GetMessage(slot,id)`.
   Deliver the exact slot(s) and order so an injected name at that slot renders on the item in the menu.
3. **Why slot 419 hangs `patch_fmg_in_memory`.** Two sub-questions Ghidra can answer without the runtime
   FMG bytes: (a) Is 419 a normal FMG-v2, or a different structure/version (so our `+0x0C group_cnt`,
   `+0x10 string_cnt`, `+0x28` group layout are garbage → the group loop reads junk `first_id/last_id`
   and spins)? Find the FMG parser the game uses on load and confirm the header/group layout per slot
   tier. (b) Does the game store 419 as a *merged/large* GoodsName (all DLC goods) with a group table
   whose ranges are legitimately huge/sparse? Either way, tell us the invariant that distinguishes a
   safe-to-patch base FMG from a hazardous one, so the DLL can reject the bad slot in O(1) BEFORE the
   present-thread patch.
4. **Correct injectable slot per category** for a NEW custom id (goods/weapon/armor/accessory): the base
   slot the item-name path reads AND that `patch_fmg_in_memory` can safely rebuild.

## Deliverable
`docs/re/windows_fmg_slot_re_findings.md`: the slot→name table (or the enum), the item-name
`GetMessage` slot + order, the FMG-format/why-419-hangs explanation, and the O(1) invariant to reject a
hazardous slot. With that, the DLL fix is: inject names at the confirmed base slot + guard
`inject_fmg_entries` to refuse non-base/hazardous slots (fast error, never a present-thread freeze).

## Notes
- Static only; the FMG string data is runtime (loaded from `item.msgbnd`/`menu.msgbnd`) — answer the
  STRUCTURE/slot questions, not the live contents. If a live header dump of `sub[419]` is needed, the
  DLL can add an `fmg_dump <slot>` RPC (reads `+0x00..+0x40` of `sub[slot]`) — but try static first.
- The immediate Gap C name path already works via slot 10; this sweep hardens it + kills the footgun.
