# Task B3 — native world-map clip via the Scaleform movie — SOLVED LIVE (2026-07-03, RPM)

Sibling to `worldmap_native_clip_b3_scaleform_re_prompt.md`. The prompt budgeted this as **static
Ghidra RE**; it was instead solved **live via ReadProcessMemory** against the running game (already
sitting in the world-map menu) in one session — no disassembly. This records the answer AND an
important correction to the plan's premise.

## Method (why live beat Ghidra here)

External RPM scanner (`scratchpad/GfxScan.cs`, `OpenProcess(VM_READ|QUERY)` + `ReadProcessMemory`),
driven from PowerShell against live `eldenring.exe`:

1. **RTTI scan of the MSVC image** for `.?A…@Scaleform@@` / `@CS@@` type descriptors → complete
   object locators → vftables. 83 Scaleform/CSScaleform vftables recovered, incl.
   `CSScaleformSwfPlayer@CS@@` (vft VA depends on ASLR; resolve by RTTI each boot).
2. **Heap scan** for live objects carrying those vftables → 23 live `CSScaleformSwfPlayer`.
3. **Identified the world-map movie** mod-agnostically by walking each player's pointers for its
   movie-name string: the player at the map is `MovieView "02_120_worldmap.gfx"` — and its MovieDef
   path is `…/addons/MapForGoblins/menu/02_120_worldmap.gfx` (the ERR/MapForGoblins-replaced movie),
   proving the match is by the movie the engine actually loaded, not a baked constant.
4. **Found the viewport/clip struct** on the movie and a **deterministic pointer chain** to it from a
   struct the probe already resolves (WorldMapDialog), verified by RTTI at every hop.

This is the whole point of the user's question: **live RPM was dramatically more efficient than the
static path** — RTTI→heap→name-match handed us the exact object + offsets that the Ghidra prompt was
going to reverse from the GFx draw code.

## The answer — offsets (all live-verified)

Chain (RTTI-verified at each node), rooted at a struct the probe already reaches:

```
WorldMapDialog (CS::WorldMapDialog)                     ; probe resolves this (Task A dumps its head)
  + 0x140  -> movieHandle                               ; { MovieImpl* @+0x00 ; CSScaleformSwfPlayer* @+0x58 }
      + 0x00  -> MovieImpl  (GFx movie/render impl)
          + 0xA8 : int BufferWidth, BufferHeight        ; = 1920, 1080   (canvas backbuffer size)
          + 0xB0 : int Left, Top, Width, Height         ; = 0, 0, 1920, 1080   <-- THE CLIP RECT
```

- **Clip rect = `MovieImpl + 0xB0`** as `int32 Left, Top, Width, Height` (a Scaleform
  `Render::Viewport`; buffer size precedes it at `+0xA8`).
- **Units: virtual-canvas 1920×1080** (identical to the canvas singleton `+0x118/+0x11c`), NOT
  backbuffer px. The consumer applies the same `realW/1920, realH/1080` scale the overlay already uses.
- Stable across repeated reads; chain re-verified from `WorldMapDialog+0x140` directly (not only via
  the CSMenuMan→CSPopupMenu stack the BFS first found — that stack path also works but the Dialog
  anchor is the robust one since the probe already holds the Dialog).
- `movieHandle+0x58` is the `CSScaleformSwfPlayer` (RTTI-confirmed) — an alternate handle if the
  player is wanted; `SwfPlayer+0x18` also points at the same MovieImpl.
- **Alternate anchors that also reach the same MovieImpl** (if the Dialog is ever unavailable):
  `CANVAS_SINGLETON → … → player`, and `CSScaleform` static singleton (image slot `exe+0x3D83148`
  → `CSScaleformImp@CS@@`) → movie graph. Dialog+0x140 is the shortest/cleanest.

## IMPORTANT CORRECTION to the plan's premise

The plan expected the native clip to remove icons **under the ERR dial** and in the letterbox void,
retiring BOTH the edge cull (#1) and the dial/user exclusions (#2/#3). **The movie viewport is the
FULL canvas `(0,0,1920,1080)` — there is NO engine-computed inset sub-rect.** Scanned the whole
MovieImpl (`+0x0..+0x600`, int and float): the only rect present is the full canvas.

Architecturally that is expected: ER draws the world map **edge-to-edge** and paints the compass
dial + time pill as **HUD on top of the map, inside the same viewport**. So:

- ✅ This rect **retires the edge/void cull (#1)** — it is a clean, stable **screen-space** map
  viewport, replacing the current `s_canvas_min/max` hack (the `+0x350` map-ART extent projected
  through our delayed view, which rides our affine and mis-clips at panned edges).
- ❌ It does **NOT** remove markers under the ERR dial (#2) or the user rects (#3). The dial is a
  **disc** over the map — no engine rectangle represents it, and the map viewport does not carve it
  out. Those must stay as a **separate HUD-overlap layer**, not something the native clip provides.

Net: B3 delivers a correct native **viewport** for #1, but the plan's hope that it also subsumes the
dial exclusion is unfounded — keep `in_game_ui_exclusion` (dial) + `ui_exclusion_rects` as their own
gate.

## One axis left unverified (cheap to close, needs a window resize)

At the current **1920×1080 16:9** window the viewport == full canvas, so the letterbox case isn't yet
demonstrated. B1 already established the in-game res slider does NOT change the backbuffer; a real
**window/desktop resize to a non-16:9 aspect** is the discriminator. Expected: `MovieImpl+0xB0`
becomes a letterboxed inset (pillar/letterbox bars), which is exactly the void #1 wants to cull.
Recipe: resize the game window to e.g. 21:9 or a tall aspect, re-read `MovieImpl+0xB0` — if it goes
inset, #1 is fully covered in screen space. (Not done now to avoid disturbing the live session.)

## Consumer wiring (`src/worldmap/map_renderer.cpp`)

- Resolve `MovieImpl` once when the map opens via `WorldMapDialog + 0x140 → +0x00`; cache; re-resolve
  on reopen (Dialog is already tracked for Task A's `menu_covers_map`). Read `MovieImpl+0xB0` (int
  L,T,W,H) → canvas-space rect.
- Feed it to `s_canvas_min/max` (replacing the projected `+0x350` extent, ~L1815). `in_draw_bounds` +
  `PushClipRect` consume it as today → cull + hover + pixels follow. Scale canvas→screen with the
  existing `realW/1920, realH/1080`.
- **Keep** `in_game_ui_exclusion` (dial disc + time pill) and `ui_exclusion_rects` as the separate
  HUD-overlap layer (see correction above). Keep the `clip_game_ui` ini gate.
- Offsets are build-specific (App 2.6.2.0 / ERR 2.2.9.6). Resolve the SwfPlayer/movie by **RTTI +
  movie-name match** (`02_120_worldmap.gfx`) for patch-robustness rather than hardcoding the vft VA;
  the `+0x140 / +0x58 / +0xA8 / +0xB0` struct offsets are what to pin.

## Dead ends / notes

- Prompt's assumption "native clip removes the dial void" — **wrong** (see correction). The clip is
  the full-canvas map viewport; the dial is HUD-over-map.
- CSScaleform static singleton (`exe+0x3D83148` → `CSScaleformImp`) does NOT directly own the movie
  graph within a short BFS (its movie manager path is long); WorldMapDialog+0x140 is far shorter.
- Scanner + raw scans kept in scratchpad (`GfxScan.cs`, `vtables_scaleform.txt`,
  `heap_scaleform.txt`) — regenerate per boot (ASLR).
