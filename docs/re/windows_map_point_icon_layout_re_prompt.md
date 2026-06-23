# RE brief — find the map-point icon LAYOUT (iconId → sub-rect on the 1024×2048 map sheet)

App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. Our DLL is in-process on Linux/Proton (vkd3d).
Read-only. Deliverable: a recipe `WORLD_MAP_POINT_PARAM.iconId → (x0,y0,x1,y1)` on the map-point
sheet, so the overlay draws the REAL ER map icon (grace, château, église, donjon…) on each marker.

## What is SOLVED (the texture pipeline — don't re-derive)

The whole "draw ER's own icon textures on our overlay, no game GPU bind" chain works end-to-end:

1. **Oodle hook** — IAT-hook `eldenring.exe`'s import of `oo2core_6_win64!OodleLZ_Decompress`
   (MinHook failed with MH_ERROR_MEMORY_ALLOC; IAT hook works). Every menu `.dcx` ER loads passes
   through; the detour reads the decompressed buffer (it's in our address space post-decompress).
2. **Buffers are TRANSIENT** — ER frees each decompressed buffer after parsing it (reading later =
   "not DDS"). So we COPY out IN the hook. The `.tpf.dcx` decompresses to a `TPF` (its entries are
   headered DDS); we grab the DDS bytes (`g_dds_list`, deduped). Oodle block-decompresses in ~256 KB
   chunks, so big DDS span blocks — small TPFs are self-contained.
3. **Our own texture manager** (`goblin_overlay.cpp`):
   - `create_tex_from_dds_mem` — raw DDS → our `ID3D12Resource` (BC format native, no CPU decode) →
     SRV. Verified: rendered an ER texture (a loading-screen art) from RAM, correct format/colors.
   - `copy_er_sheet_direct` / `copy_sheet_cached` — `CopyResource` a game sheet WHOLE into our texture
     (cached by src ptr) → **sheet-as-atlas**. Markers draw with a UV sub-rect → ONE ImGui draw call
     per sheet (not per icon). This is the perf-correct path.
4. **Item icons DONE** — the find-by-name hook (`FUN_140d63c30`) resolves `MENU_ItemIcon_<iconId>`
   with a REAL sub-rect (`img+0x74..0x80`) on its sheet. So item iconId → rect is FREE (name-encoded).
   `native_item_icon(iconId)` → `harvested_icon` (sheet+rect) → `copy_sheet_cached` → tex + UV.
   Browse-gated for coverage, but iconId↔rect is direct, no layout file needed.

## The REMAINING problem — map-point icons have NO per-icon rect source yet

The 1024×2048 map-point sheet (grace, château, église, donjon, ruines…) is captured (it's the
harvested grace sheet, `g_grace_sprite.sheet`; verified the in-game map icons match it 1:1, e.g.
Forge of the Giants cauldron, Carian Study Hall tower). **But we have no iconId → rect for them:**

- The find hook only logs `MENU_MapTile_*` (full-texture map TILES, rect=0,0), `MENU_ItemIcon_*`,
  `MENU_StatusIcon_*`, `MENU_Load_*` — **never the map-POINT icons**. So map points are drawn by
  Scaleform FRAME (a worldmap-gfx movieclip frame per iconId), not via a per-icon `CSTextureImage`
  find → no rect from this path.
- The Oodle hook caught several BND4 (~64–128 KB) on map open but **all `hasLayout=false`** (no UTF-16
  `.layout` name) — these are other bundles (likely map-tile layouts), NOT the icon sblytbnd. So the
  icon layout sblytbnd is NOT decompressed during map open (loaded at boot before our hook installs,
  or already resident, or our UTF-16 scan is wrong).

## Leads to crack iconId → rect (pick one)

1. **Find the Scaleform map-point frame → sub-rect map.** The worldmap gfx has a map-pin movieclip;
   `WORLD_MAP_POINT_PARAM.iconId` selects its frame. RE where the engine sets that frame from iconId,
   and how the frame maps to a sub-texture rect on the sheet (the gfx's internal sblytbnd layout the
   movie carries in memory). The grace path proves a per-name `CSTextureImage` exists for force-created
   sprites — is there a resident name→rect table for the map icons (repo+0xb0 twin map / the movie's
   image list) keyed by something iconId-derived?
2. **Force the layout to decompress, then grab it.** Either hook `OodleLZ_Decompress` EARLIER (before
   the boot load of `01_common.sblytbnd.dcx`), or `force_load_file("…/01_common.sblytbnd.dcx")` so the
   Oodle hook catches the BND4 → parse BND4 → `.layout` XML (`SubTexture name x y w h`) → name/iconId →
   rect. Fix the `hasLayout` scan if names aren't UTF-16. (We already have oodle + the BND4 capture
   scaffold, `g_sblytbnd`.)
3. **Offline-bake the layout.** `tools/extract_subtextures.py` already parses the sblytbnd (SoulsFormats
   BND4 + DDS crop). Emit a generated `iconId → (sheet, rect)` C++ table. Profile-specific (ERR vs
   vanilla sheets differ), committed. Least RE, but not mod-agnostic.

## Validate

For any candidate iconId→rect: crop that rect from the captured 1024×2048 sheet (UV = rect/dims) and
compare to the live native map icon for a point with that iconId (e.g. grace iconId 370, Forge of the
Giants, Carian Study Hall). Match = the layout is correct.

## Deliverable

`iconId → rect` for the map-point sheet, validated against the live map. Then the overlay's
MapPointProvider returns `{mapSheetTex, uv=rect/dims}` and markers show the real ER map icon — one
draw call (sheet-as-atlas), no game bind, populated from whatever ModEngine served.
