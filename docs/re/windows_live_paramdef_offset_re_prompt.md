# RE brief — read a PARAM field offset (iconId) from the LIVE PARAMDEF, not a baked one

Goal: the DLL should resolve `EquipParam*.iconId` (and any field) by **field NAME at runtime**, with
**zero offline/baked data** — robust to ER patches and to ERR/Convergence/ERTE regulation swaps.
App **2.6.2.0 / ERR 2.2.9.6**, imagebase `0x140000000`. Read-only RE (Ghidra static + light CT to
confirm pointers). This unblocks per-item overlay loot icons (the item→iconId half; the sprite-pixel
half is separate — see `windows_runtime_icon_textures_followup_re_findings.md`).

## Why — a baked paramdef DRIFTS (proven this session)
We wrote a pure offset calculator (`tools/paramdef_iconid_offset.py`) that replicates SoulsFormats'
field-layout / bitfield packing (type-strict: a bitfield opens a new storage byte when its underlying
TYPE changes — `u8 x:1` then `dummy8 y:7` = 2 bytes). On the repo's **vanilla** paramdef XML it
computes `EquipParamWeapon.iconId = 0xBF`, **exactly matching the earlier Ghidra static finding**
(`windows_menu_item_icon_re_findings.md`). Two independent STATIC methods agree on `0xBF`.

But the **LIVE probe proves `iconId = 0xC0`** (raw bytes: a Dagger's `0x64`=100 sits at byte `0xC0`;
reading u16@`0xBF` yields `0x6400`=garbage, u16@`0xC0` yields a clean `0..999` range over all 6643
rows). → **The repo's vanilla paramdef is 1 byte STALE vs ERR's actually-loaded regulation** (a field
added/resized before iconId). So ANY baked offset/paramdef is version+mod-coupled and silently wrong.
Only the PARAMDEF the game itself loaded is authoritative. Hence: read it live.

## What the DLL ALREADY has (anchors — `src/from/params.hpp`)
The DLL walks params live today:
```
param_list_address (ParamList**)
  → ParamList.entries[i].param_res_cap (ParamResCap*)
      ParamResCap.param_name  : DLWString  @ +0x18   ("EquipParamWeapon" — the SHORT name)
      ParamResCap.param_header : ParamHeader* @ +0x78 → ParamHeader.param_table @ +0x80 (ParamTable*)
  ParamTable: num_rows @ +0x0A, param_type_offset @ +0x10 (→ "EQUIP_PARAM_WEAPON_ST" type string),
              rows[] @ +0x40 (ParamRowInfo{row_id, param_offset, param_end_offset})
  row data = (byte*)ParamTable + row.param_offset
```
So we already reach each PARAM object + its type string ("EQUIP_PARAM_WEAPON_ST") and the row bytes.
We do NOT currently have the paramdef (field name→offset). That's the gap.

## The asks
1. **Is the PARAMDEF resident in memory at runtime?** Many engines drop it after applying the row
   layout. Determine yes/no for ER 2.6.2.0. If YES:
   - Where — a registry/repository keyed by the param TYPE string (`EQUIP_PARAM_WEAPON_ST`) or short
     name, or a pointer hanging off the PARAM object (`ParamResCap`/`ParamHeader`/`ParamTable`)? Give
     the field offset / lookup fn (RVA) to get the PARAMDEF object for a given param.
   - The PARAMDEF object layout: how to enumerate its FIELDS, and for each field get (a) its NAME
     string and (b) either a stored byte offset OR the (type, bitSize, arrayLen) needed to compute
     it. If offsets are stored per-field, that's the jackpot — we read `iconId`'s offset directly.
2. **If the paramdef is NOT in memory:** give the AUTHORITATIVE iconId byte offset for THIS build
   from the decomp's actual field access (the load instruction), for:
   `EQUIP_PARAM_WEAPON_ST.iconId` (live=`0xC0`, confirm), `EQUIP_PARAM_PROTECTOR_ST.iconIdM`/`iconIdF`,
   `EQUIP_PARAM_ACCESSORY_ST.iconId`, `EQUIP_PARAM_GOODS_ST.iconId`, `EQUIP_PARAM_GEM_ST` (if it
   carries an icon). These are our fallback hardcodes (version-pinned, but correct).
3. **Bonus — the no-offset route.** From `windows_menu_item_icon_re_findings.md`: resolver
   `FUN_14073d9d0` (0x73d9d0) formats `MENU_ItemIcon_%05d` from an iconRef `{u8 type@+0, s32 id@+4}`
   (resolver `FUN_14074bcc0` @0x74bcc0). What builds that iconRef from an item (row_id → iconRef)? If
   there's a clean engine fn `item → iconId`, we can CALL/replicate it and skip field offsets
   entirely (best for zero-offline + zero-paramdef-walk). Give its RVA + signature + a worked example.

## Deliverable
- Verdict: is the live PARAMDEF reachable in memory? If yes → the lookup path (param type → paramdef
  object) + the field-entry layout (name + stored-offset, or fields to compute), as RVAs/offsets the
  DLL can walk. If no → the authoritative per-param iconId offsets for build 2.6.2.0 (item #2).
- The item→iconRef builder fn (RVA + sig + worked example) if one exists (item #3).
- Handles (RVAs / RTTI vtable names / AOBs) so the DLL can verify. The DLL reaches every PARAM object
  live (chain above) and can hook/scan — give the anchors and we confirm with a probe.
```
verify in-DLL: walk param_list → match param_name=="EquipParamWeapon" → ParamTable → for the paramdef
object you return, enumerate fields, find "iconId", read its offset, read row → expect Dagger(row 1000-ish)=100.
```
