# Implementation plans

Forward-looking implementation plans live here (per the "plans live on `master`" policy in `AGENTS.md`).
A plan stays here until its work lands; mark items done in `docs/memory/bugs/` + `docs/changelog.md` and
keep the plan only while it still has open steps. Fork an implementation branch from `master` when work
on a plan actually starts.

(Research, findings, RE recipes, and knowledge docs are NOT plans — those stay in `docs/`, `docs/re/`,
or `docs/memory/`.)

| Plan | Status |
|------|--------|
| [feat_quests_implementation_plan.md](feat_quests_implementation_plan.md) — quest browser + runtime QuestNpcLayer | Phase 1 landed `feat/quest-npc-layer`, builds clean, NOT runtime-verified; demo NPC entity_id/progress_flag unsourced (0 pins until then) |
| [dx_bugs_backlog_plan.md](dx_bugs_backlog_plan.md) — DX bug/QoL backlog (PRs A–E) | A/B/C/E done (verified in code + changelog 2026-07-01); only D (in-game pause) open, needs RE spike first |
| [spatial_grid_opti_plan.md](spatial_grid_opti_plan.md) — spatial grid: clustering + viewport-cull perf | DONE (both halves merged; full persistent-grid step deemed unnecessary by measurement) |
| [loot_item_count_plan.md](loot_item_count_plan.md) — loot undercount fix + ×N stacking | DONE 2026-06-30, runtime-confirmed |
| [loot_name_native_getmessage_refactor_plan.md](loot_name_native_getmessage_refactor_plan.md) — native GetMessage → kill FMG slot-walk + #ifdef MFG_VANILLA | refactor landed + ERR-verified + dead-code cleanup landed; vanilla+DLC verify open |
| [baked_data_full_removal_plan.md](baked_data_full_removal_plan.md) — replace ALL baked data with the runtime/disk path (subsumes the map-data-bake plan as Phase 0) | scoped, not started |
| [generated_data_removal_plan.md](generated_data_removal_plan.md) — remove the per-profile map-data bake (`generated_*`) | not started (= Phase 0 of the full-removal plan) |
| [mapgenie_category_coverage_plan.md](mapgenie_category_coverage_plan.md) — 31 missing MapGenie categories + 2 custom respawn categories | scoped; depends on generated_data_removal_plan Phase B |
| [merchant_item_search_plan.md](merchant_item_search_plan.md) — make merchant/shop stock (incl. Twin Maidens bell-bearing goods) searchable in F1 | Slice 1 DONE + in-game verified (`feat/merchant-search`); Slice 2 (name seller) deferred, Slice 3 (pins) open |
| [category_descriptor_plan.md](category_descriptor_plan.md) — single data descriptor for a marker category (kills the ~6 scattered switches); Tier 2 = runtime live-add | **Tier 1 COMPLETE + in-game verified** on `feat/category-descriptor` (enum+name+section+landmark+glyphs+coverage all from data/categories.json); Tier 2 (runtime live-add) open |
| [input_module_refactor_plan.md](input_module_refactor_plan.md) — extract input hooks (mouse/kb/gamepad/raw/DirectInput) out of `goblin_overlay.cpp` into `src/input/` | DONE, merged `feat/input-module` 2026-07-01 |
| [overlay_hot_reload_playwright_plan.md](overlay_hot_reload_playwright_plan.md) — reload only the ImGui overlay draw layer live + Route B debug-RPC AI iterate loop against the real game | scoped, not started; Windows-only |
| [goblin_inject_refactor_plan.md](goblin_inject_refactor_plan.md) — extract subsystems (icon-harvest, item classification, visibility+clustering) out of `goblin_inject.cpp`, the biggest hand-written god file | PR 0-4c DONE+MERGED — plan COMPLETE except 4d (intentional stay-behind, not a PR) |
| [big_files_refactor_plan.md](big_files_refactor_plan.md) — god functions / duplication / split seams across the 7 biggest hand-written files (draw_panel, build_buckets_impl, hk_present, read_wgm_snapshot, …) | scoped 2026-07-01, not started; item 1 waits on hot-reload Slice C |
| [clang_only_toolchain_plan.md](clang_only_toolchain_plan.md) — retire MSVC/msbuild, clang-cl+lld+ninja as the only toolchain | Phase 0's 3 `__try`-elision hazards FIXED 2026-07-01 (`5b80541`); repo-wide `__try` classify pass + Phases 1–2 open; reverses same-day "MSVC canonical" memory note |
| [settings_sweep_plan.md](settings_sweep_plan.md) — classify every F1/ini knob into keep / hardcode-calibration / dev-quarantine / delete | AUDIT DONE 2026-07-02 (full classification table); 8 ⚠FLAG items need user decision before executing; `[Debug]` ini section is the dumping ground (calib + dev jumbled) |
| [param_override_loader_plan.md](param_override_loader_plan.md) — regulation.bin-free runtime param modding: generic `param_set_field` → name-addressed → boot-time override file | **ALL 3 SLICES DONE + IN-GAME VERIFIED + MERGED to master 2026-07-03** (`param_overrides.ini`, gated `[Param Overrides] param_overrides`, default OFF; changelog Added). Optional follow-ups: F1 UI, more registry fields, tier-2 shipped Paramdex |
| [shadow_sidecar_save_plan.md](shadow_sidecar_save_plan.md) — DLL-owned `<save>.mfg` sidecar for out-of-schema state (custom items/flags/progress); strip-and-reinject keeps `.sl2` vanilla-clean | **SCOPED 2026-07-03, DEFERRED** — gated on Gap H (reserved-ID policy) + Gap C (item grants); the shipped param-override loader needs none of this. Gating RE: save/load serializer + binding key |
| [custom_item_end_to_end_plan.md](custom_item_end_to_end_plan.md) — Gap C: a custom item without a regulation.bin (clone+stat+name, then grant) | **DEFINE half DONE + in-game verified** 2026-07-03 (custom goods 90000001 composed from the 3 primitives); GRANT half gated on inventory-accessor RE + the sidecar save |
| [virtual_world_multi_world_design.md](virtual_world_multi_world_design.md) — mod-owned virtual worlds (World Virtualization #1): collision (framework assigns position, not player), active-world tracking, open-via-M, how ER's baked map tiles work | DESIGN 2026-07-04; slice A (canvas) done, B/C/D scoped |
| [map_tile_loading_plan.md](map_tile_loading_plan.md) — load ER's real world-map tile art onto the mod virtual-map canvas (endgame phase-1a): DCX→DDS→GPU chain exists; the one gap = a BHF4 (tpfbhd/tpfbdt) entry-table parser (format CRACKED); RAM-harvest vs disk-extract paths | SCOPED + format-cracked 2026-07-04, not built |
