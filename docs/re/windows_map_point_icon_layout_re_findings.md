# RE findings — map-point iconId→rect is NOT in eldenring.exe (it's layout/gfx data) → bake or runtime-parse the sblytbnd

Answers `docs/re/windows_map_point_icon_layout_re_prompt.md`. Static Ghidra RE (`D:\ghidra_proj2\ER`,
`find_pinicon` + re_v119/v124 context). App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`.

---

## 0. TL;DR

Map-point icons resolve through the **same per-pin icon-descriptor path as warp pins**, not through a
name-keyed `CSTextureImage` the find hook can surface. So:

- **Lead 1 (a resident exe-side iconId→rect / name→rect table) is a DEAD END for a clean mapping** —
  the engine doesn't hold an `iconId → (x0,y0,x1,y1)` table you can read; the rect lives in the
  **icon-layout (sblytbnd) / gfx movie**, selected by a frame/descriptor. This matches the brief's
  observation that the find hook only ever logs `MapTile/ItemIcon/StatusIcon`, never map-points.
- **The authoritative source is the icon-layout `.sblytbnd`** (`.layout` XML: `SubTexture name x y w h`)
  plus the **iconId→SubTexture-name** convention. Getting that = brief **lead 2 (runtime force-load +
  parse)** or **lead 3 (offline-bake)**. Recommended: **lead 3** (deterministic, reuses
  `tools/extract_subtextures.py`, committed per-profile like the rest of the baked data); lead 2 is the
  runtime fallback reusing the existing Oodle/BND4 scaffold.
- **The real crux for BOTH leads = iconId → SubTexture name.** That answer is in the *asset data*
  (the `.layout` names and/or the worldmap `.gfx` frame→symbol), NOT in eldenring.exe — so it must be
  read from the sblytbnd/gfx, not RE'd from code. First concrete step: dump the sblytbnd `.layout`
  names and see whether they encode the iconId (then the table is direct).

---

## 1. The map-point icon path (RE-confirmed)

Point pins (`WorldMapPointPinData`, vtable `0x2ad6688`) share the warp pin's `vt[1] SetTo
FUN_14087ae20`. SetTo binds the `"Icon_0"` child via:
```c
desc = pin->vt[12]();                 // FUN_14087bf20 (point-pin descriptor selector)
FUN_14074bcc0(Icon_0_child, desc);    // the icon-image setter (same as warps)
```
`vt[12] FUN_14087bf20` returns one of the per-layer icon sub-structs **`pin+0x250 / +0x290 / +0x2d0`**
(chosen by a `pin+0x238` map check and a `GetIconId` predicate — it consults
`"CS::WorldMapPointPinData::GetIconId"` via the dev-reg `FUN_1405d1330`). The descriptor at that offset
(`int* d`: `[0]`frame, `[1..4]`rect, `[0xe]`texture, `[0xf]`mode) is what `FUN_14074bcc0` draws.

Crucially, the **sub-rect is resolved from the gfx/layout, not stored as an iconId-keyed rect in the
exe**: the descriptor carries a frame/handle and the actual texture rect comes from the worldmap movie's
image layout (the sblytbnd the movie was built with). That's the whole reason the by-name find path
(`FUN_140d63c30` → `repo+0x80` `std::map<DLWString,CSTextureImage*>`) has entries for `MENU_ItemIcon_*`
(name-encoded iconId) but **not** for map-point icons — they're never resolved by a per-icon name.

## 2. Why lead 1 can't produce iconId→rect

- `repo+0x80` (FD4ResRep by-name tree, `er+0x3d82510`) enumerates resident images keyed by the GFx
  symbol string. Even with the map open, a map-point symbol → rect would only help **if** we knew
  iconId → that symbol's name. The exe has no such map (the pin uses a descriptor/frame, §1).
- No iconId-keyed twin table was found on the repo that yields rects; the icon identity for points is a
  frame/layout selection, which terminates in asset data.

⇒ Reading more eldenring.exe code will not yield `iconId → rect`. The mapping crosses into the
`.sblytbnd`/`.gfx` assets.

## 3. Recommendation — lead 3 (offline-bake), lead 2 as runtime fallback

**Lead 3 (recommended, deterministic):**
1. Locate the worldmap icon-layout sblytbnd for the active profile (ERR sheets differ from vanilla).
2. `tools/extract_subtextures.py` (SoulsFormats BND4 + DDS crop) → list the `.layout` `SubTexture
   name x y w h` entries on the map-point sheet (the 1024×2048 grace sheet, `g_grace_sprite.sheet`).
3. **Determine iconId → SubTexture name** (the crux): inspect the names — if they encode the iconId
   (or map 1:1 to the worldmap gfx frame order), the table is direct; otherwise pull frame→symbol from
   the worldmap `.gfx`. Emit a generated `iconId → (sheet, rect)` C++ table (profile-specific, committed,
   like the other baked data).
4. Overlay `MapPointProvider` returns `{mapSheetTex, uv=rect/dims}` → real ER map icon, one draw call.

**Lead 2 (runtime, reuses the Oodle/BND4 scaffold):** the icon sblytbnd isn't decompressed during map
open (the captured BND4s were `hasLayout=false` — wrong bundles / loaded at boot before the hook). So
`force_load_file(".../<worldmap icon>.sblytbnd.dcx")` (loader `FUN_140d771d0` by groupId) → the Oodle
hook catches the BND4 → fix the `hasLayout` scan (the `.layout` names may not be UTF-16) → parse →
SubTexture name→rect. Same iconId→name crux as lead 3.

## 4. Validate (either lead)
Crop the candidate rect from the captured 1024×2048 sheet (UV = rect/dims) and compare to the live
native icon for a known point (grace iconId 370, Forge of the Giants, Carian Study Hall). Match = correct.

## 5b. RESOLVED — the names DO encode the iconId (inspected `01_common.sblytbnd.dcx`)

Dumped the ERR `mod/menu/hi/01_common.sblytbnd.dcx` layouts (`tools/dump_layout_names.py`). The
map-point icons live in **`SB_MapCursor.layout` → sheet `SB_MapCursor.png`** (~2048×1024), name-encoded:

- **Standard points: `MENU_MAP_<NN>.png`** — `NN` ∈ {04,05,06,07,08,09,10,11,13,14,16,17,18,20,21,22,
  23,24,25,26,27,28,29,30,50} = the **`WORLD_MAP_POINT_PARAM.iconId`** (zero-padded 2-digit). So
  **`iconId → "MENU_MAP_%02d"` on `SB_MapCursor.png` is DIRECT** — each entry carries `x y w h`.
- Special/MP markers are named (`MENU_MAP_Church`, `MENU_MAP_Enemy_0x`, `MENU_MAP_Friend_0x`,
  `MENU_MAP_Host`, `MENU_MAP_Player_0x`, `MENU_MAP_Range`, `MENU_MAP_memo_2x`, …).
- **ERR custom: `SB_MapCursor_ERR.layout` → `SB_MapCursor_ERR.png`** (8 entries): `MENU_MAP_ERR_Boss`,
  `_Camp`, `_Bounty`, `_GraceUnderground`, `_Remembrance`, `_BlueTower`, `_Completed`, `_Completed2`.
  These map to whatever iconIds the ERR mod assigns its custom points (the ERR `WorldMapPointParam`
  rows) — that iconId→name is the only non-trivial bit, taken from the ERR param/config per profile.

⇒ **iconId→rect is solved for standard points** (no gfx frame→symbol needed — the `MENU_MAP_%02d`
convention IS the mapping). Lead 3 bake becomes: parse `SB_MapCursor.layout` (+ `_ERR`) → emit
`iconId → (sheet, x,y,w,h)`; UV = rect / sheet-dims. Validate by cropping vs the live icon.

NB the runtime Oodle-hook capture saw `hasLayout=false` because the icon sblytbnd loads at **boot**
(before the hook) — so for the runtime path (lead 2) `force_load_file` `01_common.sblytbnd.dcx`; but the
**offline bake (lead 3) needs no runtime at all** now that the file + naming are known.

## 5. Handles
- point pin `WorldMapPointPinData` vtable `0x2ad6688`; SetTo `vt[1] FUN_14087ae20` `0x87ae20`; icon
  descriptor selector `vt[12] FUN_14087bf20` `0x87bf20` (→ `pin+0x250/+0x290/+0x2d0`); icon-image setter
  `FUN_14074bcc0` `0x74bcc0`.
- find/repo: `FUN_140d63c30` `0xd63c30`; FD4ResRep singleton `er+0x3d82510`, by-name map `repo+0x80`.
- scaffold: Oodle hook + `g_dds_list` / `g_sblytbnd`, `force_load_file`, sblytbnd loader `FUN_140d771d0`;
  offline `tools/extract_subtextures.py`.
- NOT in exe (don't chase): an `iconId → rect` table — it's `.sblytbnd`/`.gfx` asset data.
```
