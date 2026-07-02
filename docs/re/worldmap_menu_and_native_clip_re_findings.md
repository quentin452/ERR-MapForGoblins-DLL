# Worldmap z-order RE — findings

Sibling to `worldmap_menu_and_native_clip_re_prompt.md`. Records what was solved.

## TASK A — "menu covers map" flag — SOLVED (2026-07-03, Windows, live in-DLL byte-diff)

**Result: `CSMenuMan + 0x104` (u8) = 1 exactly while a submenu covers the OPEN world map, 0 on
the bare map.** Consumed by `goblin::worldmap_probe::menu_covers_map()`; `render_markers` skips the
whole worldmap marker pass while it reads 1 (gated by ini `clip_game_ui`).

### Method
Live byte-diff via the in-DLL probe + RPC driver (the repo's proven runtime-RE loop), on Windows
against the live ERR install:
- Widened `dump_menu_state` (RPC `dumpmenu`) to cover CSMenuMan `+0x0..0x400`.
- Added `menu_open_diag` behind ini `[Debug] debug_menu_cover_diag`: while the map is open it
  delta-scans the CSMenuMan head AND the WorldMapDialog head and logs any byte that changes.
  - **Gotcha that cost the first two runs:** the first version copied `region_diag` — 4-byte stride
    + filter the int32 to `[0,256)`. A menu flag is a single BYTE; a byte flipping at a non-4-aligned
    offset (e.g. `+0xCD`) makes the enclosing int32 jump by `n<<8` and gets filtered out → **zero
    logs**. Fix: scan **byte-wise** (a byte is always `[0,255]`, alignment-agnostic). This is the
    general lesson for any menu/flag byte hunt in this codebase.

### Evidence (wmprobe log, `[MENUOPEN-DIAG]`, one session)
Covering menu used: the **fast-travel confirm dialog** (select a grace → "Vous allez voyager vers…"
OK/Cancel modal over the map).
- Bare map opened at 00:13:14, then ~16 s of panning: **`mm+0x104` never changed (stayed 0)** →
  zero false-positives during bare-map pan/zoom.
- Then **4 cover/uncover cycles**: `mm+0x104` toggled `0→1 … 1→0` cleanly, 8 flips, ending 0.
- Only ONE `resolve: cursor` / `PUBLISH` in the whole session and no teardown/re-resolve → the
  WorldMapDialog **persisted** through all cycles → this was a menu-over-map, not a map reopen.

Corroborating fields (same cycles, less clean): `mm+0x01c` (0↔4), `dlg+0x098` (1↔2). `mm+0x104`
wins — a straight 0/1 boolean.

### Note on the doc's "dead end"
The prompt marked `CSMenuMan+0xCD` DEAD ("map screen up, not child-covers-it"). On THIS build `+0xCD`
*did* leave 7 (→3) while covered, so it isn't as dead as thought — but `+0x104` is still the better
signal: a direct boolean, no magic `==7`, and (unlike `+0xCD` which is `world_map_open()`'s byte) it
reads 0 on the bare open map, so it can't be confused with "map is up".

### Implementation (committed)
- `constexpr CSMENUMAN_MENU_COVER_OFF = 0x104` + `bool menu_covers_map()` in
  `goblin_worldmap_probe.cpp` (mirror `world_map_open()`: resolve slot once, SEH-read the byte, log
  distinct values as `[MENUCOVER]`). Declared `GOBLIN_RENDER_API` in the header (render module imports
  it like `project()`).
- RPC `status` gained `menucover=` for regression checks.
- `render_markers` (`map_renderer.cpp`) early-outs the whole worldmap pass when
  `clip_game_ui && menu_covers_map()` — right after the map-closed `get_live_view` early-return, before
  any `PushClipRect`. No view-delay reset (map still open underneath → resumes smoothly on close).

### Validation status — DONE (2026-07-03, live)
- ≥4 cycles + zero bare-map false-positives: **met** (fast-travel confirm dialog).
- ≥2 distinct covering menus: **CONFIRMED live** — `menucover=` flips `0→1` for multiple covering
  menus (not just the warp prompt), and markers visibly disappear under the covering menu. `+0x104`
  is a CSMenuMan-level field, so it behaves as the generic "a dialog is stacked over the current
  screen" flag as expected. **Task A complete.**

## TASK B — clip to ER's native map clip rect — IN PROGRESS (B1 instrument built 2026-07-03)

Current manual clipping (what B replaces), all in `map_renderer.cpp`:
1. **Canvas clip** (`s_canvas_min/max`, ~L1815) — the engine's static full-map rect `view+0x350`
   (map-ART extent, MARKER space) projected through OUR delayed view. Kills icons on the void past
   the map edge, but it's content-extent not the screen viewport, rides our affine + delayed view.
2. **Static ERR dial exclusion** (`in_game_ui_exclusion`, ~L1036) — hardcoded disc (1815,1000) r240
   + time pill, ERR-only magic numbers. Not mod-agnostic.
3. **User-drawn exclusion rects** (`ui_exclusion_rects`) — manual per-install rectangles.

**Goal:** ER's own screen-space map clip/scissor → pixel-identical native clip, retires 2+3.

**B1 — struct rect-dump (BUILT, not yet run):** `map_clip_diag(bbW,bbH)` (probe), gated by ini
`[Debug] debug_map_clip_diag`, called from `hk_present` while the map is open. Dumps every plausible
f32 rect (finite, `(0,8192]`) from the candidate live structs — the virtual UI canvas singleton
(`[slot]+0x128`, +0x100..0x1C0), the WorldMapDialog (+0xA00.., +0x2B60..), the WorldMapArea view
(+0x300..0x400), and the WorldMapViewModel (+0x0..0x120) — as `[MAPCLIP]`, ONE-SHOT per backbuffer
resolution. **Run recipe:** open the map, then change the game resolution in-game → two `[MAPCLIP]`
dumps; a value that SCALES with the backbuffer is the native screen scissor, a constant is
virtual-canvas (1920×1080) or marker space. Known non-answers: `view+0x340..0x34c` (cursor/snap
bounds), `+0x350` (map-art extent) — both marker space.

**B1 RESULT (run 2026-07-03) — no screen-space map rect on the scanned structs; escalated:**
- Dumped clean at 1920×1080: `canvas+0x118/11c = 1920/1080` (the FULL virtual canvas, no map
  sub-rect), `view+0x340..0x34c = [-267,-152,1652,927]` (the documented marker-space cursor/snap
  bounds — 16:9-looking but NOT screen space), `view+0x320` a small quad. No obvious screen-space
  map-viewport sub-rect.
- **Key architectural finding:** changing the in-game resolution to 1280×720 did NOT change the DXGI
  swapchain backbuffer (stayed 1920×1080 → `map_clip_diag` never re-dumped). ER renders the map UI
  into a fixed backbuffer = the window/desktop size and up/downscales the internal 3D target. So the
  map UI clip lives in **virtual-canvas (1920×1080) space**, and the "scales with backbuffer"
  discriminator needs a real window/desktop resize, not the in-game slider. B1's null result +
  fixed-canvas architecture ⇒ the native clip is likely an **inline D3D12 scissor** set at draw time,
  not a rect parked on a struct → escalate to B2.

**B2 — command-list scissor/viewport sampler — DEAD END (run 2026-07-03), pivoted to B3.**
`debug_scissor_probe` hooks `RSSetScissorRects` (vtable slot 22) + `RSSetViewports` (slot 21) on
`ID3D12GraphicsCommandList` from the first submitted list, tags each rect `mapopen=0/1`, dedups, logs
`[SCISSOR]`/`[VIEWPORT]`. What we learned running it:
- The hooked addresses live in **`D3D12Core.dll`** — ER ships the **Agility SDK** (two D3D12 runtimes:
  OS `d3d12.dll` stub + Agility `D3D12Core.dll` doing the real work).
- **MinHook-on-function did NOT redirect** these D3D12Core methods (install returned `MH_OK`, but the
  detour never fired — not even `RSSetViewports`, which runs every frame).
- Switched to a **direct vtable-swap** (canonical D3D12 method hook: overwrite the slot pointer the
  engine reads) — the swap write succeeded (`vt=0x7fff937ba850`), but the detour **still never fired**,
  including for viewports.
- Conclusion: the engine's per-frame render calls don't read the vtable slot we swapped → either it
  records scene/UI command lists with a **different vtable** than the `lists[0]` we sampled, or it
  **pre-records command lists and re-submits** them (so scissor/viewport is set once, before our hook).
  A "swap every distinct vtable + log distinct vtables" pass would disambiguate, but per the user's
  call we **pivot to B3** rather than drill further into the command-list mechanism.

The B2 probe (`debug_scissor_probe`, `note_map_scissor`/`note_map_viewport`, the two detours +
vtable-swap in `hk_execute_command_lists`) stays committed as documented scaffolding — OFF by default,
non-firing; do not re-attempt the MinHook path.

**B3 — Ghidra static RE of the Scaleform map-movie clip — ACTIVE.** See
`worldmap_native_clip_b3_scaleform_re_prompt.md`.
