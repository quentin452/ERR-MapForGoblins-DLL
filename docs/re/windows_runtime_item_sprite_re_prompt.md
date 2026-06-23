# RE brief — runtime item-icon SPRITE resolution (mod-robust, no FFDEC)

Goal: for an ARBITRARY item iconId (from a loot marker — an item the player may never display), get
its icon SPRITE (sub-rect + pixels) at runtime from the **currently-loaded** GFX/TPF, so it
auto-matches whatever mod is active (ERR / Convergence / ERTE all ship custom item icons → a baked
vanilla atlas would be WRONG; the loaded gfx is the only correct source). No FFDEC, no per-profile
re-bake. App **2.6.2.0 / ERR 2.2.9.6**, imagebase `0x140000000`. Read-only RE (Ghidra static + light
CT/probe confirm).

## What is ALREADY solved (so this brief is only about the SPRITE half)
- **iconId per item = DONE, live, mod-robust.** item row → `EquipParam*.iconId` (u16) at fixed
  compiled offsets (paramdef/exe-stable, mod-invariant): Weapon `0xBE`, Protector `iconIdM 0xA6`/
  `iconIdF 0xA8`, Accessory `0x26`, Goods `0x30`, Gem `0x04` (cross-verified: current SOTE Paramdex ×
  live high-distinct columns; Accessory/Gem show textbook sequential iconIds). The DLL reads any
  item's iconId live.
- **The icon pipeline (prior findings, `windows_menu_item_icon_re_findings.md` + `..runtime_icon_textures_followup_re_findings.md`):**
  item → iconId → `FUN_14073d9d0` (`0x73d9d0`) formats `MENU_ItemIcon_%05d` from iconRef
  `{u8 type@+0, s32 id@+4}` → GFX resolves the symbol to a sprite. ERR names the symbol
  **`MENU_FL_<id>`**. Generic image widget = `FUN_14074bcc0` (`0x74bcc0`).
- **CreateImage hook (we own it):** `CSScaleformImageCreator::CreateImage` (`FUN_140d6bbc0`, vt[1];
  AOB `WORLDMAP_CREATE_IMAGE`) returns `CS::CSTextureImage` (vtable RVA `0x2bb8910`). On it: sprite
  **RECT** at `+0x74/+0x78/+0x7c/+0x80` (x0,y0,x1,y1), dims `+0x84/+0x88`, GFx import NAME at `+0x40`
  (`'MENU_FL_<id>_ptl'`). Item icons live on a **2048×2048** menu atlas (74×74 sprites).
  **⛔ LIMIT: CreateImage only fires for icons the menu actually DRAWS** → reach-limited to displayed
  items. Only 3 distinct `MENU_FL_<id>` ever captured live (the Recent-Items panel); the grid
  thumbnails use a different path. This is the wall this brief must get past.
- **Sheet GPU texture chain (SOLVED, but only post-draw):** `CSTextureImage+0x10` → Scaleform
  `Render::Texture` (vtable RVA `0x2c0f318`, bound LAZILY on first render) → `+0x70` →
  `ID3D12Resource` (vkd3d; `+0x10` i32 DIM==3, `+0x20` Width, `+0x28` Height). Sheets dedup by ptr.
- **DX12 infra we have (goblin_overlay.cpp):** the game's `ID3D12Device` (`g_device`), command queue
  (`g_command_queue`), SRV heap (`g_srv_heap`) — enough to `CopyTextureRegion` a sub-rect GPU→GPU
  into our own SRV and sample it in ImGui (BCn-direct, no CPU de-pitch).

## The asks (get the sprite for an UN-displayed iconId)
1. **Loaded-movie symbol→rect table (preferred — no draw needed).** The Scaleform GFx movie that owns
   the item icons has a symbol/character/import dictionary mapping `MENU_FL_<id>` (or
   `MENU_ItemIcon_<id>`) → its sprite sub-rect on the 2048×2048 sheet. Where is the loaded
   `GFxMovieView`/`MovieDef`/resource table for the inventory menu, and how do we walk it to look up
   **ANY** iconId's rect WITHOUT the game drawing it? (GFx `MovieDef` resource/import table, or the
   sprite-sheet character def list.) This is the clean unlock: enumerate all rects from the loaded
   gfx → mod-correct by construction.
2. **Draw-free path to the sheet `ID3D12Resource`.** Today we only reach the sheet resource via
   `CSTextureImage+0x10→Render::Texture+0x70` AFTER a draw binds it. Find a draw-free route to the
   item-icon sheet's `ID3D12Resource` — e.g. a TPF/texture repository keyed by the sheet name, or the
   movie's backing texture object — so we can `CopyTextureRegion` rects for items never displayed.
3. **Force-create fallback (if #1 isn't walkable).** A callable function that, given an iconId (or the
   string `MENU_ItemIcon_<id>`), creates/resolves the GFx image OFF-SCREEN so our existing CreateImage
   hook captures its rect. Identify the function (RVA), the context it needs (a live `GFxMovieView`?
   the inventory menu loaded?), and whether calling it for arbitrary ids off the normal draw path is
   safe. (This trades a clean table-walk for invoking the engine's own resolver per needed id.)
4. **Confirm mod-robustness.** Verify the rects come from the LOADED `.gfx` (not a static exe table) —
   so the same code auto-adapts to ERR/Convergence/ERTE custom item icons. If any part is baked in the
   exe, say which.

## Deliverable
- Either the **loaded-movie symbol→rect lookup** (RVAs/offsets to walk the GFx movie's sprite table),
  or the **force-create call** (RVA + required context), so we can resolve `MENU_FL_<id>`→rect for an
  arbitrary iconId without the player displaying the item.
- The **draw-free `ID3D12Resource`** path to the item-icon sheet.
- Confirmation the rects are loaded-gfx (mod-robust), not exe-baked.

## Recipe we will implement (the consumer, so offsets/handles are actionable)
Per loot-marker item: live `iconId` (SOLVED) → look up `MENU_FL_<id>` rect in the loaded movie (#1)
[or force-create (#3)] → `CopyTextureRegion` that sub-rect from the sheet `ID3D12Resource` (#2) into
our SRV (`g_device`/`g_command_queue`/`g_srv_heap`) → per-icon UV → draw on the overlay marker. Cold
fallback = the baked per-category PNG atlas (mirrors how live_projection falls back to baked).
Verify with our CreateImage hook (`[ICONMAP]` rect for a displayed item must match the table lookup
for the same id) + a DX12 readback of one rect.
