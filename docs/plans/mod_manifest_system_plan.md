# Mod manifest / bundle system plan

Status: **DESIGN + slice 1 STARTED 2026-07-05.** One `mod.toml` manifest declares the WHOLE MapForGoblins mod
(worlds + objects + style + markers + bundle + items); `goblin::mod::load(folder)` realizes it. Unifies the
currently-scattered per-subsystem TOMLs behind one modder-authored entry point.

## Why
Today each subsystem self-loads its own TOML at boot (independent `*_boot(mod_folder)` calls in `dllmain`):
`virtual_worlds.toml` (vworld), `world_bundle.toml` (AEG edits), `custom_items.toml`, `param_overrides.ini`,
+ the planned `objects.toml` (proc-mesh 3D) and the new `[style]` (postfx). A modder has no single place that
says "this is my mod." The manifest is that place — declarative, one file, framework-realized.

## Schema (`<mod_folder>/mod.toml`)
```toml
[mod]
name    = "My Cool Mod"
version = "1.0"
author  = "…"

# Global restyle of ER's render (goblin_postfx, greybox job #2b).
[style]
enabled  = true
mode     = "grayscale"    # grayscale | posterize | edge | edge_desat
strength = 1.0

# Section refs — delegate to the existing sub-loaders (default filenames if omitted).
[worlds]  file = "virtual_worlds.toml"   # vworld::load
[bundle]  file = "world_bundle.toml"     # world_bundle::apply  (AEG edits)
[items]   file = "custom_items.toml"     # custom_items::apply

# Future: proc-mesh 3D objects (needs the proc-mesh lib + r3d realizer).
# [[object]] id="plat_01" world="dev_arena" pos=[…] primitive="box" size=[…] material="greybox" …
```

## Orchestration — `goblin::mod::load(mod_folder)`
- **No `mod.toml`** → no-op; the subsystems load via their own boot calls exactly as today (backward-compat).
- **With `mod.toml`** → parse it; realize each declared section. Slice 1 realizes `[mod]` (metadata, logged) +
  `[style]` (→ `postfx::set_mode/strength/enabled`). Later slices take over the worlds/bundle/items load order
  (the manifest becomes THE boot orchestrator, replacing the scattered `*_boot` calls) + the `[[object]]`
  realizer once the proc-mesh library exists.

## Slices
1. **✅ STARTED 2026-07-05:** `goblin_mod.{hpp,cpp}` — parse `mod.toml`, `[mod]` metadata + `[style]`→postfx at
   boot; RPC `mod status|reload`. Proves the manifest drives a real subsystem end-to-end. Backward-compatible.
2. Manifest OWNS the load order: delegate `[worlds]/[bundle]/[items]` to their loaders from `mod::load` (retire
   the standalone `init_*` boot calls; a missing section = skip). One declarative boot.
3. `[[object]]` realizer: proc-mesh gen (primitives → CSG → …) + `add_collision` box, drawn by r3d. Depends on
   the proc-mesh library (`virtual_world_3d_backend_plan.md` step 3).
4. Named **style presets** (`[style.greybox]`, `[style.noir]`) a mod picks; per-world style overrides.
5. Packaging / validation: a `mfg_min` framework-version gate; a realizer that LOGS any field/section it can't
   yet honor (the "dev-dimension realizer-logs-gaps" idea) → the missing-primitive checklist writes itself.

## Compatibility rule (ship discipline, from the design principles)
The manifest is ADDITIVE over the existing standalone loaders — a mod with no `mod.toml` behaves exactly as
before, and each section is optional. Mod-agnostic: `[style]` postfx works on any install (operates on the
final frame); `[worlds]/[bundle]/[items]` reuse the already-mod-agnostic loaders.
