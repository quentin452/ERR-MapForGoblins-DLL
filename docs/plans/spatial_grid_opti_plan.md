# Spatial Grid Rendering Optimization Plan (v2)

> **STATUS 2026-06-30:** the **clustering half is DONE** (`feat/spatial-grid`, PR E, merged) —
> `spatial_grid.hpp` + `grid_cell_key` exist and `draw_clusters` now groups by map-space tile (DX
> items 8/9). The **perf half is NOT done yet**: the render hot loop still scans every marker per frame
> (`for L : layers → for m : markers()`); the `SpatialGrid` is built/queried only for clustering, not
> yet wired into a viewport-culling marker pass. That viewport-cull optimization is the remaining work
> below.
>
> **STATUS 2026-06-30 PM — perf half, step 1 done (`feat/spatial-grid-cull`).** Measured baseline first
> (step 7): `render.worldmap.markers` avg **3.58 ms** (max 11.65), `render.worldmap.clusters` ~**0 ms**
> — so the cost is the per-marker **visibility gates**, and `project_marker` is already cached. Chose
> the **minimal viewport-cull** over a persistent grid: clustered-eligible markers (which are NOT
> screen-culled, since off-screen members feed their pile) now skip their gates when their map-space
> falls outside the unprojected viewport rect (+1 tile). New `unproject_screen` in `goblin_projection.hpp`;
> rect computed once/frame; ~15-line loop change, no persistent grid / invalidation state. Pile centroids
> intact (one-tile margin keeps edge-straddling cells whole). AWAITING in-game re-measure + visual check
> (no markers vanish). If the residual cheap O(N) iteration still shows up, the full persistent-grid
> version below is the follow-up; the measurement says it likely won't.

This plan removes the per-frame O(N) marker scan from the world-map render loop. Today
every visible layer is iterated in full (`for L : layers → for m : L->markers()`) on every
frame in the Present-thread hot loop; when zoomed in, ~95% of those markers are off-screen.
We bucket markers into a **2D spatial grid** keyed on Elden Ring's 256 m map tile and query
only the cells intersecting the visible viewport → O(visible) (typically 100–300 markers).

> **v2 note.** Rewritten after an audit. Fixes: the world↔map-space conversion the v1 plan
> glossed over; method names that don't exist (`finalize`/`collect` — the layers build a
> lazy `cache_` in `markers()`); grid invalidation (critical because the planned
> `QuestNpcLayer` rebuilds its cache dynamically); feeding BOTH the marker pass AND the
> cluster pass from the grid; and a measure-first step (projection is already cached, so the
> real per-marker cost may be the gate/category checks, not projection).

---

## 0. Why now — marker-count trajectory

* **Today: ~8,000 markers** (matches the runtime `[COVERAGE]` count and the
  `overlay-render-perf-followups` note). The per-frame O(N) scan is already documented
  TODO #2 there.
* **Soon: >10,000 markers.** `docs/coverage_vs_mapgenie.md` lists **31 marker categories
  present in MapGenie but NOT YET WIRED** in the mod (the next feature backlog). Wiring even
  a fraction of those (NPCs, materials, smaller collectibles…) pushes the total past 10k.
* So this is not premature optimization: the O(N) loop scales linearly with a count we
  *intend* to grow. The grid makes the hot loop scale with what's on screen, not with the
  total — and it makes the planned tile-clustering (DX backlog items 8–9) almost free,
  since the grid cells already ARE the tile buckets.

---

## 1. Existing code this builds on (verified)

| Need | Already in tree |
| --- | --- |
| Render loop to replace | `for (auto *L : layers) { … for (const Marker &m : L->markers()) … }` at [map_renderer.cpp](../../src/worldmap/map_renderer.cpp) L1223 (marker pass) + L1359 (cluster pass) |
| Timing | `GOBLIN_BENCH_QUIET("render.worldmap.markers")` / `…clusters` already split the two passes |
| Forward projection | `proj::project_screen(mU, mV, view, realW, realH)` in [goblin_projection.hpp](../../src/goblin_projection.hpp) (consumes **map-space**, not unified world) |
| **Inverse** projection | `proj::unproject_local(localX, localZ, zoom, panX, panZ, …)` → map-space = (screen+pan)/zoom — **already exists**, de-risks the viewport query |
| World→map-space step | "Unified world coords → map-space" conversion at [map_renderer.cpp](../../src/worldmap/map_renderer.cpp) L370 (markers store unified `worldX/worldZ`; the projector wants map-space) |
| Layer marker storage | each layer caches into `mutable std::vector<Marker> cache_`, built lazily in `markers()` (`GraceLayer`, `MapEntryLayer`) — NOT a `collect()`/`finalize()` method |

---

## 2. Coordinate spaces — the conversion the query must respect

This is the subtlety v1 missed. There are **three** spaces:
1. **Unified world** `(worldX, worldZ)` — what every `Marker` stores.
2. **Map-space** `(mU, mV)` — what `project_screen` / `unproject_local` operate in (the
   per-marker unified→map-space conversion lives at map_renderer.cpp L370).
3. **Screen px** — the backbuffer.

Two clean options; **pick (B)**:

* **(A)** Key the grid on unified-world tiles (`gridX = floor(worldX/256)`), then for the
  viewport query, unproject the 4 screen corners → map-space → invert the world→map-space
  step → world bounds. Requires an inverse of the L370 conversion (extra work, and that
  step may be non-linear per area).
* **(B) — chosen.** Compute each marker's **map-space (mU,mV) once at grid-build time**
  (we already call the same conversion when projecting) and key the grid on **map-space
  tiles**. The viewport query is then a pure `unproject_local` of the 4 corners — no extra
  inverse needed, and it matches exactly what the projector consumes. Cell size = the
  map-space extent of one 256 m world tile (constant per the projection's scale).

Document this choice in the header; it removes the v1 ambiguity.

---

## 3. Spatial grid data structure

#### [NEW] [src/worldmap/spatial_grid.hpp](../../src/worldmap/spatial_grid.hpp) / [.cpp](../../src/worldmap/spatial_grid.cpp)
```cpp
namespace goblin::worldmap {
class SpatialGrid {
public:
    void clear();
    // Insert a marker pointer under its map-space tile. mU/mV computed by the caller
    // (same unified->map-space conversion the projector uses), so the grid stays
    // projection-agnostic. The pointer must outlive the grid (see §5 lifetime).
    void insert(const Marker *m, float mU, float mV, int group);
    // Append every marker whose tile intersects the map-space rect, for `group` only.
    void query_rect(float min_u, float min_v, float max_u, float max_v,
                    int group, std::vector<const Marker *> &out) const;
    // Iterate occupied cells intersecting the rect (for tile-clustering: each cell = a pile).
    template <class Fn> void for_cells_in_rect(float min_u, float min_v, float max_u,
                                               float max_v, int group, Fn &&fn) const;
private:
    struct Cell { std::vector<const Marker *> markers; };
    // key = (group << 20) | (tu << 10) | tv   (tu/tv = map-space tile indices)
    std::unordered_map<uint32_t, Cell> cells_;
};
}
```
The `group` is folded into the key so the open-page filter (`m.group != open_grp`) is a
free consequence of which cells we touch — no per-marker group branch in the hot loop.

---

## 4. Grid ownership & population

**Each `MarkerLayer` owns its grid** (modular; only active layers are queried). Add to the
base interface a non-virtual accessor returning a `const SpatialGrid&`, plus a protected
`rebuild_grid_from_cache()` helper the layers call right after they (re)build `cache_`.

#### [MODIFY] [src/worldmap/marker_layer.hpp](../../src/worldmap/marker_layer.hpp)
* Add `SpatialGrid grid_;` (mutable) and `const SpatialGrid &grid() const { return grid_; }`.
* Add `protected: void rebuild_grid_from_cache();` — clears `grid_` and re-inserts every
  marker in `cache_`, computing each `(mU,mV)` via the shared unified→map-space conversion.

#### [MODIFY] [src/worldmap/grace_layer.cpp](../../src/worldmap/grace_layer.cpp) / [map_entry_layer.cpp](../../src/worldmap/map_entry_layer.cpp)
* In the existing `markers()` body, **after** `cache_` is populated (the lazy build that
  already runs once via the call-once / cache guard), call `rebuild_grid_from_cache()`.
* For `MapEntryLayer`, whose cache is built once up front, the grid is likewise built once.

---

## 5. Lifetime & invalidation (the v1 gap)

The grid stores `const Marker *` into each layer's `cache_`. **Rule: whenever `cache_` is
rebuilt or its storage may reallocate, the grid MUST be rebuilt in the same step** (call
`rebuild_grid_from_cache()` at the end of every `cache_` (re)build, never incrementally).

* **Static layers** (`GraceLayer`, `MapEntryLayer`): cache built once per map load → grid
  built once. Safe.
* **⚠️ Dynamic layer** — the planned `QuestNpcLayer` (see the quests plan) rebuilds its
  cache when a quest flag flips. It MUST call `rebuild_grid_from_cache()` inside the same
  rebuild, or its grid will hold dangling pointers. This is the shared invalidation
  discipline the three in-flight features (spatial-grid, DX clustering, quests) must all
  follow — document it once in `marker_layer.hpp`.

---

## 6. Optimized render loop

#### [MODIFY] [src/worldmap/map_renderer.cpp](../../src/worldmap/map_renderer.cpp)
* **Viewport bounds (map-space):** `visible_map_bounds(view, realW, realH, …)` =
  `unproject_local` of the 4 screen corners (handles rotation/zoom), take min/max of (mU,mV).
* **Marker pass (L1223):** replace the full scan with a grid query:
  ```cpp
  std::vector<const Marker *> vis;
  for (auto *L : layers)
      if (L && L->visible())
          L->grid().query_rect(min_u, min_v, max_u, max_v, open_grp, vis);
  for (const Marker *m : vis) { /* fine cull + project + gate + draw_marker */ }
  ```
  Keep the existing per-marker gate/cull/`draw_marker` body unchanged — only the iteration
  source changes.
* **Cluster pass (L1359) — feed it from the grid too.** The cluster pass is *also* O(N)
  today (it groups by `cluster_key`). Do NOT optimize only the marker pass or half the win
  is lost. When tile-clustering is active (DX items 8–9), use `for_cells_in_rect` so each
  occupied cell is a ready-made pile; for the legacy nearest-grace clustering, still restrict
  the input to `query_rect` results so the grouping is over visible markers only.

---

## 7. Measure first (don't assume the bottleneck)

The projection is **already cached**, so the per-marker cost is partly the gate/category
checks, not projection. Before and after each change, capture `render.worldmap.markers` and
`render.worldmap.clusters` from the bench log so we attribute the win correctly.

### Benchmark
1. **Baseline (O(N)):** all categories on, panned to a dense legacy dungeon, zoomed in and
   zoomed out. Record both bench markers at ~8k total.
2. **After grid:** same scenes. Expect the *zoomed-in* case to drop sharply (few visible
   cells); the *fully zoomed-out* case is the worst case (most cells visible) — confirm it's
   no worse than baseline.
3. **Scale test:** synthetically duplicate markers to ~12k (proxy for the 31 MapGenie
   categories) and confirm zoomed-in frame time stays ~flat while baseline O(N) grows.

### Correctness
4. **Cull parity:** pan/zoom slowly across edges — no pop-in, no missing icons at the
   seams (the query rect must be inclusive of partially-visible edge tiles; pad by one cell).
5. **Cluster parity:** pile count and member counts match flat iteration when toggling grid
   on/off.
6. **Dynamic layer:** with `QuestNpcLayer` active, flip a quest flag and confirm its marker
   moves/updates with no stale or dangling draw (validates §5).

---

## 8. Future: Phase 2 — Grid of Quadtrees (now better justified)

With the 31 MapGenie categories wired we cross 10k and, at maximum zoom-out, a flat grid
query touches many cells. If that worst case ever matters, host a small quadtree inside each
grid cell (hybrid **grid-of-quadtrees**): a node fully inside the viewport contributes all
its markers at once → O(log N) range search; dense legacy dungeons split deeper, open fields
stay flat. Defer until the §7 scale test shows the zoomed-out case is actually a problem —
the flat grid is expected to suffice through the first tranche of new categories.
