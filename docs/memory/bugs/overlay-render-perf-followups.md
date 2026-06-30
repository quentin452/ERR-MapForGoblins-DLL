---
name: overlay-render-perf-followups
description: "Two perf follow-ups for the overlay world-map marker loop (rebuilt O(n) every frame): frame-coherent cache on camera/mouse change, then a spatial grid."
metadata: 
  node_type: memory
  type: project
---

The overlay world-map marker render (`src/worldmap/map_renderer.cpp`, the `render.worldmap.markers`
bench block, ~line 1185) **re-walks EVERY marker EVERY frame** — `n_iter` counts the full O(n) pass:
for each marker it does `project_marker` → `project_screen` (current view/zoom) → off-screen cull →
the live event-flag visibility gates → clustering (pile push) → `draw_marker` + `hover_test`. With
~8477 markers on a full map this is the dominant overlay cost (felt lag at high counts, per the
counter comments). NOTE the map-space→page projection (`m.live_u/v/page`) is ALREADY cached/prewarmed
once (`s_prewarmed`, config `live_projection`); what's recomputed per-frame is the screen transform +
cull + gates + cluster piles + hover, plus the whole iteration itself.

**★ FOLLOW-UP #1 (MAJOR, do first): frame-coherent caching — only rebuild when the camera or mouse
moved.** On an idle frame (no pan/zoom change AND no mouse move) the drawn set, cluster piles, and
hover result are IDENTICAL to last frame. Detect "view unchanged" (cache last view.zoom + pan/offset +
the open group/page) and "mouse unchanged"; when both hold, skip the O(n) rebuild and re-emit the
cached draw list / hover hit. Caveat: the visibility gates read LIVE event flags (collected/cleared,
story flags, map fragments) — those CAN change without camera/mouse motion (player loots an item mid-
map-open), so either (a) also invalidate on a cheap flag-generation tick, or (b) refresh gates on a
throttle (e.g. every N frames) while caching projection+cull+cluster between. Biggest win for the
common case: the map sits open, nothing moving.

**FOLLOW-UP #2 (DONE 2026-06-30, branch `feat/spatial-grid-cull`): viewport-cull of the clustered hot
path.** The cheap variant landed first: instead of a persistent per-cell index, the loop still sweeps
O(n) but skips the expensive *gates* for clustered-eligible markers whose 256-unit pile cell
(`spatial_grid.hpp` `grid_cell_key`, kTileSize=256) can't be on screen — `proj::unproject_screen` of the
4 screen corners → map-space rect, +1 tile margin (`map_renderer.cpp` ~1543), cull at ~1663. Result:
`render.worldmap.markers` **3.58 → 1.28 ms (~64%)**, verified in-game (zoom in/out, no markers vanish at
edges). Proven visually invariant (margin == cell size, so on-screen-centroid piles keep every member).
The full *persistent-grid* version (O(visible) sweep via `SpatialGrid::for_cells_in_rect`) is the later
step IF the O(n) scan still shows up — measurement says it won't, so it's parked. Composes with #1.

**SEPARATE BUG (spawned 2026-06-30, NOT the cull): zoom/pan marker re-adjust.** User sees markers
"re-adjust for a fraction of a second" on zoom + pan. Decoupled from the cull (which is provably
visually invariant — see above). Cause is the pre-existing **`ViewDelay` motion-sync**
(`goblin_projection.hpp` `ViewDelay`, `map_renderer.cpp` `kViewDelayFrames=1.0f`, `g_view_delay.apply`
~1408): markers are projected from the view **1 frame in the past** to match the engine's eased basemap,
but it's a fixed *frame-count* delay — it can't track *variable* frame time. With frame spikes (run
2026-06-30: 15 spikes ~7ms, `present.frame_wall` max 356ms) the 1-frame offset drifts from the basemap
ease → markers snap on stop. FIX candidates: (a) make `kViewDelayFrames` a live config to A/B tune
in-game; (b) switch to a *time-based* (ms) ease instead of frame-count; (c) match the engine's actual
basemap interpolation curve. Reproduces on master (cull off).

**STATUS 2026-06-30 NIGHT: fix candidate (a) IMPLEMENTED** on branch `feat/view-delay-tune` (stacked on
`feat/spatial-grid-cull`): `kViewDelayFrames` constexpr → live config `view_delay_frames` (default 1.0,
`apply()` clamps to ring `[0, N-1]`) + an F1 **"Marker motion delay (frames)"** slider [0..7]
(`goblin_overlay.cpp`, "Marker scale" header) so it's tunable live (no restart) and Save-to-INI persists.
Deployed `4ec98027` (cull + knob + slider). NEXT: pan the map and tune the slider — if one fixed frame
count kills the re-adjust at all frame rates, keep it as the new default; if it only holds at steady fps
and drifts on spikes, escalate to candidate (b) time-based ease. Record the chosen value here.

Related UX backlog (separate, spawned as a task this session): a per-category **density slider** +
**item search bar** for the same ~8477-marker pressure — see [[nobake-coverage-scoreboard]] context and
the `baked_only` diag toggle (also a marker-loop filter). These are overlay-UX, the two above are pure
render-perf.
