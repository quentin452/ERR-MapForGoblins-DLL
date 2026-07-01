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
| [input_module_refactor_plan.md](input_module_refactor_plan.md) — extract input hooks (mouse/kb/gamepad/raw/DirectInput) out of `goblin_overlay.cpp` into `src/input/` | DONE, merged `feat/input-module` 2026-07-01 |
| [overlay_hot_reload_playwright_plan.md](overlay_hot_reload_playwright_plan.md) — reload only the ImGui overlay draw layer live + Route B debug-RPC AI iterate loop against the real game | scoped, not started; Windows-only |
| [goblin_inject_refactor_plan.md](goblin_inject_refactor_plan.md) — extract subsystems (icon-harvest, item classification, visibility+clustering) out of `goblin_inject.cpp`, the biggest hand-written god file | PR 0 + PR 1 (icon-harvest) DONE+MERGED; PRs 2-4 not started |
