# RE findings — reading a PARAM field offset (iconId) without baked data

Answers `docs/re/windows_live_paramdef_offset_re_prompt.md`. Static Ghidra RE (`D:\ghidra_proj2\ER`,
scripts `re_v81..v82`) + the live probe (Weapon `iconId=0xC0`). App 2.6.2.0 / ERR 2.2.9.6,
imagebase `0x140000000`. Read-only.

---

## 0. TL;DR

There is **no queryable PARAMDEF in the exe** — ER accesses param fields by **compiled offsets**, so
the offset is *fixed per ER build*, not runtime-drifting. The repo's bundled paramdef XML is simply a
**pre-DLC** version (1 byte short before `iconId`), which is why `0xBF` (XML) ≠ `0xC0` (live build).
The robust, truly zero-offline answer is **self-calibration**: at startup, find each param's `iconId`
offset empirically (the exact method that already found `0xC0`). No baked offset, no paramdef walk,
survives ER patches *and* ERR/Convergence/ERTE regulation swaps.

- **Ask #1 — is the paramdef resident/queryable?** → **NO.** `EQUIP_PARAM_WEAPON_ST`,
  `EquipParamWeapon`, `PARAMDEF`, `ParamdefMetaData` have **zero exe xrefs** (re_v81/v82): the type
  strings + param names live only in the *loaded regulation data*; the exe never looks a param up by
  string and never parses a paramdef at runtime. Field access is compiled (hardcoded offsets). §1.
- **Ask #2 — authoritative offsets** → build-fixed; the live build is the source of truth (Weapon
  `0xC0`). The pre-DLC XML can't be trusted (per-param DLC deltas differ). Get them by
  **self-calibration** (§3) or by shipping the **2.6.2.0/SOTE Paramdex** paramdefs (then the existing
  calculator is correct). §2.
- **Ask #3 — a callable `item→iconId`** → no standalone export; the engine reads `iconId` via the
  **compiled `+0xC0` offset inlined in menu/item-population code**, then stuffs it into a GUI
  `iconRef {u8 type@+0, s32 id@+4}` that the generic widget builder `FUN_14074bcc0` (`0x74bcc0`)
  formats (`MENU_ItemIcon_%05d`). So "calling the engine" still bottoms out at the same offset; the
  mod's own `get_param` + a self-calibrated offset is the equivalent and is version-proof. §4.

---

## 1. Why there's no live paramdef to read (deliverable: the verdict)

Evidence the exe has no paramdef/name system:
- **`EQUIP_PARAM_WEAPON_ST` (UTF-16) — 0 xrefs** (re_v81). The param *type* string is stored in the
  loaded `ParamTable+0x10`, not in the exe's string pool, and no exe code references it.
- **`EquipParamWeapon` (ASCII) — 0 xrefs; `PARAMDEF` — 0; `ParamdefMetaData` — 0** (re_v82). The exe
  contains no param-by-name lookup and no paramdef-parser strings.
- The mod's `get_param(L"EquipParamWeapon")` works only because it walks the **loaded regulation**
  (`ParamResCap.param_name` DLWString `+0x18`) — that name lives in *data*, put there by the param
  loader, not used by engine field-access code.

Conclusion: ER applies the paramdef **at load** (to lay out / default rows) and then accesses every
field through **compiled struct offsets** (`EQUIP_PARAM_WEAPON_ST::iconId` is a fixed `+0xC0` baked
into the exe). There is **no resident name→offset map** to query. So the "read field by name at
runtime" route is not available in-engine — but it isn't needed (§3).

## 2. The offset is build-fixed, and the XML is pre-DLC (deliverable #2)

The exe's compiled offset is constant for a given ER build → `EQUIP_PARAM_WEAPON_ST.iconId = 0xC0`
for 2.6.2.0 (live-confirmed: a Dagger's `100` at `+0xC0`; `u16@0xBF` = `0x6400` garbage). It does
**not** drift between regulation swaps (ERR/Convergence must keep the exe-compatible 2.6.2.0 layout,
else the engine reads wrong fields). The repo's XML computes `0xBF` (an *odd* offset; live is even
`0xC0`) because it is a **pre-Shadow-of-the-Erdtree paramdef** missing ≥1 byte added before this
region. Per-param DLC deltas differ, so you can't just `+1` the others.

Two correct ways to get all five (Weapon/Protector `iconIdM`+`iconIdF`/Accessory/Goods/Gem):
- **(a) Ship the matching paramdefs** — replace `tools/paramdefs/*` with the **ER 2.x / SOTE Paramdex**
  set; then `paramdef_iconid_offset.py` / the Ghidra calc give the *correct* offsets for this build.
- **(b) Self-calibrate at runtime** (§3) — no paramdef at all. Preferred (mod-proof too).

## 3. Recommended: self-calibrating offset finder (zero offline, version+mod proof)

This is the method that already found `0xC0`, generalized. For each `EquipParam*`, at startup walk
its rows (the DLL already reaches `ParamTable` + row bytes) and pick the byte offset whose `u16`
column behaves like an iconId:
```cpp
// pick the offset in a plausible window whose u16 column is: mostly in [1, ~70000],
// non-constant, and (best) contains a KNOWN anchor item's KNOWN iconId.
int find_iconid_off(ParamTable* t, int lo, int hi, uint16_t anchorRowId, uint16_t anchorIcon) {
    int best=-1, bestScore=-1;
    for (int off=lo; off<=hi; ++off) {
        int sane=0, total=0; bool anchorOk=false; std::set<uint16_t> seen;
        for (auto& row : rows(t)) {
            uint16_t v = u16(rowptr(t,row)+off); ++total; seen.insert(v);
            if (v>0 && v<=0xFFFE) ++sane;
            if (row.id==anchorRowId && v==anchorIcon) anchorOk=true;
        }
        int score = sane + (seen.size()>8?1000:0) + (anchorOk?100000:0);
        if (score>bestScore){ bestScore=score; best=off; }
    }
    return best;   // lock once; re-run only on regulation reload
}
```
Anchor by a stable vanilla item (e.g. Dagger `row 1000000` → known `iconId`), or just by the
"sane distribution" heuristic if no anchor. Lock per param, cache, redo on regulation change. This
needs **no paramdef and no hardcoded offset** — exactly the brief's goal.

## 4. The `item→iconId` path (deliverable #3) — no offset-free engine call

The GUI consumes a prebuilt `iconRef {u8 type@+0, s32 id@+4}`: `FUN_14074bcc0` (`0x74bcc0`, generic
image-tag widget, ~25 callers across all menus) calls `FUN_14073d9d0(iconRef, …)` →
`MENU_ItemIcon_%05d % id` (see `windows_menu_item_icon_re_findings.md`). The `iconRef.id` is set by
the **inventory list-population** code, which reads the item's `EquipParam.iconId` at the **compiled
`+0xC0`** and copies it in. So there is no standalone `item→iconId` export that hides the offset — any
"engine call" path still performs the same `+0xC0` read internally. Therefore the mod's existing
`get_param` + a **self-calibrated** offset (§3) is the cleanest equivalent and is strictly more robust
(it adapts to any build/regulation without pinning a menu function). Pinning the exact inventory
populator is a deep menu-data trace with no advantage over §3.

## 5. Handles / what to change

- verdict anchors: `EQUIP_PARAM_WEAPON_ST`/`EquipParamWeapon`/`PARAMDEF` = **0 exe xrefs** (no
  queryable paramdef). iconRef `{u8 type@+0, s32 id@+4}`; resolver `FUN_14073d9d0` `0x73d9d0`; widget
  `FUN_14074bcc0` `0x74bcc0`.
- live offset (this build): `EQUIP_PARAM_WEAPON_ST.iconId = 0xC0` (confirmed). Others: self-calibrate
  (§3) or Paramdex-regen (§2a) — do **not** trust the pre-DLC repo XML.
- DLL: add `find_iconid_off` (§3) to the existing param walk; drop the baked offset table. Optionally
  also refresh `tools/paramdefs/*` to the 2.x Paramdex so offline tooling matches the build.
- Runtime confirm (already done for Weapon): per param, self-calibrate → expect the anchor item's
  known iconId; log the locked offset.
```
