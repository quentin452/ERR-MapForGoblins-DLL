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
- **Dev-drive primitives (built + in-game verified 2026-07-03, RPC-driven, all SEH-guarded):**
  `goblin::inventory::give_item(id, qty)` — grant (qty>0) / REMOVE (qty<0) via the game's AddItemFunc
  (ER has no separate RemoveItem; removal = negative qty). `goblin::warp::to_grace(graceId)` — fast-travel
  to any grace via the game's own Lua-event warp (proper area-load; dev-world navigation + testing the
  overlay across map areas). Both from the Hexinton CT recipes, static AOBs in `re_signatures.hpp`. Join
  the param/FMG/row primitives + the sidecar save as the framework's runtime surface.

## Honest constraints (from the discussion, validated)

- **Save persistence**: a custom item ID picked up gets written into the `.sl2`. Loading without
  the DLL leaves an orphan ID (item vanishes or worse). Any injected-item design must treat
  "DLL must be present at load" as a hard contract, and pick IDs from reserved high ranges to
  <!-- Gap H is now FROZEN: docs/memory/process/reserved-id-and-load-contract.md -->
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

## Future directions (bigger bets — TRACKED for later, 2026-07-03)

Three directional bets raised by the user. Not scoped plans — captured so they aren't lost. Each is
connected to what already exists so the on-ramp is visible.

### 1. World Virtualization — a FRAMEWORK feature (not existing-mod interop)
**Scope (clarified 2026-07-03): this is about the FRAMEWORK's OWN worlds, not swapping third-party
overhauls.** A "world" = a DATA BUNDLE authored WITH the framework (param overrides + custom items + FMG
names + map/loot edits + flags + its own save context), all applied at RUNTIME over one shared base
game. Virtualization = the framework holds N such worlds and switches the ACTIVE one live — no reinstall,
no regulation.bin swap, no touching the install. (The "Convergence ⟷ ERR" line is only an ANALOGY for
the user-facing benefit — the framework does NOT need to ingest an overhaul's regulation.bin; that
external-interop problem is a separate, much larger VFS bet and explicitly out of scope here.)
- **Directly on the ramp — the pieces mostly EXIST:** regulation-free param overrides
  (`param_overrides.ini`), the `custom_items.toml` author surface, live map edits + `refresh_markers`,
  and the sidecar save-clean (`shadow_sidecar_save_plan.md`) that keeps each world's state out of the
  vanilla `.sl2`. A world is just a grouping of these authored artifacts.
- **What's actually missing (the real work):** (a) a WORLD BUNDLE format grouping the params/items/
  names/map-edits/flags of one world; (b) an ACTIVATION step that resets to base then applies the active
  bundle (params reload from regulation each boot, so "reset to base" is cheap; the live overrides just
  need to be re-driven) + a `refresh_markers`; (c) SAVE-CONTEXT switching so each world has its own
  sidecar (`.mfg`) state and they don't cross-contaminate. Shares almost everything with #2 (the editor
  authors a world; virtualization stores + swaps worlds). This is a tractable framework feature, not a
  VFS research project.

### 2. In-Game World Editor (ImGui, param-based) — SLICE 1 LANDED 2026-07-03
An ImGui frontend over the runtime primitives — edit params/items/loot/markers live and SEE it, no
rebuild. **This is the closest of the three: the whole live-edit loop already exists.** **Slice 1 is
IN** — F1 → "World Editor (live)" (`panel_world_editor.cpp`): pick an asset, see the loot item its map
marker resolves to, re-skin that lot's `lotItemId01`, `Refresh markers` → on the map. Next: a real
asset/item picker, repoint-to-lot, lot-cloning (needs `refresh_markers` v2), and SAVE edits as a world
bundle (feeds #1). We built it this
session — `param_setf`/`param_clone` (edit), `custom_items` (define), `loot_at` (inspect what a marker
resolves), `pickUpItemLotParamId` repoint + `lotItemId01` (re-skin a loot spot), and **`refresh_markers`
(see the edit on the map)**. The editor = a panel wiring these to widgets (pick asset → pick item →
apply → refresh). On-ramp: the F1 panel, `category_descriptor` Tier 2 (runtime live-add), and the
`refresh_markers` v2 (incremental regen + LotReader-index reset so NEW cloned lots resolve, not just
existing ones — see HANDOFF).

### 3. 3D model variants + reuse across worlds
Vary/retexture placed models and reuse models in new worlds. The asset/geometry frontier — AEG assets,
FLVER models, MSB placements. **Hardest + furthest:** map markers already read placements from disk MSB,
but CREATING/varying placements needs MSB write + model handling, which is outside the current runtime
framework (repointing only re-skins existing placements; new coords/models need MSB editing). Long
horizon; depends on an MSB-write path that doesn't exist yet. **Progress 2026-07-03:** the MSB-write path
is now partly cracked at the INSTANCE layer — MOVE an existing placement is a solved+durable live primitive
(`vtable[0xd0] SetWorldMatrix`), and ADD a new placement is scoped (`spawn_clone` route; see
`docs/re/windows_msb_placement_write_re_findings.md`).

### 4. Mesh-FREE custom objects — Havok collision + ImGui procedural render (user idea 2026-07-05)
**★ DESIGN DECISION (user, 2026-07-05) — split the render path by world TYPE, so each uses the right tool
and neither fights a wall:**
- **Editing a BASE ER world (a real dimension) → AEG assets.** The modder picks which existing AEG asset to
  place; the engine streams + renders + collides it natively (real shaded mesh). This is the geom-spawn
  pivot-2 path (name-driven `AEG###_###`, `windows_geom_spawn_pivot2_re_findings.md`) — reuse EXISTING ER
  assets, no new mesh. The MSB/FLVER *creation* wall (#3) is avoided because you only place assets that
  already exist in the game.
- **A custom VIRTUAL world (ours) → ALWAYS ImGui procedural.** Havok collision (`add_collision`) + ImGui
  billboard/wireframe/grid. **"Aucun problème possible"** — zero dependency on the engine's asset/streaming/
  MSB system, so no write-frontier, no mesh authoring, 100% mod code. The greybox aesthetic IS the virtual
  world's aesthetic (it's ours, it doesn't have to look like ER).
This resolves the tension cleanly: real ER worlds get real assets (AEG, engine-rendered); our worlds get the
free, unblocked ImGui path. Below is the ImGui path (the virtual-world half).

**The clever sidestep of #3's AEG/MSB/FLVER wall for a GREYBOX/schematic world.** A modder's custom object
does NOT need an authored mesh: give it (a) a **Havok collision volume** — `add_collision` (Route D box) is
LIVE-PROVEN, injects a static walkable/blocking body into the live `hknpWorld`, the engine does collision
itself, no `.hkx`/AEG file; and (b) a **procedural ImGui visual** — project the object's world XYZ to screen
and draw a diamond / line / box-wireframe / level-grid with the modder's chosen colour. Author surface =
`{pos, shape, size, colour, collision extents}` in a `.toml`. No mesh handling at all.
- **Pipeline (user's diagram):** Havok raycast/OBB → exact impact `(wX,wY,wZ)` → **3D world-to-screen** →
  `ImDrawList` primitive. The collision half + the raycast are DONE (`add_collision`, `goblin_heightfield`
  cast). The renderer is trivial (we draw ImGui everywhere).
- **THE ONE MISSING PRIMITIVE = a 3D world-to-screen** (the gameplay camera view-proj matrix). We have the
  vmap's 2D map w2s but NOT the 3D one. It's scoped in the freecam RE (`GameRendCameraSet` er+0x680460 /
  `CSCameraImp`, `windows_freecam_re_{prompt,findings}.md`) but the view-matrix offset + the `CSCameraImp`
  singleton AOB are still pending Ghidra. Extract that → `w2s3d(xyz)=viewproj*xyz→NDC→screen` and this whole
  path opens. Freecam and this share the exact same blocker, so they unlock together.
- **Honest caveats:** ImGui draws are 2D overlays (billboards/wireframes), NOT shaded 3D meshes → a
  schematic/greybox look, not photoreal. No depth occlusion by default (draws over walls) — gate visibility
  with a per-object Havok raycast (we have it) or accept always-on-top for a dev world. So a "custom object"
  = invisible walkable collision + an ImGui glyph marking it. Legit for a dev/greybox/modding world; not a
  AAA asset.
- **Why it's the RIGHT modder on-ramp:** #3 (real meshes) needs the unsolved MSB/FLVER write wall. THIS path
  needs zero mesh authoring — the framework owns Havok-collision + ImGui-draw; the modder supplies pos +
  shape + colour. Strictly simpler + already 90% built (only the 3D w2s remains). It is `far_terrain_relief_
  plan.md` Layer 2 ("make it exist under the player's feet") realised via greybox instead of streamed geom.
- **Quadtree reuse (user Q):** the vmap `MarkerQuadtree` is a 2D XZ index for MAP-viewport cull+cluster; 3D
  objects cull by camera FRUSTUM + distance + behind-camera reject, a different axis. Reuse it only as a
  COARSE broad-phase (XZ-near-camera query → skip projecting far objects), then per-candidate do the 3D
  project + frustum/behind/LOD cull. For a handful of dev objects, skip it and project all.

### Dev "creative mode" mini-track (tooling for #2/#3 — scoped 2026-07-03)
Two small/moderate RE items that make world-editing (move/ADD/spawn) practical to build + verify by giving
a dev sandbox loop — **warp into a throwaway map + fly around + place/move/verify, isolated from the live
save's world.** Do AFTER the ADD primitive; not on its critical path. Full scope in `docs/HANDOFF.md`.
- **(1) Warp-to-(mapid, x,y,z) dev primitive** — today's `warp` only fast-travels to an existing grace by
  id (`LuaWarp_01`); the missing piece is a coordinate/map-id warp so the dev can drop into any existing map
  at any coord. Sandbox = an existing SPARSE map (arena / cleared dungeon / overworld tile), NOT a new one.
- **(2) Freecam** — detach camera from player + input-drive it; the verification+authoring tool for move/ADD
  and the World Editor. Moderate, well-trodden ER RE (not a frontier).
- **Capstone (LATER, not this mini-track):** creating a genuinely NEW empty map/page FROM SCRATCH = full map
  creation (new MSB + collision + streaming/worldmap registration) — strictly harder than ADD. The sandbox
  deliberately reuses an existing map so it isn't blocked on this.
