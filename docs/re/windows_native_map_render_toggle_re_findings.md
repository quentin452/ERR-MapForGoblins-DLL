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
- **RECOMMENDED toggle = the D3D12 `RSSetScissorRects` detour** (dev Task B2 in `goblin_config`) — force the
  native map-layer scissor empty. Confirmed the right lever (§4): the menu **draw is a separate Scaleform
  render pass**, not a slot in the update tick — so the scissor (downstream, at the D3D12 rasterize layer) is
  the clean "hide pixels, keep logic" point, needs no menu vtable/slot, and MFG already hooks there.
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

## 4. Draw-slot hunt — RESULT: the menu draw is a SEPARATE render pass ⇒ use candidate 2 (2026-07-05)
Traced the menu system to find candidate-1's draw vfunc slot. Chain:
- CSMenu base ctor `FUN_140741960` (er+0x741960) stamps `DLReferenceCountObject → CS::SceneObjModifier →
  **CS::MenuWindow**` — so the menu base class is **`CS::MenuWindow`** (WorldMapDialog derives from it).
- `CSMenuManImp` ctor `FUN_1407650a0` registers a `CSEzUpdateTask<CSEzTask,CSMenuManImp>` whose execute is
  **`FUN_140766980`** (er+0x766980) — the per-frame menu-manager tick.
- **`FUN_140766980` is the menu UPDATE (logic) tick, NOT draw:** it advances menu logic + dev-hotkey toggles
  (`FUN_140ddb560()==0x2d/0x38/0xa/…`), calling sub-steps `FUN_140767180`/`FUN_1407668f0`/`FUN_1407664f0`. The
  per-menu **DRAW is a separate Scaleform RENDER pass** (the GFx movies rasterize during present, not in this
  logic tick).

**⇒ Conclusion:** the menu-level draw vfunc lives in the Scaleform render path (deeper — chasing it is
several more runs, uncertain), while the **update** is `FUN_140766980` (leave it running = keeps logic alive).
So **candidate 1 (draw-vfunc no-op) is NOT the cheap path.** **Build the toggle as candidate 2 — the D3D12
`RSSetScissorRects` detour** (already scaffolded, `goblin_config` Task B2 `[SCISSOR]`): the native map's GFx
rasterizes under a scissor seen only with `mapopen=1`; forcing that rect empty hides the pixels at the exact
render layer, needs NO menu vtable/slot, and MFG already hooks there. This IS the "render-only, keep logic"
toggle the design wants — the scissor is downstream of the untouched update tick.

## 4b. Combat gate (follow-up — noted, not yet RE'd)
The user's map↔combat gate is TWO uses of one state: (UX) in combat → cut the vmap OR show an "in combat"
badge; (CORRECTNESS) in combat → disable `warp` in the grace-list (ER forbids warping in combat; the vmap,
being mod-drawn, must replicate that gate or it would let the player warp illegally — the fast-travel
game-breaking risk the design docs flag). **Follow-up RE:** read the combat/danger state — cleanest is the
game's own "warp allowed?" gate (must replicate it for fast-travel anyway); alternatives = a battle/danger
flag on WorldChrMan/LocalPlayer. Deferred to after the render toggle.

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
menu base class     CS::MenuWindow  (CSMenu base ctor FUN_140741960 er+0x741960: DLReferenceCountObject->SceneObjModifier->MenuWindow)
menu managers       CSFeManImp (ctor 0x76b9d0) / CSMenuManImp (ctor 0x7650a0)  [CSFEMAN_SLOT/CSMENUMAN_SLOT pinned]
menu UPDATE tick    FUN_140766980 (er+0x766980)  = CSMenuManImp CSEzUpdateTask execute (LOGIC, not draw — leave running)
RECOMMENDED toggle  D3D12 RSSetScissorRects detour (goblin_config Task B2 [SCISSOR], mapopen-tagged) — draw is a separate Scaleform pass
```
Cross-ref: `imgui_only_map_plan.md` (Track C), `single_surface_ui_plan.md`, `virtual_world_multi_world_design.md`
(endgame phase 3), `windows_csworldmapmenu_re_prompt.md` (the CSWorldMapMenu subsystem).
```
