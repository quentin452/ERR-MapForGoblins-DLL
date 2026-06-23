# RE findings — item→param dispatch + the authoritative iconId offset (the right way)

Answers `docs/re/windows_menu_cursor_iconid_populator_re_prompt.md`. Static Ghidra RE
(`D:\ghidra_proj2\ER`, scripts `re_v83..v87`). App 2.6.2.0 / ERR 2.2.9.6, imagebase
`0x140000000`. Read-only.

---

## 0. TL;DR — honest result

I mapped the **item→EquipParam dispatch architecture** and pinned the **per-category row
resolvers** (clean hook/call points that hand you the exact `EquipParam*` row for an item id). I did
**not** isolate the single `movzx [row+0xXX]` iconId instruction — it's one of *dozens* of
near-identical per-property getters, and statically picking THE icon one with confidence wasn't
achievable in a reasonable sweep (6 passes found the text getters and several numeric getters, not a
unique icon read). **But that instruction isn't actually needed** — with the resolvers below you get
the row deterministically and confirm the offset per-category against the live `[ICONMAP]` ground
truth in one shot. That is the reliable path; the byte-sum and single-anchor methods that misfired
(`0xBF`/`0xC0`) are both retired.

- **Ask #1 — hovered item:** the menu holds an **item handle**; `FUN_14073d600(handle, &out)` →
  the **item id** (`category = id>>0x1C`: 0=weapon, 1=protector, 2=accessory, 4=goods, 8=spirit-ash;
  `rowId = id & 0x0FFFFFFF`). The "real cursor" feeds this id into the resolvers (§1). §2.
- **Ask #2 — the offset:** don't hunt the instruction; **resolve the row (§1) and find the u16
  offset whose value == the captured iconId** for ≥2 known items per category (per-item
  deterministic, kills the statistical ambiguity). Goods candidate `0x20` — verify with a 2nd goods
  anchor. §3.
- **Ask #3 — iconId→rect:** **GFx-only**, confirmed — the C++ side ends at the iconId *value*; the
  sub-rect is the gfx sprite-sheet (CreateImage capture / FFDEC). §4.

---

## 1. The per-category EquipParam-row resolvers (the key deliverable)

Each takes `(wrapper*, rowId)`, walks the SoloParam chain `*(*(x+0x80)+0x80)` = the param **table**,
**binary-searches** it for the row (note the `/100*100` id-base grouping), and stashes the resolved
`EquipParam*` row ptr into the wrapper (**row/data ptr at `wrapper+0x8`**; `wrapper+0x8 != 0` ⇒ found):

| category (`id>>0x1C`) | param | resolver RVA |
|---|---|---|
| 0 | EquipParamWeapon | **`FUN_140d54600`** |
| 1 | EquipParamProtector | **`FUN_140d47460`** |
| 2 | EquipParamAccessory | **`FUN_140d21eb0`** |
| 4 | EquipParamGoods | **`FUN_140d39df0`** |
| 8 | spirit-ash (Goods subset) | `FUN_140d2a360` |

item-handle→id helper: **`FUN_14073d600`** (`0x73d600`). Category dispatch (`id & 0xF0000000`) +
these resolvers are the whole item→row mechanism; the menu's many item-property getters
(`0x674xxx`/`0x675xxx`, e.g. text via `FUN_140d10430`/the FMG mgr `DAT_143d7d4f8`; numeric via a
direct `[row+off]` read like `FUN_140674680` reading `row+0x3a`) all sit on top of these.

**These resolvers ARE the confirm tool:** hook one (or call it — the mod already does the equivalent
param-table search in `get_param`) to get the `EquipParam*` row for any item id, then read/scan its
fields. No baked offset, no paramdef.

## 2. The hovered item ("real cursor") — ask #1

The menu code carries the selected item as a **handle** that `FUN_14073d600` turns into the id. The
robust, menu-agnostic way to get the hovered item's id + row for the DLL: when the inventory draws an
item icon (your `CreateImage`/`[ICONMAP]` hook already fires there), the same code has the item id in
scope → resolve it via §1 to the `EquipParam*` row. (I did not pin a single static
"current-hovered-item" pointer — ER spreads it across the per-menu data; the id→row resolvers are the
stable mechanism and sidestep needing that pointer.)

## 3. The authoritative offset — resolve + confirm (ask #2, the real prize)

The single read instruction is buried in a large family of near-identical getters; rather than guess
which, get the value **directly and deterministically**:

```
for a KNOWN item whose iconId you captured live ([ICONMAP] MENU_FL_<N>):
  row = resolve(category, id & 0x0FFFFFFF)        // §1 resolver, or get_param row search
  for off in 0x10..0x40 step 2:                   // candidate window
      if u16(row + off) == N:  record off
  // the TRUE iconId offset is the one that holds N for EVERY known item of that category
```
- Use **≥2 known items per category** — one anchor is ~coincidence-prone (your `[ICONFIND]` Goods
  `0x20` from `40144` is a *candidate*; confirm with a second goods item's `[ICONMAP]` id).
- This is per-item exact (no statistics over the whole table), and version/mod-proof (reads the live
  build's row). Do it once per param type, cache the offset, redo on regulation reload.
- The discredited values: paramdef byte-sum `0xBF` (pre-DLC XML), single-anchor `0xC0` (the `[CALIB]`
  density scan showed `distinct=2` there → not iconId). Do not reuse them.

Why not the instruction: the icon getter is one of ~dozens of `0x674xxx`/`0x675xxx` property getters,
most routed through per-category resolvers or the FMG/manager layer; none is uniquely identifiable as
"icon" without the very runtime confirm above. So the confirm IS the authoritative method here.

## 4. iconId → sprite rect — GFx-only (ask #3, confirmed)

Consistent with every prior finding: the C++ item path ends at the iconId **value** (§3). The
`value → MENU_ItemIcon_%05d → gfx sprite → atlas sub-rect` step lives entirely in the Scaleform
movie; there is no C++/param rect table. The rect is only observable per-draw on the `CSTextureImage`
(`+0x74/+0x78/+0x7c/+0x80`, `windows_runtime_icon_textures_followup_re_findings.md`) or via FFDEC.
For un-displayed loot you get the iconId (§3) but the pixels still require a prior capture / FFDEC.

## 5. Handles

- item-handle→id `FUN_14073d600` `0x73d600`; category `id>>0x1C`, rowId `id & 0x0FFFFFFF`.
- resolvers (§1): weapon `0xd54600`, protector `0xd47460`, accessory `0xd21eb0`, goods `0xd39df0`,
  ash `0xd2a360`. Wrapper row/data ptr @ `wrapper+0x8`. Param chain `*(*(x+0x80)+0x80)` = table.
- offset = resolve+confirm (§3) against `[ICONMAP]`; ≥2 anchors/category. Goods `0x20` to confirm.
- iconId→rect: GFx-only → CreateImage capture / FFDEC.
- The cluster `0x674680/0x674ae0/0x674e50/0x675140/0x675450/0x675680/0x675c40/0x68a700/0x68d2a0` =
  item-property getters (text via `DAT_143d7d4f8`+`FUN_140d10xxx`; predicates; numeric direct reads)
  — sit atop the §1 resolvers; useful map, but the iconId one isn't uniquely flagged statically.
```
