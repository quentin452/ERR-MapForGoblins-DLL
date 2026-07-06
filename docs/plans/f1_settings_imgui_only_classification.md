# F1 / ini settings — ImGui-only-map classification

Inventory 2026-07-04. Classifies every setting through the **ImGui-only map** lens (native ER map
retired, vmap = sole surface). Axis: **KEEP** (still meaningful on the vmap) · **HARDCODE-TRUE** (bake
on, drop toggle) · **DROP** (native-map-only or dead → remove) · **PORT** (native-map feature to
re-point at the vmap). Different axis from `settings_sweep_plan.md` (preference/hardcode/dev/dead) — a
sweep-"keep" can be a DROP here. Execute as part of Track C3 in `imgui_only_map_plan.md` — AFTER the
switch, never before (these knobs still drive the native map until then).

Sources: `goblin_config_schema.cpp`, `goblin_config.hpp`. ~150 ini keys (~90 are category `show_*`).

## Phase-1a DONE (2026-07-06) — behavioral native-map knobs baked to defaults, F1/ini toggles removed
Removed as user-facing toggles (baked ON / to default in code; suppression + clip + motion-sync logic
kept, always-on): **`grace_overlay`, `grace_suppress_native`, `landmark_suppress_native`,
`suppress_native_bosses`, `clip_game_ui`, `view_delay_frames`, `view_delay_zoom`.** Behavior unchanged
(all defaulted true/1.0). Both builds green. Deferred to a later Phase: `dial_*` (9), `ui_exclusion_rects`
(interactive editor), the dev/diag probes (kept — hidden RE tooling, not user settings).

## Phase-1b DONE (2026-07-06) — game-UI exclusion subsystem removed
Removed the entire ERR-dial / user-zone marker exclusion subsystem (it only clipped overlay markers
under the retired native map's own always-on-top UI): `ui_exclusion_rects` + the 9 `dial_*` keys, their
F1 "UI exclusion zones" section (zone editor + dial placement), the `game_ui_exclusion_alpha` soft-fade,
`in_game_ui_exclusion`, `scale_vtx_alpha`, the `ui_rect_*`/`dial_edit` accessors, and the
`cfg_uiExclusionRects_ref` export. `draw_marker` no longer fades. Both builds green.

## DROP — native-map-only / dead (retire after the switch)
- **Native-map overlay clipping/exclusion:** ~~`clip_game_ui`~~ ✅DONE, ~~`ui_exclusion_rects`~~ ✅DONE.
- **ERR day/night dial (9 keys):** ~~`dial_disc_x/y/r`, `dial_pill_x0/y0/x1/y1`, `dial_fade_margin`~~ ✅DONE.
- **Native-pin suppression:** ~~`landmark_suppress_native`, `grace_suppress_native`, `suppress_native_bosses`~~ ✅DONE (baked ON).
- **Native-basemap motion-sync:** ~~`view_delay_frames`, `view_delay_zoom`~~ ✅DONE (baked to default).
- **Native-map RE/diag probes:** `debug_worldmap_probe`, `debug_page_switch`, `dump_native_pins`,
  `debug_menu_cover_diag`, `debug_map_clip_diag`, `debug_scissor_probe`, `overlay_markers_proto`
  (KEEP for now — hidden dev/RE tooling, removing loses tooling for no user benefit).

## HARDCODE-TRUE
- ~~`grace_overlay`~~ ✅DONE — baked true, toggle dropped (overlay is the sole grace source).

## PORT — re-point at the vmap (Track A build items)
- `show_region_labels` + `region_toggles` + `debug_region_volumes` → **A7** (region labels on the vmap).
- The **entire `[Clustering]` block** (`enable_clustering`, `cluster_spiderfy`, `cluster_hard`,
  `cluster_threshold`, `cluster_distance_adaptive`, `cluster_near_threshold`, `cluster_near_radius`,
  `cluster_far_radius`, `cluster_exclude`, `cluster_threshold_overrides`, `cluster_debug_radius`,
  `cluster_debug_markers`) + `debug_cluster_anchors` → **A8** (vmap plots raw today).
- `stack_identical_items` → **A8** (co-located ×N stacking).
- `show_minimap` → flip its open-gate from `world_map_open()` to `virtual_map_open()`.
- Item search (**A9**) has no ini key of its own (panel state) — the 3rd PORT feature.

## KEEP — surface-agnostic (the bulk)
All ~90 category `show_*` toggles; the 7 `[Display Sections]`; marker style/scale (`overlay_master_scale`,
`overlay_icon_scale`, `icon_legibility`, `icon_min_half_px`, `altitude_cue`, `redify_boss_icons`);
data gates (`require_map_fragments`, `collected_graying`, `hide_collected`, `hide_killed_bosses`,
`baked_only`); loot labeling (`live_loot_labels`, `anonymous_loot`, `drop_merchant_phantoms`,
`loot_msb_dir`); masters (`show_all`, `show_all_except`, `icons_hidden`); UI (`overlay_toggle_key/gamepad`,
`overlay_language`, `virtual_keyboard_layout`, `pause_on_open`, `quest_progress`, quest knobs); minimap
appearance (`minimap_zoom/size/opacity/anchor*/offset*`); enemy-bar HUD; `[Param Overrides]`, `[Sidecar]`;
and the whole dev/RE/watchdog/bench set that isn't native-map-specific (RPC, watchdogs, `diag_*`,
`probe_field_*`, `bench_*`, `fix_midsession_resolution`, `debug_render_dims`, marker dump…).

## ⚠ `?` — resolve BEFORE deleting
1. **`native_item_icons`** — does the game's `MENU_MAP` GPU icon stay resident if the native map never
   opens? The vmap reads this flag (`panel_virtual_map.cpp:587`). If the sprite isn't resident without a
   native map open → this becomes PORT/DROP (fall back to atlas/circle). **Test before trusting.**
2. **`dump_icon_textures` / `dump_converters`** — the vmap's marker projection (A3) uses the LIVE
   converter grid, and icons may still need the harvested atlas. Confirm the vmap doesn't depend on these
   native-map-sourced probes before deleting them.

## Rollup
~60 non-category knobs: **~34 KEEP · 1 HARDCODE-TRUE · ~19 DROP · ~15 PORT** (clustering dominates PORT).
All ~90 category toggles KEEP. The DROP set only becomes safe to delete once the native map is off
(Track C1); until then it's live.
