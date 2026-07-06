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

## 4c. ⛔ Candidate 2 (D3D12 RSSetScissorRects) DISPROVEN live — 2026-07-05 (Linux/Proton)
Ran the scissor recon end-to-end (`tmp/scissor_recon.py`: boot → 3× native-map open/close cycles → the
`[SCISSOR]`/`[VIEWPORT]` detour dedup sets, tagged mapopen). **Result: ZERO rects appear mapopen=1-only.**
All 30 distinct scissor rects are GENERIC engine passes present every frame regardless of the map — shadow
atlas tiles `(3072,1024)-(4096,2048)` 1024², the mip chain `512/256/128/64/16/4/1`, full-screen
`1920×1080` / `960×540`. The tagging is sound (proven by the ONE mapopen=0-only rect = the **minimap**
`(1681,24)-(1895,238)` 214², correctly present only when the map is CLOSED). So a distinct map scissor would
have shown as mapopen=1-only — none did.

**Conclusion:** the Scaleform world-map rasterizes **full-screen through the same generic engine scissors**;
there is **no map-specific scissor rect to force empty**. Emptying the shared full-screen rect while the map
is open would blank the WHOLE frame (game render included), not just the map = black screen, not a cull.
**Candidate 2 is dead for isolating the map.** (The scissor detour stays useful as-is for the `MAPCLIP`/
resolution diagnostics, just not as the cull lever.)

**⇒ Pivot to the GFx-layer lever (candidate 1, but reached without the draw-vfunc slot):** the native map IS
the Scaleform movie `02_120_worldmap.gfx`, and its render **clip rect is already RE'd** at `MovieImpl+0xB0`
(int L,T,W,H, virtual 1920×1080) via `WorldMapDialog+0x140 → +0x00`
(`worldmap_native_clip_b3_scaleform_re_findings.md`, there READ for our marker clip). Forcing that clip to
`(0,0,0,0)` each frame stops the movie rasterizing while `WorldMapDialog`'s UPDATE tick keeps running = the
exact "render-only, keep logic" toggle — no menu vtable/slot needed, and **Linux-doable** (the offsets are
live-verified reads). Implemented behind the dev RPC `movieclip read|hide|show` +
`worldmap_probe::movieclip_maintain()` (per-present zero-write) 2026-07-05; live GO/NO-GO test in progress.

## 4d. ⛔ MovieImpl+0xB0 clip write ALSO inert for cull — 2026-07-05 (Linux/Proton)
Implemented + live-tested the §4c pivot (`movieclip read|hide|show` RPC + `movieclip_maintain()` per-present
zero-write; `tmp/movieclip_test.py`). **The resolve is CORRECT** — with the two-deref chain
`WorldMapDialog+0x140 → *(+0x00)=MovieImpl`, `read` returns `clip=(0,0,1920,1080) buf=(1920,1080)` (matches the
findings; a one-deref bug first read garbage `buf=(942682421,…)` — the fix is: movieHandle is a POINTER at
`dialog+0x140`, MovieImpl is a POINTER at `movieHandle+0x00`, so TWO derefs, validated by `buf==1920×1080`).
**But forcing `+0xB0` to `(0,0,0,0)` does NOT hide the map:** the write HOLDS (read-back stays `(0,0,0,0)`, the
engine does not rewrite it) yet the native map + all markers render unchanged (screenshots `mc_before/hide/show`).
Clean round-trip, no crash, menu logic intact throughout (map closes normally).

**Conclusion:** `MovieImpl+0xB0` is a **descriptive** viewport (the map's screen extent — what B3 READ it for, to
clip OUR markers), **not a render-enable the GFx pass consults.** So BOTH cheap levers are dead: candidate 2
(D3D12 scissor, §4c — no map-specific rect) and this GFx clip write. The `movieclip read` verb is KEPT as a live
map-viewport diagnostic; `hide/show` are kept as reusable scaffolding (resolve + per-frame maintain) for a future
**movie visible/enable-flag** hunt, but are INERT for culling today.

**⇒ What's actually left for the cull** (all NOT cheap, and the production flip is GATED on Track A parity +
Track B fast-travel anyway, so this is not urgent):
- **Candidate 1 — the Scaleform DRAW vfunc no-op** (skip submitting the movie's GFx render pass). Needs the draw
  slot — findings §4 flagged this as "several uncertain Ghidra runs" (Windows). The real remaining path.
  **← CHOSEN 2026-07-06 (user): pursue this. Sharpened Windows prompt written →
  `windows_native_map_drawvfunc_re_prompt.md`** (has the anchors, the two dead levers to skip, and a
  fallback B = a render-enable field on `CSScaleformSwfPlayer`/MovieImpl).
- **A movie/player visible/enable flag** — blind RPM field-scan of MovieImpl / `CSScaleformSwfPlayer`
  (`movieHandle+0x58`) for a boolean that gates render. Linux-doable but a risky spike (flipping unknown fields
  can crash); reuse the `movieclip` scaffolding.
- **Candidate 3 — CSMenuMan draw-loop skip the WorldMapDialog entry** (Windows RE).

**Recommendation:** since M5's cheap Linux levers are exhausted AND the production flip is gated + not-cheap,
**deprioritize the cull** and keep advancing the vmap migration on UNGATED bricks; pick up the cull via candidate 1
(Windows Ghidra) when the Windows/RE track has a slot.

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
toggle (DISPROVEN)  D3D12 RSSetScissorRects — DEAD (§4c): map draws full-screen through generic engine scissors, no map-specific rect
RECOMMENDED toggle  GFx clip write: MovieImpl+0xB0 (int L,T,W,H) = 0 via WorldMapDialog+0x140->+0x00 — RPC movieclip, movieclip_maintain() per frame
```
Cross-ref: `imgui_only_map_plan.md` (Track C), `single_surface_ui_plan.md`, `virtual_world_multi_world_design.md`
(endgame phase 3), `windows_csworldmapmenu_re_prompt.md` (the CSWorldMapMenu subsystem).
```
