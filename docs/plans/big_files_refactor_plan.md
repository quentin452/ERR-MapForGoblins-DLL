# Big-files refactor plan — god functions, duplication, split seams

Status: **scoped, not started** (audit 2026-07-01, 4 parallel read-only investigators over the 7
biggest hand-written files). Fork an implementation branch per item when work starts; land items
independently — nothing here is one big-bang refactor.

Scope note: `src/generated_*` (~210k lines) is codegen output, NOT a refactor target (its removal
is `baked_data_full_removal_plan.md`). `goblin_inject.cpp` was already de-godded
(`goblin_inject_refactor_plan.md`, COMPLETE). Input hooks already extracted
(`input_module_refactor_plan.md`, DONE). This plan covers what's left.

**Coordination:** `overlay_hot_reload_playwright_plan.md` Slice C (render-DLL LoadLibrary split) is
in flight and touches `goblin_overlay_render.cpp` / `goblin_overlay.cpp` / `CMakeLists.txt`.
Item 1 below (draw_panel split) must land AFTER Slice C settles, on top of its file layout — the
per-section panel files belong to the render module's source list (`GOBLIN_RENDER_SOURCES`).

## God functions found (audit snapshot; line refs = state at 105742b)

| Function | File | Lines | Mixes |
|---|---|---|---|
| `draw_panel()` | src/goblin_overlay_render.cpp:362 | ~1680 | 10 independent panel sections |
| `build_buckets_impl()` | src/worldmap/map_entry_layer.cpp:1809 | ~1159 | 11-pass marker build + 82 interleaved spdlog diags |
| `hk_present()` | src/goblin_overlay.cpp:1272 | ~518 | input poll + map render + ImGui + GPU fence/submit |
| `read_wgm_snapshot()` | src/goblin_collected.cpp:358 | ~517 | RB-tree walk + instance resolve + 2 dev probes + loot scan |
| `render_markers()` | src/worldmap/map_renderer.cpp:1359 | ~418 | 8 phases; marker loop duplicated in `draw_minimap()` |
| `refresh()` | src/goblin_collected.cpp:1074 | ~338 | GEOF classify + WGM classify + sticky + write-back |
| `probe_loop()` | src/goblin_worldmap_probe.cpp:711 | ~219 | cursor resolve + coord tracking + diag dumps |

## Ranked items (each independently landable)

### 1. Split `draw_panel()` into per-section files — HIGH ROI, mechanical
Sections have zero interdependence. Extract: visibility/settings grid, quest browser
(`quest_browser_panel.cpp`), clustering, debug/dev tools. Evidence of cost: the 2026-07-01
settings-search fix had to touch all 10 sections inside one function.
**After hot-reload Slice C** (same files).

### 2. Extract the duplicated marker-gate loop (map vs minimap) — BUG-RISK KILLER
`render_markers()` (map_renderer.cpp:1577) and `draw_minimap()` (:1847) run near-identical loops:
same gates (discover/secondary/hide-when/fragment/fog), same player-state setup (:1383 vs :1799),
same uiScale/icon-size math (:1451 vs :1812). A gate fix applied to one silently misses the other.
Extract `marker_passes_gates()` predicate + shared `setup_player_state()` + `compute_icon_scale()`.

### 3. De-duplicate item classification in map_entry_layer.cpp — ~100 LOC, one choke point
- 4-line classify chain copy-pasted **7×** (lines 283/457/551/609/733/977/984) →
  `classify_loot(int32_t key) -> {int cat, bool live_fallback}`.
- loot-identity resolution (`resolve_loot_item_textid` + `item_marker_category`) **~20×** →
  `resolve_loot_identity(lot_id, type, fallback_textid)`.
- `WORLD_MAP_POINT_PARAM_ST` init boilerplate **16×** → `make_wmp(const DiskEntity&)`.
Mod-agnostic acceptance logic concentrates here — single point to fix classification bugs.

### 4. Quarantine dev/RE diagnostics out of production functions (~500–700 lines)
- goblin_collected.cpp: FIELDINS probes (:565–602, :636–712), MAPINS/LOOTPOS diag → `*_diag.cpp`.
- goblin_worldmap_probe.cpp: `dump_converters()` (:491, 101L), `dump_native_pins()`,
  `input_delta_scan()` → diag file.
- icon_harvest/overlay: grace debug viewer, `dump_icon_textures_live()` phases.
Production functions shrink; `read_wgm_snapshot()` loses ~150 lines without behavior change.

### 5. Shared `icon_uv()` — cross-file duplicate
goblin_overlay_render.cpp:656 (`baked_uv` lambda) ≡ map_renderer.cpp:62 (`icon_uv()`): identical
ICON_CELLS strcmp+UV computation. One shared header (e.g. `goblin_overlay_icons_util.hpp`).

### 6. Decompose remaining god functions (medium, per-function branches)
- `build_buckets_impl()`: extract entity-pos cache build (dup ×2 at :1921/:1928), split the 7
  world-feature builders (~520L, :1059–1629) into `world_feature_builders.cpp`, diag recorder out.
- `hk_present()`: extract `input_poll_frame()` (:1300), `gpu_submit_frame()` (:1717); keep the
  map+UI middle as-is (hot path, hot-reload split already isolates the draw layer).
- `read_wgm_snapshot()`: extract `walk_wgm_tree()` (~208L), `resolve_wgm_instances()` (~132L),
  `scan_mapins_loot()` (~157L).
- `refresh()`: extract `classify_wgm_collected()` (:1198–1270, deeply nested dup-name logic),
  `apply_collected_hiding()` (:1360, two near-identical g_param_ptrs loops → one
  `apply_param_hide_state(val)`).
- `probe_loop()`: split resolve/track/diag. Consolidate the seh_read/write wrapper bloat (:111–141)
  — but ONLY per the SEH rules in `clang_only_toolchain_plan.md` (RPM vs noinline-call pattern).
- goblin_collected.cpp `remap_row_ids()`: same remap loop ×3 (:994/:1005/:1022) → one template.

### 7. Grace-sprite state unification — ARCHITECTURAL, LAST
Split today across icon_harvest (discovery, candidates, `g_grace_*` globals) and overlay (SRV
creation/display) via raw shared globals. Known hazard: overlay caches SRV handles into
icon_harvest GPU textures with **no invalidation** if a texture is rebuilt/evicted. Needs a small
design (versioned handle or single owner module) — do after the mechanical wins above.

## Order
Mechanical first (1 after Slice C, 3, 5 — pure moves; cross-build after each), then
behavior-preserving dedup (2, 4, 6), grace-sprite design (7) last.
