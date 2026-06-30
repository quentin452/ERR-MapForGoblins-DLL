# Implementation plans

Forward-looking implementation plans live here (per the "plans live on `master`" policy in `AGENTS.md`).
A plan stays here until its work lands; mark items done in `docs/memory/bugs/` + `docs/changelog.md` and
keep the plan only while it still has open steps. Fork an implementation branch from `master` when work
on a plan actually starts.

(Research, findings, RE recipes, and knowledge docs are NOT plans — those stay in `docs/`, `docs/re/`,
or `docs/memory/`.)

| Plan | Status |
|------|--------|
| [feat_quests_implementation_plan.md](feat_quests_implementation_plan.md) — quest browser + runtime QuestNpcLayer | not started |
| [dx_bugs_backlog_plan.md](dx_bugs_backlog_plan.md) — DX bug/QoL backlog (PRs A–E) | items 1, 7, 8, 9 done; C/D/E open |
| [spatial_grid_opti_plan.md](spatial_grid_opti_plan.md) — spatial grid: clustering + viewport-cull perf | DONE (both halves merged; full persistent-grid step deemed unnecessary by measurement) |
| [loot_item_count_plan.md](loot_item_count_plan.md) — loot undercount fix + ×N stacking | DONE 2026-06-30, runtime-confirmed |
| [loot_name_native_getmessage_refactor_plan.md](loot_name_native_getmessage_refactor_plan.md) — native GetMessage → kill FMG slot-walk + #ifdef MFG_VANILLA | refactor landed + ERR-verified + dead-code cleanup landed; vanilla+DLC verify open |
| [generated_data_removal_plan.md](generated_data_removal_plan.md) — remove the per-profile map-data bake (`generated_*`) | not started |
| [mapgenie_category_coverage_plan.md](mapgenie_category_coverage_plan.md) — 31 missing MapGenie categories + 2 custom respawn categories | scoped; depends on generated_data_removal_plan Phase B |
