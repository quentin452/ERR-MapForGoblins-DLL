# Vision note — MapForGoblins as the seed of a runtime modding framework

Status: **vision / research note, NOT a plan** (2026-07-01, from a user design discussion).
No implementation scheduled; recorded so the direction survives sessions. When any piece becomes
real work, scope it as a `docs/plans/` plan first.

## The idea

The no-bake direction (runtime memory access + disk `.msb`/EMEVD reads instead of baked data —
`baked_data_full_removal_plan.md`) is the same architecture a full "runtime mod" needs: a mod that
adds items/bosses/content WITHOUT shipping a `regulation.bin` or repacked FromSoft archives.
Instead of file replacement (Smithbox → regulation.bin, ME2 overrides), the DLL grafts changes in
memory after the game loads its own files. Ultimate user-facing shape: one folder = one DLL +
config + plain assets; zero game files touched; uninstall = delete folder; composable with
other overhauls (no regulation.bin merge wars).

## What this repo ALREADY has (further along than it looks)

- **Runtime param row injection — already shipped, small scale**: `goblin_tutorial_popup.cpp`
  injects TutorialParam rows + FMG entries at runtime; `goblin_inject.cpp` injects
  WorldMapPointParam rows. Adding items = same machinery pointed at
  EquipParamGoods/EquipParamWeapon (walk param_list, copy a template row, patch IDs).
- **VFS/disk bricks**: `dvdbnd_reader` (BDT/BHD reads), Oodle IAT hook (captures decompressed
  TPF/sblytbnd), `force_load_file` via CSFile (by-path resident loads).
- **Generic infra**: AOB sig framework with health surfacing (`[SIG]`), DX12+ImGui overlay,
  event-flag reads, item-grant/flag hooks, GPU texture harvest.

## Honest constraints (from the discussion, validated)

- **Save persistence**: a custom item ID picked up gets written into the `.sl2`. Loading without
  the DLL leaves an orphan ID (item vanishes or worse). Any injected-item design must treat
  "DLL must be present at load" as a hard contract, and pick IDs from reserved high ranges to
  avoid colliding with overhauls (compat ≠ coherence: Convergence etc. still interact logically).
- **Models/animations are the hard wall**: a playable model = geometry + skeleton + materials +
  collision + Havok behavior/animations. "Swap the vertex buffers in RAM" does NOT work as a
  design; the realistic path is serving a VALID FLVER/BND through a file-resolution hook — i.e.
  you still author FromSoft formats, you just deliver them without touching the install.
  FBX/Assimp→FLVER at runtime would mean writing a FLVER writer in the DLL (big, separate project).
- **Param table growth**: tables are sorted row-descriptor arrays; appending in bulk needs
  realloc+resort or slack exploitation. Row-copy (what we do) is proven; mass-add is not.

## Framework split (GoblinFramework core + client mods)

Natural core/client boundary is ALREADY being built: the hot-reload Slice C split
(`overlay_hot_reload_playwright_plan.md`) separates a host (hooks, sig-scan, params, VFS, DX12
overlay, `overlay_api` ~110 fns + import lib) from a render/client module via LoadLibrary.
That host surface IS the future framework API.

**Decision: do NOT extract a framework speculatively.** Extract the core lib when a SECOND mod
actually exists (same discipline as "ERR is the dev install, not the target boundary"). Until
then, the only action is keeping the `overlay_api`/render-DLL boundary clean.
