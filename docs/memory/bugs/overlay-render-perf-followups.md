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

**FOLLOW-UP #2 (LATER): spatial grid / index to make the per-frame loop O(visible) not O(n).** Bucket
markers by map-space cell (per group/page) once at build; per frame, only visit buckets overlapping
the viewport (for cull/draw) + the bucket under the mouse (for hover). Turns the full O(n) sweep into
O(markers near screen), the real fix when zoomed in with thousands of markers. Composes with #1 (the
grid is rebuilt only on camera move; #1 skips even that on idle). Also helps the cluster/pile pass
(`render.worldmap.clusters`) and `nearest_region`.

Related UX backlog (separate, spawned as a task this session): a per-category **density slider** +
**item search bar** for the same ~8477-marker pressure — see [[nobake-coverage-scoreboard]] context and
the `baked_only` diag toggle (also a marker-loop filter). These are overlay-UX, the two above are pure
render-perf.
