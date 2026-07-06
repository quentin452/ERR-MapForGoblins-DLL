# Task: RE the WorldMapDialog DRAW vfunc — no-op the native map's Scaleform render pass (Windows, Ghidra)

You are on **Windows** with **Ghidra/IDA** (`D:\ghidra_proj2\ER`, imagebase 0x140000000) + a debugger that
attaches to the running game (Cheat Engine + x64dbg). Repo: **ERR-MapForGoblins-DLL** (Elden Ring world-map
mod). This is **candidate 1** of `windows_native_map_render_toggle_re_findings.md` — the ONE remaining lever
to stop the native world map from rendering while keeping its logic alive.

## Why (what already works, what this finishes)
MFG already replaces the native map with a fullscreen ImGui **Virtual World Map** (vmap) that opens on the
game map key (`vmap_on_map_key`) and OWNS all input (native map pan/zoom/click fully locked out — cursor +
raw-input + wndproc hooks). The vmap draws **opaque over** the still-open native map. The last brick: the
native `WorldMapDialog` still **renders** underneath (wasted GPU + a peek risk if the cover ever isn't
perfect). We want a **render-only** off switch: skip the native map's draw, keep its update/logic (fast-travel
resolution, page/cursor/fog state) so nothing in the state machine breaks.

## DO NOT re-try these — proven DEAD live (findings §4c/§4d, 2026-07-05 Linux/Proton)
- **D3D12 `RSSetScissorRects`** (candidate 2): the Scaleform map rasterizes full-screen through GENERIC engine
  scissors — there is NO map-specific scissor rect (recon: 30 rects, none mapopen=1-only; the one mapopen=0
  rect = the minimap, so tagging is sound). Emptying the shared rect blanks the WHOLE frame. Dead.
- **`MovieImpl+0xB0` GFx clip write** (the §4c pivot): the two-deref resolve `WorldMapDialog+0x140 → *(+0x00)=
  MovieImpl`, `+0xB0` = int (L,T,W,H) is CORRECT (read-back `(0,0,1920,1080)`, `buf==1920×1080`), but forcing
  it `(0,0,0,0)` HOLDS yet the map + markers render unchanged. `+0xB0` is a **descriptive viewport**, not a
  render-enable. Dead. (The `movieclip read|hide|show` RPC + `worldmap_probe::movieclip_maintain()` scaffolding
  is KEPT — reuse its resolve chain + per-present maintain for whatever this task lands on.)

## The target (what IS the map, from §1–§4)
- Native map = **`CS::WorldMapDialog`** — a GFx/Scaleform **CSMenu** dialog. vtable **er+0x2b2d7d8**, size
  **0x3ed0**, ctor `FUN_1409cf8f0` (er+0x9cf8f0). Base = `CS::WorldMapDialogBase` (`FUN_1409be5e0`) →
  **`CS::MenuWindow`** (CSMenu base ctor `FUN_140741960` er+0x741960: DLReferenceCountObject → SceneObjModifier
  → MenuWindow). ViewModel vtable er+0x2ad82e0 (pinned `WORLDMAP_VIEWMODEL_VTABLE_RVA`).
- **Update ≠ draw.** The menu UPDATE (logic) tick is `FUN_140766980` (er+0x766980, `CSMenuManImp`
  `CSEzUpdateTask` execute). **Leave it running.** §4 established the per-menu **DRAW is a SEPARATE Scaleform
  render pass** (movies rasterize during present, not in `FUN_140766980`) — that pass is what this task must find.
- The movie is played by a **`CSScaleformSwfPlayer`** reachable at **`movieHandle+0x58`** (movieHandle is a
  pointer at `WorldMapDialog+0x140`; MovieImpl is `*(movieHandle+0x00)`).

## Goal — find the draw submit + a no-op point (either of two)
**(A) The per-menu DRAW vfunc slot (cleanest).** Find where the menu system iterates active menus and calls
each one's **draw** vfunc during the render/present pass (NOT the `FUN_140766980` logic tick). Sub-steps:
1. From `CSMenuManImp`/`CSFeManImp` (ctors 0x7650a0 / 0x76b9d0; slots `CSMENUMAN_SLOT`/`CSFEMAN_SLOT`), find
   the DRAW-side traversal — distinct from the `CSEzUpdateTask` update. Look for a second task/callback
   registered by these managers whose body walks the active-menu list and calls a vfunc at a fixed slot on
   each `CS::MenuWindow`. (The render task likely runs off the graphics/present tick, not the sim update task.)
2. Read the `CS::MenuWindow` (or `CSMenu`) vtable layout: identify the **draw/render vfunc slot index**
   (the one that submits the GFx movie — cross-check it reaches `CSScaleformSwfPlayer::…Render/Display` via
   `movieHandle+0x58`). Report the slot **offset** (e.g. vtable+0xNN) and the callee.
3. Hook strategy for MFG: at that draw call site (or by patching the `WorldMapDialog` vtable slot — identify
   the dialog by vtable **er+0x2b2d7d8**), when the mod flag is on, **skip the submit / return early**. Keep
   `FUN_140766980` untouched.

**(B) Fallback — a movie/player render-ENABLE flag.** If the draw slot is too deep for this session: on the
`CSScaleformSwfPlayer` (`movieHandle+0x58`) or `MovieImpl`, find a **boolean/visible/enable** field the render
pass CONSULTS (unlike the descriptive `+0xB0`). Candidates: a `bVisible`/`bRender`/advance-flag on the player
or the GFx movie root (`GASPMovieRoot::SetVisible`-style). Deliver its offset + observed on/off effect. (A
blind RPM field-scan is Linux-doable via the `movieclip` scaffolding, but risky — flipping unknown fields can
crash; prefer a Ghidra-guided offset from the render pass reading the field.)

## Deliverable → `docs/re/windows_native_map_drawvfunc_re_findings.md`
- The draw submit site: the menu draw vfunc **slot** on `CS::MenuWindow`/`CSMenu` (offset + callee), OR the
  central render-loop call that submits each menu's movie + how to identify the `WorldMapDialog` entry
  (vtable er+0x2b2d7d8). If (B): the render-enable field offset on the player/movie.
- The exact **hook point** MFG should take (MFG already detours Present + hooks a bunch via MinHook; a vtable
  slot swap or a call-site detour both fit) + the on/off condition, keeping the update tick live.
- Any guardrail: confirm skipping ONLY the draw does not desync the movie (the update advances it; the render
  just doesn't submit). Note if the movie must still be advanced/displayed to a null target vs simply skipped.

## Validation (hand back to Linux — RPC + screenshot)
Wire it behind a dev flag/RPC verb (like the existing `movieclip`). On Linux/Proton: open the game map →
native map **pixels gone**, vmap still drawn, **fast-travel + page/cursor/fog logic intact**, map **closes
normally** (no freeze). Compare `[BENCH]` present cost with the native draw skipped.

## Guardrails (from §3, §5)
- **Render-only.** Do NOT suppress the menu OPEN/close or the UPDATE tick (`FUN_140766980`) — that risks the
  known vmap map-context freeze and breaks fast-travel/page/fog.
- Production flip is gated on vmap Track A parity + Track B fast-travel anyway (design docs); this task lands
  the LEVER + a dev flag so it can be A/B'd now and flipped last.

## Anchors (findings §6)
```
WorldMapDialog     vtable er+0x2b2d7d8  size 0x3ed0  ctor FUN_1409cf8f0 (er+0x9cf8f0)
  base             WorldMapDialogBase   FUN_1409be5e0 (er+0x9be5e0);  CSMenu base ctor FUN_140741960 (er+0x741960)
  menu base class  CS::MenuWindow       (DLReferenceCountObject -> SceneObjModifier -> MenuWindow)
  view model       er+0x2ad82e0  (WORLDMAP_VIEWMODEL_VTABLE_RVA)
menu managers      CSMenuManImp ctor 0x7650a0 / CSFeManImp ctor 0x76b9d0   [CSMENUMAN_SLOT / CSFEMAN_SLOT]
update tick (LOGIC)FUN_140766980 (er+0x766980)  — CSEzUpdateTask execute; LEAVE RUNNING (not the draw)
movie chain        WorldMapDialog+0x140 -> *(+0x00)=MovieImpl ; player @ movieHandle+0x58 (CSScaleformSwfPlayer)
DEAD levers        D3D12 scissor (no map rect) ; MovieImpl+0xB0 clip write (descriptive, inert)
```
Cross-ref: `windows_native_map_render_toggle_re_findings.md` (§4 the draw-is-a-separate-pass result),
`worldmap_native_clip_b3_scaleform_re_findings.md` (the +0xB0 clip resolve, reused), `imgui_only_map_plan.md`
(Track C), `single_surface_ui_plan.md`, `virtual_world_multi_world_design.md` (endgame phase 3).
