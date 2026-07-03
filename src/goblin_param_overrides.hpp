#pragma once

// ── Slice 3: boot-time param-override loader ──────────────────────────────────────────────────
// Reads `param_overrides.ini` (next to MapForGoblins.ini) and applies each field edit to the live
// param table via goblin::paramedit::param_set_field_by_name. This is the shippable "mod ER without
// a regulation.bin" surface: edits are made in RAM to the ACTIVE install's params, save-safe (reset
// each launch, re-applied by the DLL), touch no game files, composable with any overhaul.
//
// Gated on ini `[Param Overrides] param_overrides` (default OFF — it mutates balance). Must be called
// AFTER params are ready (the from::params init gate) — dllmain wires it next to inject_tutorial_popup.
// Plan: docs/plans/param_override_loader_plan.md (Slice 3).

namespace goblin
{
// Apply param_overrides.ini if the gate flag is on and the file exists. No-op (logged) otherwise.
// Every bad line is logged + skipped, never fatal (a file authored for a DIFFERENT mod degrades
// gracefully). Returns the number of edits successfully applied.
int apply_param_overrides();
}  // namespace goblin
