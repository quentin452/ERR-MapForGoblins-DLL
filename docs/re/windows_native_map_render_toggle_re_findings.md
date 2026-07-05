# Native world-map render toggle — RE findings (the on/off point for the vmap takeover)

Answers the user's "RE the native map render path so we can just flip it on/off and replace it with the vmap"
(Track C of `imgui_only_map_plan.md` / endgame phase-3 of `virtual_world_multi_world_design.md`;
`single_surface_ui_plan.md`). Static Ghidra on `D:\ghidra_proj2\ER` (imagebase 0x140000000), 2026-07-05.
**Status: render surface identified + toggle strategy settled; the exact draw-vfunc SLOT is the one remaining
pin (§4).**

---

## 0. TL;DR
- **The native map IS `CS::WorldMapDialog`** — a GFx/Scaleform **CSMenu** dialog (vtable **er+0x2b2d7d8**,
  size **0x3ed0**, base CSMenu ctor `FUN_140741960`; `WorldMapViewModel` vtable er+0x2ad82e0, already pinned
  `WORLDMAP_VIEWMODEL_VTABLE_RVA`). It builds a GFx movie of `"Body/…"` sprites and owns every sublist
  (Warp/Area/Marker/Memo/NetPlayer/Activity/PlaceName/SignPuddle/Blood) + the point cursor + time widgets.
- **The SAFE on/off = render-only:** skip the dialog's **draw** while letting its **update/logic** run. Keeps
  fast-travel, page state, cursor, fog/fragments alive → no state-machine break, no freeze. **Do NOT suppress
  the menu open/close** (that risks the known vmap map-context freeze).
- **Cheapest already-built lever = the D3D12 `RSSetScissorRects` detour** (dev Task B2 in `goblin_config`) —
  clip the native map-layer scissor to empty. Crude (clips, doesn't skip work) but needs no menu vtable.
- **Sequence:** RE + wire the toggle NOW (cheap; enables A/B vmap-vs-native + de-risks the takeover), but only
  FLIP it off in production AFTER Track A parity + **Track B fast-travel** (the map's #1 job). = endgame phase 3.

## 1. Why WorldMapDialog is the surface (evidence)
`WorldMapDialog` ctor `FUN_1409cf8f0` (er+0x9cf8f0): `*self = CS::WorldMapDialog::vftable; FUN_1409c1080();`
(base = `WorldMapDialogBase`), deleting-dtor frees **0x3ed0** bytes. The base ctor `FUN_1409be5e0`
(er+0x9be5e0, 9.5 KB) calls the **CSMenu base ctor `FUN_140741960`**, stamps `CS::WorldMapDialogBase::vftable`,
and builds the GFx display tree via `FUN_14074a2f0(movie, out, "Body/…")` (get-sprite-by-path) for:
`Body/_/Base/Player`, `.../WarpList`, `.../AreaList`, `.../MarkerList`, `.../MemoList`, `.../NetPlayerList`,
`.../ActivityList`, `.../PlaceNameList`, `.../SignPuddleList`, `Body/BloodList`, `Body/PointCursor`,
`Body/PlaceName`, `Body/Time`, … So the native map is a **Scaleform menu**, not custom D3D — its pixels come
from the menu system advancing+displaying this GFx movie each frame.

## 2. The render path (how it draws)
The menu manager (`CSFeMan` / `CSMenuMan`, slots pinned `CSFEMAN_SLOT`/`CSMENUMAN_SLOT`) ticks active menus:
each menu's **update** advances its GFx movie + logic, and its **draw** submits the movie to the Scaleform
renderer (→ D3D12). `WorldMapDialog` inherits these vfuncs from CSMenu. **Skipping the draw hides the map
pixels; keeping the update preserves all logic** (fast-travel resolution, page/cursor/fog state).

## 3. Toggle-point candidates (ranked)
1. **`WorldMapDialog` draw vfunc — no-op under a flag (cleanest).** Hook the dialog's draw slot; when the mod
   toggle is on, return without submitting the GFx movie. Keeps update/logic. **Needs the slot (§4).**
2. **D3D12 `RSSetScissorRects` detour — already scaffolded** (`goblin_config` Task B2 `[SCISSOR]`): the native
   map layer draws under a scissor seen only with `mapopen=1`. Force that rect empty → nothing rasterizes.
   No menu vtable needed; crude (work still runs) but robust + transport-level.
3. **`CSFeMan`/`CSMenuMan` draw loop — skip the WorldMapDialog entry** (identify by vtable er+0x2b2d7d8). One
   central hook covers it; slightly more invasive than (1).
- **Avoid:** suppressing the menu OPEN/close or the update (breaks the state machine → freeze; the map drives
  fast-travel/page/fog).

## 4. Remaining pin (one more RE step)
The exact **draw vfunc slot** of `WorldMapDialog`/CSMenu. The vtable-walk prints only ~6 slots, so pin it by
decompiling the **CSMenu draw dispatch** (the FeMan/MenuMan per-menu update+draw loop) and reading the
indirect `(**(code**)(*menu + 0xNN))(menu)` that submits the GFx movie — the same technique that found the
geom `SetWorldMatrix` slot. Then AOB the target draw function (a slot index can't be AOB'd) + self-heal the
slot at runtime, like `goblin_geom_move.cpp resolve_setter`. Alternatively skip this and ship candidate (2)
(the scissor detour) first — it's already scaffolded and needs no slot.

## 5. Sequencing (honest — per the design docs)
- **Now (cheap, do it):** RE/pin the toggle point + wire a dev flag. Lets you **A/B the vmap against the
  native map** live and de-risks the takeover — value even before replacement.
- **Production flip = LAST:** only render-off the native map once **Track A parity** + **Track B fast-travel**
  (grace list → `warp`) are proven. Flipping it off before fast-travel bricks the player's #1 map action.
  This is exactly endgame phase-3 "suppress the native map — LAST + optional"; most value (kill clipping via a
  full-screen mod surface) is already banked without any suppression.

## 6. Anchors
```
native map menu     CS::WorldMapDialog       vtable er+0x2b2d7d8  (size 0x3ed0; ctor FUN_1409cf8f0 er+0x9cf8f0)
  base              CS::WorldMapDialogBase   vtable via FUN_1409be5e0 (er+0x9be5e0); CSMenu base ctor FUN_140741960
  view model        CS::WorldMapViewModel    vtable er+0x2ad82e0  (pinned WORLDMAP_VIEWMODEL_VTABLE_RVA)
  GFx sprite getter FUN_14074a2f0(movie, out, "Body/…")   (Scaleform display-list access = it's a GFx menu)
menu managers       CSFeMan (CSFEMAN_SLOT) / CSMenuMan (CSMENUMAN_SLOT)   [pinned] — own the update+draw loop
already-built lever D3D12 RSSetScissorRects detour (goblin_config Task B2 [SCISSOR], mapopen-tagged)
```
Cross-ref: `imgui_only_map_plan.md` (Track C), `single_surface_ui_plan.md`, `virtual_world_multi_world_design.md`
(endgame phase 3), `windows_csworldmapmenu_re_prompt.md` (the CSWorldMapMenu subsystem).
```
