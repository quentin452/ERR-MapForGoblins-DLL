# RE PROMPT — memory anchor for the active mod's data root (auto-locate maps, no config)

You are a reverse-engineering agent on **Windows** with Ghidra/IDA + the live Elden Ring (ERR mod)
process + RPM. Find a **static memory anchor** that yields the **active mod's data root** (or the
resolved regulation/map path) so the injected DLL can locate the mod's `map\MapStudio\*.msb.dcx` at
runtime with **zero config**. Deliver in this repo's signature style (`src/re_signatures.hpp`:
patch-resilient AOB + `relative_offsets {{3,7}}` to lift a singleton slot from a `mov reg,[rip+disp]`).

## Why
The disk-MSB loot feature (`loot_from_disk_msb`, `src/worldmap/loot_disk.cpp`) reads loot from the
mod's real `map\MapStudio\*.msb.dcx`. Today it locates that dir via config `loot_msb_dir` or an
ancestor-walk from the DLL folder (commit 14897b5) — fragile: it misses mods mounted OUTSIDE the game
dir (e.g. a ModEngine package at `~/Downloads/Convergence_mod`), and forces per-profile config +
per-profile launch testing. If instead the DLL reads **where the game itself loaded its data from**,
the feature becomes loader-agnostic (ME3 / ModEngine2 / UXM / vanilla), profile-agnostic, and
config-free — and the whole "set up + launch each profile" problem dissolves (it self-locates whatever
is mounted). Params are already read live from `SoloParamRepository`; the ONLY missing piece is the
on-disk map directory.

## What's known (offline recon — start warm, don't re-derive)
- **eldenring.exe strings** (confirmed present): `CSRegulationManager`, `CSRegulationStep`,
  `CSRegulationStep::STEP_WaitLoadLocalFile` (the local-file regulation load), and the Dantelion IO
  aliases `Core.IO.Alias.regulation`, `Core.IO.Alias.mapstudio`, `capture:/mapstudio`,
  `Core.IO.Alias.cap_mapstudio`. So ER loads regulation + maps through the **Dantelion virtual file
  system** keyed by named aliases ("regulation", "mapstudio") that resolve to real device roots.
- **ME3 redirect**: the bundled ModEngine3 (`internals/modengine/`) mounts the mod via a filesystem
  override (`me3_mod_host`: `asset_hooks.rs` / `filesystem.rs override`). It passes its attach config
  to the injected host via env `ME3_ATTACH_CONFIG` + an IPC bridge `ME3_BRIDGE_FILE_*`; `mod_host`
  exports only `DllMain` (no queryable API) and has a `GetCurrentPackageId`. The `.me3` profile lists
  `[[packages]] path = "../../mod"`. Reading ME3's own structures is ME3-version-specific — PREFER the
  **game-side resolved path** (works for any loader).
- This is the same singleton-anchor game as the rest of the project: e.g. `EVENT_FLAG_MAN_SLOT` AOB +
  `relative_offsets {{3,7}}`, and the WorldMapManImp anchor-walk. Match that format.

## Questions (in order)
1. **CSRegulationManager singleton.** Find its static slot (AOB → `mov reg,[rip+disp]`). Does it (or
   the `CSRegulationStep::STEP_WaitLoadLocalFile` site) retain the **resolved regulation file path** or
   a pointer to the source device/root? If so: offset to that string.
2. **Dantelion file-device / IO-alias resolver (the loader-agnostic target).** Find the manager that
   maps the `"regulation"` / `"mapstudio"` aliases to real device roots (DLFileDeviceManager /
   virtual-root table / DLDeviceManager). Under ME3, what real directory does the `"mapstudio"` alias
   (or its backing device) resolve to? Give the anchor (static slot AOB) + the offset/iteration chain
   to that **root path string**. The map dir is the prize; the regulation path (Q1) is a fallback whose
   parent is the same mod root.
3. **Deliverable anchor.** A static-base → offset chain (RPM-safe; the DLL reads in-process) that
   yields a directory string from which `<root>\map\MapStudio` (or the loaded `.msb.dcx` set) is
   reachable. VALIDATE it live on the running ERR process and paste the **actual resolved path** it
   reads (under ME3/Proton if available, else Windows). State whether it's loader-agnostic or ME3-only.
4. **Timing/stability.** Is the anchor populated before our DLL's first map build (init-time) and
   stable for the session? Regulation loads early at boot; confirm the chain is non-null by the time
   `load_disk_treasures()` runs (or note when it becomes valid).

## Deliverable (mirror docs/re/windows_resident_msbe_layout_re_findings.md)
- The AOB(s) + `relative_offsets` for each anchor, ready to drop into `re_signatures.hpp`.
- The exact offset chain: `slot -> [+a] -> [+b] -> path string`, with field meanings.
- A live sample: the resolved root/path string read from the running game.
- A one-line verdict: which anchor to use for the **map dir** (regulation-path-parent vs mapstudio-alias
  root vs file-device mount), and loader-agnostic vs ME3-only.

## Bar
We already have a working fallback (config `loot_msb_dir` + ancestor-walk, and a possible CreateFileW
hook). A clean static anchor must beat those: no process-wide hook, no config, stable across launches.
If the only viable path is a file-open hook (CreateFileW / NtCreateFile / the Dantelion DLFileOperator
open), say so and name the best hook point + the arg holding the resolved path.
