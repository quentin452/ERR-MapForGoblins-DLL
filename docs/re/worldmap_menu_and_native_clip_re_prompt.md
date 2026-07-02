# Worldmap z-order RE — (A) menu-over-map covering flag, (B) clip to ER's own native map clip rect

Two related RE tasks that both close the same class of overlay bug: **we render post-present, so our
worldmap markers draw OVER everything the game puts on top of the map** — submenus that open over the
map (Task A) and the map's own letterbox/clip edges (Task B). The static ERR day/night-dial exclusion
(`fix/f2-fog-locate-v2`, in-game validated) and the user-drawn UI-exclusion-zone editor are the
current STOPGAPS; both are hand-authored screen regions, not the game's real clip. These tasks replace
them with a game-state signal (A) and the engine's own clip rect (B).

Read first: `docs/HANDOFF.md` §"Overlay z-order clipping … dial DONE, menu-over-map OPEN" and
§"Overlay polish batch" item 1 ("REAL map clipping"). Probe template + all helpers:
`src/goblin_worldmap_probe.cpp` (`[REGION-DIAG]`, `[INPUT-DELTA]`, `dump_menu_state`, `seh_read*`,
`resolve_cursor_via_menu`, `find_view_model`). RPC driver: `tools/mfg_rpc.py`. Consumer for BOTH:
`src/worldmap/map_renderer.cpp` — the worldmap pass `PushClipRect(s_canvas_min, s_canvas_max)` (~L1846)
and the `in_draw_bounds` / `in_game_ui_exclusion` cull predicate (~L1042-1073).

Style note: this repo proved Group 2 RE and the whole projection stack end-to-end on Linux via in-DLL
probes under Proton (see `docs/re/linux_group2_prompt_binding_re_findings.md` for the tone). Prefer the
in-DLL probe + RPC-driver loop; escalate to Windows/Ghidra only where a live struct can't reach it.

---

## TASK A — "menu is covering the map" flag  → LIKELY DOABLE ON LINUX (in-DLL byte-diff + RPC)

**Objective.** Find a single game-state bool/byte that is **1 exactly while a submenu is open OVER the
open world map** (the "Z" map-menu, the beacon/marker-placement dialog, region-list, etc.) and **0 when
the bare map is showing** (pan/zoom, no child menu). Consumer: while that flag is up, SKIP the entire
worldmap marker pass so our icons don't punch through the covering menu.

**Platform call: Linux first.** This is a live-state discovery, not static decompilation — the exact
byte-diff / RPC-toggle method that already works in this repo. No Ghidra expected. Escalate to Windows
only if no stable field is found in the CSMenuMan / WorldMapDialog windows below.

### Prior art / known dead ends
- `CSMenuMan+0xCD` is the per-screen menu-state byte used by `goblin::world_map_open()`
  (`src/goblin_tutorial_popup.cpp` L336; `==7` ⇒ world-map screen up). **On this build it does NOT
  distinguish a submenu-over-map from the bare map** — treat +0xCD as DEAD for this purpose. It answers
  "map screen is up", not "a child dialog covers it".
- The probe already exposes exactly the right tool: RPC `dumpmenu <tag>` → `dump_menu_state()`
  (`goblin_worldmap_probe.cpp` L1079). It hexdumps **CSMenuMan +0x0..0x200**, and the WorldMapDialog
  (`cursor-0x2DB0`) windows **+0xA00..0xB40** and **+0x2B60..0x2C40** to the wmprobe log. That is the
  byte-diff harness — this task is mostly "drive it with two states and diff".
- `[REGION-DIAG]` (L252) already delta-scans dialog `+0..0x600` and view `+0..0x400` for small ints that
  flip on map-region switches — the SAME pattern extended to the menu-open transition is the scan.

### Probe recipe (in-DLL + RPC)
1. **Widen `dump_menu_state`** if needed so both diff points are covered in one call: keep the CSMenuMan
   `+0x0..0x200` window (the submenu-stack head almost certainly lives here — a menu manager keeps its
   active-child list / top-of-stack pointer + a depth/count near the head), and keep the two dialog
   windows. Consider adding a `+0x200..0x400` CSMenuMan window and a small-int delta pass (reuse
   `[REGION-DIAG]`'s int32 filter: log offsets whose value changes AND stays in `[0,256)`).
2. **Script the two states with the RPC driver** (game already running under Proton, ini
   `[Debug] debug_rpc_port = 38700` is set in the deployed dev ini):
   ```
   # from a loaded save, get the bare map up (this install binds the map to M, not vanilla G):
   ./tools/mfg_rpc.py --port 38700 key M            # open world map
   ./tools/mfg_rpc.py --port 38700 status           # expect map_open=1
   ./tools/mfg_rpc.py --port 38700 dumpmenu bare     # STATE 0: bare map, no child menu
   # now open the submenu that covers the map. Identify its bind live (see step 3); e.g.:
   ./tools/mfg_rpc.py --port 38700 key <mapmenu_key> # open the "Z" map-menu / beacon dialog
   ./tools/mfg_rpc.py --port 38700 dumpmenu covered  # STATE 1: submenu OVER the map
   ./tools/mfg_rpc.py --port 38700 key <mapmenu_key> # close it
   ./tools/mfg_rpc.py --port 38700 dumpmenu bare2     # STATE 0 again (confirm it returns)
   ```
   `key` is closed-loop (kbseen-verified, auto-refocus+resend on loss) so the presses land; keep the
   game window focused during the run (`g_has_focus` gate — see HANDOFF Phase-4 background blocker).
3. **Discover the covering-submenu bind first** if unknown: with the map open, `dumpmenu` before/after
   each candidate input, or add a transient `[MENUOPEN]` log when ANY CSMenuMan head field changes while
   `world_map_open()` is true. The "Z" name is the vanilla map-menu key; on this AZERTY/rebound install
   the actual key must be read live (same lesson as map=M). A gamepad button may be the real opener —
   if keyboard can't reach it, drive the pad via the game's own input or note it for a manual repro.
4. **Diff the two logged states.** `grep '\[MENUDUMP:' MapForGoblins_wmprobe.log`, line up `bare` vs
   `covered`. A byte/int that is one value in `bare`, a different STABLE value in `covered`, and RETURNS
   in `bare2` is the candidate. Prime suspects: a **submenu-count / stack-depth** near the CSMenuMan head
   (0 with only the map, ≥1 with a child), or an **active-child pointer** (null vs non-null). Prefer a
   count/depth over a pointer (a pointer needs a null-vs-live test; a depth is directly the flag).
5. **Confirm across cycles.** Repeat open/close ≥4× and for MORE than one covering menu (map-menu AND
   beacon/marker dialog if they differ). The winning field must flip 0→1→0 every time, for every
   covering menu, and stay 0 while merely panning/zooming the bare map.

### Success criteria
- A stable offset (CSMenuMan+X or WorldMapDialog+X) that reads **1 (or ≥1 / non-null) exactly when a
  submenu covers the open map, 0 otherwise**, reproduced across ≥4 open/close cycles and ≥2 distinct
  covering menus, with **zero** false-positives during bare-map pan/zoom.
- Expose it as `goblin::worldmap_probe::menu_covers_map()` (mirror `world_map_open()`: resolve the slot
  once, SEH/RPM-read the byte, log distinct values). Add `menucover=` to the RPC `status` line for
  regression checks.

### Consumer code site
- `src/worldmap/map_renderer.cpp`, worldmap pass: **early-out the entire marker pass when
  `menu_covers_map()` is true** — the cleanest place is right before the `PushClipRect` block (~L1846)
  / at the top of the worldmap draw, so markers, piles, labels, fans, region chips AND hover all go
  silent together (same "hover dies with the pixels" invariant the canvas clip already keeps). The
  minimap pass is unaffected (a covering menu is a worldmap-screen event). Gate behind the existing
  `clip_game_ui` ini so it can be toggled off for debugging.

---

## TASK B — clip our overlay to ER's OWN native map clip rect  → PLATFORM UNSURE (Linux attempt → Windows/Ghidra fallback)

**Objective.** Instead of stacking hand-drawn exclusion zones, clip our post-present worldmap overlay to
the **same rectangle/scissor the game uses to clip its own map/minimap UI layers** — the map viewport
scissor, or the Scaleform/GFx clip on the world-map movie — so our overlay clips IDENTICALLY to the
native map art ("pour que ce soit parfait"): no icons in the letterbox void, under the dial, or past a
partially-scrolled map edge, matching the game pixel-for-pixel at every resolution/zoom.

**Platform call: split the task.**
- **B1 (try on Linux first):** the clip may already be reachable from a LIVE struct we hold — no Ghidra.
- **B2 (Windows/Ghidra fallback):** if the clip lives only inside the GFx/Scaleform draw path and isn't
  parked on any struct we can reach at runtime, escalate to static RE on the Windows box (the Ghidra
  project + scripts live there; Scaleform clip on the map movie is a decompilation job).

### Prior art / seed
- **The seed already exists:** `LiveView.mapMinU/mapMinV/mapMaxU/mapMaxV` = the WorldMapArea **static
  full-map rect** at `view+0x350..0x35c` (`[minX,minZ,maxX,maxZ]`, marker space). The worldmap pass
  ALREADY projects it and `PushClipRect`s to it (`map_renderer.cpp` ~L1832-1846, branch
  `refactor/marker-gates-clip-labels`, in-game validated — killed the "icons on the black void" bug).
  So we clip to the **map ART extent** today. What's MISSING is the **screen viewport/scissor** the game
  clips its UI draw to — the +0x350 rect is content bounds in marker space, projected through OUR view;
  the native clip is a screen-space rect the engine sets around the map layer, which is what actually
  hides overspill under the dial / at the frame edge / behind side panels.
- `view+0x340..0x34c` is the CURSOR/snap bounds rect (already read for locate clamp in
  `set_view_center`), distinct from +0x350. Neither is the screen scissor.
- Existing viewport/canvas leads to chase from a live struct: the **virtual UI canvas singleton**
  (`CANVAS_SINGLETON_RVA` → `+0x128` → `+0x110 originX/+0x114 originY/+0x118 w/+0x11c h`, dumped as
  `[CANVAS]` in the probe, expect 1920×1080) and the WorldMapDialog itself (already walked +0xA00.. and
  +0x2B60.. in `dump_menu_state`). The map-view struct reached via `find_view_model()` (the
  WorldMapViewModel) is another candidate root.

### B1 — Linux runtime attempt (in-DLL, no Ghidra)
1. **Dump the map-view / dialog for any rect-shaped float quad** that tracks the on-screen map area.
   Extend the probe's one-shot window dump (the `bounds_dumped` block, `goblin_worldmap_probe.cpp`
   ~L891) over the WorldMapArea (`view`), the WorldMapDialog, and the WorldMapViewModel: log every f32
   that looks like a screen rect (values in `[0, screenW]×[0, screenH]`, or `[0,1]` normalized, forming a
   `{x,y,w,h}` or `{minX,minY,maxX,maxY}` quad). Change the resolution mid-session (the game supports
   it — see `windows_midsession_resolution_swapchain_re_findings.md`) and re-dump: the **native scissor
   scales with the backbuffer**, a marker-space rect does not — that delta identifies it.
2. **Hunt a D3D12/RS scissor the engine sets around the map draw.** If a screen rect isn't parked on a
   struct, the clip may only exist as an `RSSetScissorRects` / `RSSetViewports` call in the map layer's
   draw. From our present hook we can observe the command context; a targeted probe can record the
   scissor rect active during the map UI pass (the frame region where the map movie draws). This is the
   boundary of "reachable live" — if the scissor is computed inline and never stored, B1 fails → B2.
3. **Validate against ground truth.** Overlay the candidate rect as a debug outline (like the existing
   locate-debug overlay) and eyeball it against the map art edge / under the dial at several
   resolutions and zoom levels, map scrolled to each edge.

### B2 — Windows / Ghidra fallback (static RE)
- If the clip isn't reachable from a live struct, statically RE the **Scaleform/GFx world-map movie
  clip**: ER renders menus via CSScaleform (the world map is `menu/…/02_120_worldmap.gfx`). Find where
  the map movie's clip/mask rect is set — the GFx `SetViewport` / clip-rectangle path for the map layer,
  or the CSScaleform draw that binds the map movie's scissor. Deliverable: the RVA + struct offset (or
  the formula from canvas size) that yields the screen-space map clip rect, so the runtime side can read
  or reconstruct it. Cross-reference the existing menu/Scaleform RE notes under `docs/re/` before
  starting (`windows_csworldmapmenu_re_prompt.md`, the page-transition findings).
- Keep it mod-agnostic (prime directive): the result must be a rect the ENGINE computes for whatever
  movie/resolution is loaded, not an ERR-specific constant.

### Success criteria
- A screen-space rect (per frame, resolution- and zoom-correct) equal to the game's own map-UI clip,
  sourced from a live struct (B1) or reconstructed from an RE'd formula (B2). Feeding it into the
  worldmap pass makes our overlay clip pixel-identically to the native map — verified by the debug
  outline sitting exactly on the map art boundary at ≥3 resolutions and both map pages.

### Consumer code site
- `src/worldmap/map_renderer.cpp`: **replace the source of `s_canvas_min/s_canvas_max`** (currently the
  projected +0x350 marker-space rect, ~L1832-1846) with the native screen clip rect (intersect the two
  if both are wanted — content extent AND viewport). `in_draw_bounds` already consumes
  `s_canvas_min/max` via `s_canvas_clip`, and `PushClipRect` already applies it, so once the rect is the
  native one the cull + hover + pixels all follow for free. **Retire `in_game_ui_exclusion`** (the
  static dial disc + user-drawn zones) once B lands — those were the stopgap this task removes; keep the
  ini `clip_game_ui` gate.

---

## Sequencing
Do **A first** (self-contained, Linux-only, high confidence — it's a byte-diff run that reuses the
existing `dumpmenu` harness) and **B1 second** (Linux runtime attempt off the existing +0x350 seed).
Only escalate to **B2** (Windows/Ghidra) if B1 proves the native scissor isn't reachable from a live
struct. A and B are independent: A hides the whole pass under covering menus; B tightens the pass's clip
to the map's own edges. Together they retire both stopgaps (the static dial exclusion and the manual
UI-exclusion zones).

_This is a prompt/plan doc — no C++ changes here. Write findings to a sibling
`*_re_findings.md` (and update `docs/HANDOFF.md`) when solved._
