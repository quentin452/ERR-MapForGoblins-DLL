# Task B3 — RE the native world-map CLIP rect via the Scaleform map movie (Windows, Ghidra)

You are on **Windows** with **Ghidra** (the project + scripts already live on this box) and a
**debugger** that attaches to the running game (Cheat Engine / x64dbg). Repo:
**ERR-MapForGoblins-DLL**. This is **Task B3** of the overlay z-order work — read
`worldmap_menu_and_native_clip_re_prompt.md` (the plan) and
`worldmap_menu_and_native_clip_re_findings.md` (what's solved/ruled out) first.

## Goal

Find the **screen-space rectangle the engine clips the world-map draw to** — the map viewport /
Scaleform movie clip — and deliver a way to obtain it at runtime: an **RVA + struct offset** on a
live object we can reach, **or** a **formula from the virtual-canvas size**. The overlay renders
post-present; feeding it this rect makes our markers clip **pixel-identically to the native map**
(no icons in the letterbox void, under the ERR dial, or past a partially-scrolled edge), retiring
the hand-authored dial exclusion + user-drawn zones (`in_game_ui_exclusion`,
`ui_exclusion_rects` in `src/worldmap/map_renderer.cpp`). Keep it **mod-agnostic**: the result must
be a rect the ENGINE computes for whatever movie/resolution is loaded, never an ERR constant.

## What is already RULED OUT (don't repeat)

- **B1 (live struct rect-dump), done.** Dumped every plausible f32 rect from the virtual UI canvas
  singleton, the WorldMapDialog, the WorldMapArea view, and the WorldMapViewModel. **No screen-space
  map-viewport sub-rect is parked on any of them** — only the *full* canvas (`canvas+0x118/+0x11c =
  1920/1080`) and marker-space rects (`view+0x340..0x34c` cursor/snap bounds, `view+0x350` map-art
  extent). So the clip is **not** a simple field on the structs we already hold.
- **Architecture fact:** ER draws the map UI into a **fixed 1920×1080 virtual canvas** and up/downscales
  to the backbuffer; the in-game resolution slider does NOT change the DXGI backbuffer. So the native
  clip is expressed in **virtual-canvas (1920×1080) units** (constant across the in-game res setting),
  applied via a canvas→backbuffer scale at draw. A live-readable clip is therefore likely a
  **canvas-space sub-rect** (or the whole canvas) on the Scaleform movie/renderer, not a backbuffer rect.
- **B2 (D3D12 scissor/viewport command-list hook), dead end.** ER ships the **Agility SDK**
  (`D3D12Core.dll`). Hooking `RSSetScissorRects`/`RSSetViewports` (vtable slots 22/21) on the first
  submitted `ID3D12GraphicsCommandList` — via MinHook AND via a direct vtable-swap — **never fired**,
  not even for viewports (which run every frame). The engine's per-frame render calls don't read the
  vtable slot we could reach. So the clip is set deep in the **GFx/Scaleform renderer's own D3D12
  backend**, on command lists we can't cheaply intercept from the present/ECL hook — hence this static
  RE. (Scaffolding kept OFF under `debug_scissor_probe`; do not re-attempt the command-list hook.)

## Where to look (anchors)

Analyse the **MSVC `.text` at VA `0x140001000`**, not the VMProtect `.text` at `0x144c0e000`.
Cleartext RTTI / scope strings are the anchors.

- **Scaleform render layer.** `Scaleform::Render::*` RTTI strings. Prior finding: worldmap icons are
  **Scaleform display-list objects** positioned by the GFx movie via `Scaleform::Render::Matrix2x4<float>`
  (see `marker_affine_hook_re_findings.md`, `marker_to_mapspace_re_findings.md`). The map's clip/viewport
  is set in the **GFx renderer's per-movie draw** — hunt `Scaleform::Render::Viewport`,
  a `SetViewport`/`Display`/`Draw` on the movie/renderer, or a clip/mask rect on the movie view.
- **CSScaleform (ER wrapper) + the world-map movie.** ER renders menus via `CS::CSScaleform`; the world
  map is `menu/…/02_120_worldmap.gfx`. Find where the map movie's **GFxMovieView viewport / stage clip
  rect** is set each frame — that computation (canvas size → movie rect) IS the clip source. Look for
  the movie-name string / the movie-list slot for 02_120_worldmap and walk its draw path.
- **Virtual UI canvas singleton** (already resolved in-DLL): `[eldenring.exe + CANVAS_SINGLETON_RVA]
  → +0x128 → +0x110 originX / +0x114 originY / +0x118 w / +0x11c h` (= 1920×1080, dumped as `[CANVAS]`).
  The map clip is almost certainly **derived from this canvas** (a stage/letterbox rect computed from
  canvas w/h + aspect). Xref the canvas singleton into the Scaleform draw to find the derivation.
- Cross-reference the existing menu/Scaleform RE: `windows_csworldmapmenu_re_prompt.md`
  (`CSWorldMapMenu`/`CSWorldMapPointManImplement`), `marker_affine_hook_re_findings.md`,
  `windows_worldmap_page_transition_re_findings.md`.

## Deliverable

A `*_findings.md` sibling with:
- The **RVA** of the routine that computes/sets the map movie's viewport/clip, and either
  - a **live-readable struct offset** (on the GFxMovieView / CSScaleform movie object / a renderer
    struct we can reach from the active cursor→dialog chain or the canvas singleton) that holds the
    screen/canvas-space clip rect, OR
  - a **formula** = f(canvas w, h) that reconstructs the rect (if it's computed inline, not stored).
- Confidence + runtime evidence (attach, break on the routine, read the rect at a couple of
  resolutions/aspect ratios; confirm it matches the visible map art edge). Note the **units**
  (canvas 1920×1080 vs backbuffer px) explicitly — the consumer applies the same canvas→screen scale
  the overlay already uses (`realW/1920`, `realH/1080`).
- Mark dead paths so they aren't retried.

## Consumer (once found)

`src/worldmap/map_renderer.cpp`: replace the source of `s_canvas_min/s_canvas_max` (currently the
projected `+0x350` marker-space extent, ~L1815) with the native clip (intersect the two if both are
wanted — content extent AND viewport). `in_draw_bounds` + the `PushClipRect` already consume
`s_canvas_min/max`, so cull + hover + pixels follow for free. Then retire `in_game_ui_exclusion`
(the static dial disc + user rects); keep the `clip_game_ui` ini gate.

_This is a prompt/plan doc — no C++ here. Write findings to
`worldmap_native_clip_b3_scaleform_re_findings.md` and update `docs/HANDOFF.md` when solved._
