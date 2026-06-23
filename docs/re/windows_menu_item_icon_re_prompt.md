# RE brief — how ER resolves an ITEM to its menu icon (inventory/equipment)

Pivot from the worldmap-icon work (`windows_runtime_icon_textures_followup_re_findings.md`, SOLVED:
`img+0x10 → Render::Texture → +0x70 → ID3D12Resource`; sprite name at `img+0x40` = `<symbol>_ptl`).
That solved the GPU-texture + naming chain but only reaches NATIVELY-DRAWN icons. We now want the
INVENTORY/EQUIPMENT item-icon pipeline — the menu draws every item's icon, so it's the reachable
source. App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. Read-only RE wanted (Ghidra static).

## What we CONFIRMED live (Linux/Proton, probe `dump_icon_textures`)
- The general image creator `CSScaleformImageCreator::CreateImage` (`FUN_140d6bbc0`) builds EVERY GFx
  image; the returned `CS::CSTextureImage` (vtable RVA `0x2bb8910`) carries the sprite RECT at
  `+0x74/+0x78/+0x7c/+0x80` (x0,y0,x1,y1), sheet dims `+0x84/+0x88`, and a heap ptr at **`+0x40` →
  the import NAME string `'<symbol>_ptl'`**.
- Opening INVENTORY and hovering items, the captured item-icon names are **`MENU_FL_<NNNNN>_ptl`**
  (e.g. `MENU_FL_40147`, `MENU_FL_40172`, `MENU_FL_40144`) + named ones like
  `MENU_FL_Affinity_Frenzied`, on a **2048×2048** atlas (74×74 sprites). ERR stat-HUD glyphs use
  `SB_ERR_*` on a 2048×1024 sheet (separate).
- Exe (UTF-16) format-string family (the "table" — keyed by NUMERIC id, no name table):
  `MENU_ItemIcon_%05d`, `MENU_PropertyIcon_%05d`, `MENU_StatusIcon_%05d`, `MENU_Load_%05d`,
  `MENU_Tuto_%05d`, plus `%s_ptl` and `<img src='img://%s' width='%d' height='%d' vspace='%d'>`.

## What we COULDN'T resolve at runtime (the asks)
We tried to confirm `MENU_FL_<id>` == `EquipParam*.iconId` by reading each row's iconId live and
matching {40144,40147,40172}. **Zero matches**, and our XML-summed iconId offsets gave garbage
(Protector all 0, Accessory/Goods all 65535, Gem samples 2560/5120 — clearly wrong field). So:

1. **The exact iconId field offset** in `EQUIP_PARAM_WEAPON_ST`, `EQUIP_PARAM_PROTECTOR_ST`
   (`iconIdM`/`iconIdF`), `EQUIP_PARAM_ACCESSORY_ST`, `EQUIP_PARAM_GOODS_ST` — from the DECOMP /
   actual field access, not a paramdef byte-sum. (Our sums: Weapon 0xc2 read a plausible 0–999,
   the rest read garbage → the bitfield packing is off.)

2. **The format/lookup site.** Find the fn that builds `MENU_ItemIcon_%05d` (and/or `MENU_FL_<id>`)
   and trace its `%d` argument back to the SOURCE: is it `EquipParam*.iconId` directly, or an
   indirection (an icon LUT, a MenuItem struct field, an item-type→param dispatch)? A worked example
   for one weapon (its row → the id that reaches the format) is the key deliverable.

3. **`MENU_FL_<id>` vs `MENU_ItemIcon_<id>` — what id space is `40147`?** It sits far above vanilla
   weapon/armor iconId ranges and appears next to `MENU_FL_Affinity_*`. Is `MENU_FL_*` the
   ash-of-war / weapon-affinity / gem icon family (so `40147` is an `EquipParamGem`/affinity id, not
   a Weapon.iconId)? Identify which param/id each `MENU_*` family is keyed by, and which one carries
   the actual per-item inventory icon (weapons, armor, talismans, goods).

4. **iconId → sprite RECT on the 2048×2048 atlas.** Is the sub-rect defined only in the GFx movie
   (Scaleform sprite sheet, FFDEC-only), or is there a C++/param table (frame/atlas layout) keyed by
   id that we can read? If GFx-only, say so (we'll capture per-icon at draw or FFDEC).

## Deliverable
- Authoritative iconId field offsets for the 4 EquipParams (+ Gem if relevant).
- The format-site fn (RVA) + the id's provenance, with one worked weapon example
  (item row → id → `MENU_*_<id>` symbol).
- Which `MENU_*` family = the per-item inventory icon, and the id space of `MENU_FL_<id>`.
- Whether iconId→rect is GFx-only or a readable table.
- Handles (RVAs / RTTI / AOBs) so the DLL probe can confirm. The DLL can read params live
  (`get_param`) + hook CreateImage — give offsets/sites and we verify.
