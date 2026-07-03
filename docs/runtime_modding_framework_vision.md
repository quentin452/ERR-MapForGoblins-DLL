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
  **→ RESOLUTION: the shadow / sidecar save (see the dedicated section below)** — a DLL-owned
  `<save>.mfg` that (in the strip-and-reinject variant) keeps the `.sl2` vanilla-clean and downgrades
  the hard "DLL-at-load" contract to soft. Note the SHIPPED param-override loader needs none of this
  (param edits don't persist).
- **Models/animations are the hard wall**: a playable model = geometry + skeleton + materials +
  collision + Havok behavior/animations. "Swap the vertex buffers in RAM" does NOT work as a
  design; the realistic path is serving a VALID FLVER/BND through a file-resolution hook — i.e.
  you still author FromSoft formats, you just deliver them without touching the install.
  FBX/Assimp→FLVER at runtime would mean writing a FLVER writer in the DLL (big, separate project).
- **Param table growth**: tables are sorted row-descriptor arrays; appending in bulk needs
  realloc+resort or slack exploitation. Row-copy (what we do) is proven; mass-add is not.

## Save persistence — the shadow / sidecar save (design direction, 2026-07-03)

Question raised by the user: you can't cram custom framework state into ELDEN RING's `.sl2` — should
the framework keep a **"shadowing save"** (a sidecar file the DLL owns) instead? **Yes — that is the
right architecture, and it resolves the save-persistence constraint above.** Scoped (deferred) as
**`docs/plans/shadow_sidecar_save_plan.md`** — kept for later; it becomes real work only when Gap C
(item grants) starts.

**What does / doesn't need saving (tier the problem):**
- **Param FIELD edits (the shipped param-override loader, Slices 1-3) need NO save at all.** Params
  reload from `regulation.bin` every boot and the DLL re-applies the overrides; nothing is written to
  the `.sl2`. Already save-safe by construction — the shadow save is NOT for this tier.
- **What DOES need out-of-schema persistence:** granted **custom items** (IDs the vanilla `.sl2`
  inventory has no legit home for), **custom event flags** (beyond the vanilla flag block), and any
  **framework-specific progress** (mod quest state, counters). These are what the sidecar holds.

**The sidecar model (`<save>.mfg` next to the `.sl2`):** the DLL keeps a small file — the source of
truth for all framework state — and syncs it to the game's own save/load lifecycle. Two ways to run it:
- **(A) Strip-and-reinject (preferred — keeps the `.sl2` VANILLA-CLEAN).** Hook inventory/flag
  serialization: on SAVE, strip our custom entries out of what the game writes (so the `.sl2` never
  contains a custom ID); persist them to the sidecar instead. On LOAD, read the sidecar and re-grant
  the items / re-set the flags into the live session. Result: the `.sl2` stays a legal vanilla save,
  and — the big win — **this DOWNGRADES the "DLL-must-be-present-at-load" hard contract to soft**:
  loading without the framework just means the modded items are absent that session (no orphan IDs,
  no corruption). Uninstall = delete the folder + the `.mfg`, save still loads clean. This is the
  "one folder, zero game files touched, clean uninstall" vision, fully realized.
- **(B) Reserved-range + tolerate (simpler, dirtier).** Let custom items sit in the `.sl2` in a
  reserved high ID range; accept orphans if loaded DLL-less. Easier (no serializer RE) but violates
  "clean save"; only a stopgap.

**The hard part of (A) is RE:** finding the save/load serializer for inventory + the flag block to
strip/reinject, plus a robust **binding key** so a sidecar can't desync from its `.sl2` (candidates:
steam id + character slot + a stamped GUID; must survive the user copying/backing-up saves). Atomicity
matters — write the sidecar in lockstep with the game's save event, tolerate a crash between the two.

**Relation to the reserved-ID policy (Gap H):** the sidecar doesn't remove the need to pick custom IDs
from a reserved high range (to avoid colliding with overhauls in the LIVE session) — it removes the
need for those IDs to survive in the `.sl2`. Freeze the reserved-range + binding-key policy before
shipping anything that grants items. See the battle order in `runtime_live_capabilities_audit.md`
(Gap C is gated on Gap H); the sidecar is the mechanism that makes Gap C safe.

## Framework split (GoblinFramework core + client mods)

Natural core/client boundary is ALREADY being built: the hot-reload Slice C split
(`overlay_hot_reload_playwright_plan.md`) separates a host (hooks, sig-scan, params, VFS, DX12
overlay, `overlay_api` ~110 fns + import lib) from a render/client module via LoadLibrary.
That host surface IS the future framework API.

**Decision: do NOT extract a framework speculatively.** Extract the core lib when a SECOND mod
actually exists (same discipline as "ERR is the dev install, not the target boundary"). Until
then, the only action is keeping the `overlay_api`/render-DLL boundary clean.

**Related decision — embedded scripting API: `scripting_api_roi_note.md` (2026-07-02, NOT YET).**
Moving feature logic C++→scripts was assessed and deferred: enormous binding surface, hot paths +
the feature core stay C++, it doesn't touch the RE bottleneck, and it's speculative by this same
discipline. The near-term win instead = a **data-driven category/filter descriptor** + host-reload
improvements (see that note).
