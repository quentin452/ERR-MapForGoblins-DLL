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

### Validation status
- ≥4 cycles + zero bare-map false-positives: **met** (fast-travel confirm dialog).
- ≥2 distinct covering menus: **PENDING live re-check** via the new `menucover=` status field (confirm
  it also flips for the marker-placement dialog / region list, not just the warp prompt). `+0x104` is a
  CSMenuMan-level field (not on the WorldMapDialog), so it is expected to be a generic "a dialog is
  stacked over the current screen" flag.

## TASK B — clip to ER's native map clip rect — NOT STARTED
See the prompt doc. Sequenced after A.
