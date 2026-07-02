# HANDOFF — live work queue

Living cross-session queue of in-progress / not-yet-finished work. Update at the end of each session.
Committed code + `docs/changelog.md` are the record of DONE; this file tracks WHAT'S NEXT and WHY.

**Housekeeping note (2026-07-01, done):** this file had grown to 1264 lines, mostly stale entries
for work long since merged + in-game confirmed, whose content already lived in `docs/changelog.md`
or a `docs/plans/*.md` file marked COMPLETE. Backfilled 2 real gaps found in `changelog.md` (the F3
Alt+Tab keyboard-dead fix, the minimap search-hit edge-clamp + search-hint fix — both now under
`[Unreleased] > Fixed`), then deleted every HANDOFF section whose user-facing content was confirmed
present in changelog, or whose pure-refactor content was confirmed present in a COMPLETE plan doc.
Kept: genuinely live/in-progress work, open questions, and standing knowledge (gotchas, deferred
decisions, non-obvious facts) not fully captured anywhere else. If you're looking for the history of
something not below, check `docs/changelog.md` first, then the relevant `docs/plans/*.md`.

Last updated: 2026-07-02 (SINGLE-DLL migration: per-profile builds retired, ERR runtime-detected;
+ the 9 native-pin parity landmark categories in-game verified — see below).

## RESUME HERE (2026-07-01z10) — overlay_hot_reload_playwright_plan Slice C fully implemented, not yet merged/in-game-confirmed for the split build

`feat/overlay-loadlibrary-mechanism` (off `master`, 1 commit): the real two-DLL split
(`GOBLIN_OVERLAY_HOTRELOAD=ON`) now builds AND links clean (`build-linux-hotreload/`
`MapForGoblins.dll` + `goblin_overlay_render.dll` both produced), alongside the unchanged default
single-DLL build (`build-linux/`, in-game confirmed clean — SIG 29/29, grace SRVs, `render.minimap`
firing, no crash, all the new macro plumbing inert there as designed).

New: `src/goblin_dll_export.hpp` (`GOBLIN_RENDER_API` macro), `src/goblin_overlay_render_loader.{hpp,cpp}`
(consolidates every host→render call — not just the 3 draw functions; the real link surfaced 3
more: `prebuild_markers`/`inworld_hovered`/`refresh_overlay_census` — via `extern "C"` +
`GetProcAddress`, resolved once early in `dllmain.cpp`'s init sequence, idempotent).

**Two real corrections the actual link found, that BOTH prior audits missed:** (1) render calls
more host functions than audited (`loot_disk.cpp`'s disk-loaders, `worldmap_probe::project`, and
`goblin::ui::read_event_flag` called directly by a GENERATED file that can't be hand-edited); (2)
raw `extern` DATA (`goblin::config::*`, `param_list_address`) can't be fixed by a wrapper
FUNCTION — dllexport must be on the declaration the DEFINING `.cpp` sees, so
`goblin_config.hpp`/`goblin_inject.hpp`/`loot_disk.hpp`/`goblin_worldmap_probe.hpp`/`from/params.hpp`
got direct `GOBLIN_RENDER_API` annotations instead. Full detail + the general lesson ("link-time
verification is the only reliable way to find the true cross-DLL surface — budget for this in
Slice D too") in the plan doc.

**Not yet done:** merge to `master`; in-game confirm of the split build's actual runtime behavior
(Windows-only — this box is Linux, can cross-build both configs but can't run `LoadLibrary`
against the real game to verify the render DLL actually loads/renders). Next session: merge, then
either do the Windows in-game confirm or move straight to Slice D (file-watcher + real reload) —
full detail in `docs/plans/overlay_hot_reload_playwright_plan.md`.

## Clang-only Phase 1 — WINDOWS BUILD + SNAPSHOT VALIDATED (2026-07-02) → only the in-game matrix left

`build.bat` is now ninja+clang-cl (no VS/msbuild; tool paths env-overridable, defaults = the
Windows box per `build-toolchain-clang-xwin.md`); `/Brepro` determinism PROVEN on Linux (relink →
identical md5); PDB pairs archived to `pdb-archive/<ver>-<profile>/`; `tools/lint_seh.py` guards
the SEH-elision regression. Old `build/` msbuild dir is disposable (`build-err/` replaces it).

**Default build VALIDATED on Windows (2026-07-02):** `build.bat` (ERR) ran clean end to end —
auto-configure (CMake 4.1 + Ninja + `clang-cl-xwin.cmake`, Clang 22.1.8), `[80/80]` compile+link →
`[SUCCESS]`, `build-err/{MapForGoblins.dll 4.6 MB,.lib,.pdb}`. Points to note:
- **0 real errors.** The only `Failed` line is `Performing Test CMAKE_HAVE_LIBC_PTHREAD - Failed` —
  expected on Windows (no pthread), CMake falls back correctly.
- **340 warnings, all benign / third-party**, two dominant recurring sources: (1)
  `-Wdeprecated-literal-operator` on `operator"" _a` inside vendored spdlog bundled-fmt; (2)
  `-Wdeprecated-declarations` on `std::wstring_convert`/`<codecvt>` in `src/from/params.hpp:17`
  (deprecated C++17, still functional). Both suppressible if we ever want a clean log
  (`_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING`); not worth churn now. (`snapshot` adds imgui
  `-Wnontrivial-memcall` on its `memset(this,…)` ctors — same benign class.)

**`build.bat snapshot` VALIDATED on Windows (2026-07-02) — packaging + PDB archive proven.** Full
`pre-1.0.18` (ERR) snapshot ran end to end: data pipeline (964 MSB, 28313 placements, 0 MSB errors)
→ reconfigure `-DVERSION_PRE=pre` → `[64/64]` link `[SUCCESS]` → `mfg_inigen` INI → package under
`pre-release/` (`dll/offline/{dll,ini}` + `addons/MapForGoblins/menu/02_120_worldmap.gfx` + LICENSE)
→ **PDB pair archived to `pdb-archive/pre-1.0.18-err/` (DLL + 27 MB PDB, NOT shipped in the package)**
→ README version-substituted (`vpre-1.0.18`). Crash-symbolication chain verified: shipped DLL is
**byte-identical (SHA-256)** across `pre-release/`, `pdb-archive/`, `build-err/`.

**Still open:** `build.bat release` (version-bump path) un-exercised; then Phase 2's real in-game
validation matrix. Docs already flipped to "clang = canonical" + `steam_api64.lib` removed. See
`docs/plans/clang_only_toolchain_plan.md` Phase 1/2.

## Two new plans scoped (2026-07-01): big-files refactor + clang-only toolchain

`docs/plans/big_files_refactor_plan.md` (god functions/duplication across the 7 biggest hand-written
files; item 1 = draw_panel split, waits on hot-reload Slice C) and
`docs/plans/clang_only_toolchain_plan.md` (retire MSVC; USER DECISION reverses the same-day "MSVC
canonical" note in `docs/memory/tooling/build-toolchain-clang-xwin.md`). **Phase 0 update: the 3
`__try`-elision hazards (world_position per-frame probes + tutorial_popup init poll) are FIXED
(`5b80541`, built + deployed); still open = repo-wide `__try` classify pass, then Phases 1–2.**

## SINGLE-DLL migration — profiles retired (2026-07-02, `feat/mapgenie-landmark-parity`)

User audit request confirmed profiles had ~no reason left: zero real per-profile DATA (the only
divergent bakes were EMPTY non-ERR stubs of ERR-only tables + `legacy_conv`, which is a pre-param-
residence fallback only — the live `goblin_legacy_fold` is primary). Implemented:
- `goblin::err_features_enabled()` (goblin_config.cpp) replaces compile-time `profile_is_vanilla()`:
  runtime disk fingerprint `menu/deploy/projects/ELDENRINGReforged` (ancestor-walk from the DLL
  folder, "mod" overlay first, then the eldenring.exe dir; cached; logs `[PROFILE]`). ERR-only
  config force-disabled off-ERR at load, exactly like the old vanilla build.
- DELETED: `GENERATED_SUBDIR` CMake machinery, `MFG_VANILLA`/`MFG_PROFILE_VANILLA` defines,
  `tools/gen_nonerr_stubs.py`, `src/generated_{vanilla,erte,convergence}/` dirs (were gitignored),
  per-profile build dirs. `src/generated/` is THE single bake dir.
- build.bat `--vanilla/--convergence/--erte` KEPT but now only select packaging assets
  (README/gfx/SNAP_DIR) + offline pipeline data source; all profiles build/ship the same DLL from
  `build-err/`. inigen always emits the full ini (ERR entries included everywhere).
- `liveLootLabels` single default = false (vanilla package used to default true — changelog notes it).
- **ERR detection VERIFIED in-game (2026-07-02):** `[PROFILE] ERR install DETECTED (fingerprint
  menu\deploy\projects\ELDENRINGReforged) — ERR-only config active`. **Still to verify:** a vanilla
  me3 launch — expect `[PROFILE] ERR install not detected` + Reforged sections absent from F1 + ini
  rewritten without ERR sections (see `docs/memory/tooling/me3-cli-nonerr-launch.md` for the launch
  recipe). Windows `build.bat` + `build.bat snapshot` re-run (script edited; the validated snapshot
  flow predates this change). Fingerprint risk: if some ERR release ships without
  `menu/deploy/projects/ELDENRINGReforged`, add a second fingerprint or an ini override knob.
- Baked-data plan impact: Phase A per-profile regen is MOOT; remaining bake work (name_regions/
  region_anchors → disk-MSB runtime, icon atlas) unchanged.

## Native-pin PARITY — 9 new landmark categories IMPLEMENTED + in-game VERIFIED (2026-07-02)

`feat/mapgenie-landmark-parity`: full audit of native WMPP pins (every family the game still draws
that we didn't re-draw) → 9 new categories via the same `build_live_landmarks` pass: Churches /
Ruins / Rises & Towers / Shacks / Forts / Castles / Towns & Villages / Colosseums / Unique Sites
(~167 rows; + iconId 62 Ashen Leyndell → LegacyDungeon). All WMPP pins are natively eventFlag-gated
(discovered-only) — we show all. Full audit table + skip rationale (42 sub-zones, 87 quest markers,
0 ERR-arena) in `docs/memory/features/mapgenie-landmark-categories.md`. ERR build DEPLOYED to
`~/Games/ERRv2.2.9.6/dll/offline/`. **Next: in-game verify** — grep `[LANDMARKLIVE]` (now a loop
log listing all 16 landmark cats; expect Churches ~28, Ruins ~38, RisesTowers ~21, Shacks ~24,
Forts 7, Castles 6, TownsVillages ~14, Colosseums 3, UniqueSites ~26), toggle a few in F1, then
merge. Colosseum got a `category_gpu_iconId` (24) native-glyph entry, rest = circle.
**VERIFIED in-game on ERR (2026-07-02): `[LANDMARKLIVE] built 295` — all 16 categories, every
count exactly as predicted; SIG 29/29 clean, 0 errors.** Ready to merge.

**Side finding (SUPERSEDED same day by the single-DLL migration above):** the non-ERR profile
bakes were stale/incomplete (vanilla missed the whole Group1/2 enum block → its build had been
broken since Group 1; erte/convergence missed whole files). Fixed first by syncing headers, then
made moot by deleting the per-profile dirs entirely.

## Native-map landmark icon suppression — TRACKED, not started (2026-07-02)

The game still draws its OWN landmark pins on the native world map (Minor Erdtrees etc.), so our
new landmark categories (incl. the 5 that now use the native glyphs — DivineTower/Evergaol/
MinorErdtree/GrandLift/MiquellaCross) can visually DUPLICATE the native pins when toggled ON.
User decision: acceptable for now; the definitive fix is suppressing the native pins. Technical
precedent exists: native grace suppression (`goblin_grace_suppression.cpp`) and the areaNo=99
row-flip eviction used by section visibility — the same trick applied to the landmark
WorldMapPointParam rows (gated on "our category is ON", restore on OFF) should kill the native
pin without touching files. Scope when picked up: which iconIds to suppress = exactly the ones our
categories re-draw; keep ERR's own custom pins (boss/camp) untouched. **NB (2026-07-02): do this
AFTER the parity branch above merges — the suppression set is now the Group 1 + parity iconId
union (see the audit table in `docs/memory/features/mapgenie-landmark-categories.md`).**

## Group-2 Elevator MECHANISM SOLVED on Linux (2026-07-02) — implementation open

Full RE chain in `docs/re/linux_group2_prompt_binding_re_findings.md` (done entirely with in-DLL
probes + offline python — first end-to-end run of the Linux RE path). Net: recon's 5010 anchor was
LADDERS; real lever-lifts = ABP 8200-8501 "Pull/Push lever" -> ~55 ObjActParam rows (join col
+0x28) -> **MSB ObjAct EVENT section binds {asset entity, objActParamId}** => Elevator category is
a pure mod-agnostic disk parse (msbe: parse ObjAct events, filter param ids whose ABP text is a
lever, join asset position; refine by anim-id groups if gates over-capture). Smithing Table
SOLVED too (2026-07-02): [ASSETRADAR]+[ASSETCOUNT] -> model filter **AEG099_308**.
**BOTH IMPLEMENTED + MERGED to master + in-game verified (user, 2026-07-02)**: 54 Elevator markers
(AEG027_* lift family, positions near-perfect), 4 Smithing Tables. Branch
feat/mapgenie-group2-elevator-smithing merged; worktree cleaned. Group 2 remaining per the recon
doc: whatever the next MapGenie diff lists (see coverage_vs_mapgenie.py). Probes live in `src/goblin_param_scan.cpp`
([PARAMSCAN]/[EMEVDSCAN]/[ABPTEXT], debug_logging-gated).

## Linux runtime-RE path — investigate to stop the two-PC switch (2026-07-01, not started)

User pain: runtime RE is Windows-by-convention but the live game runs on the Linux box (Proton).
Options + trial plan in `docs/memory/tooling/linux-runtime-re-options.md` (default = in-DLL probes,
first trial = ceserver + CE GUI on the Proton pid). Related vision note (runtime modding framework,
NOT a plan): `docs/runtime_modding_framework_vision.md`.

## MapGenie category coverage — GROUP 1 MERGED; GROUP 2 (Portal) RE in progress (2026-07-01)

**GROUP 1 landmarks — MERGED to master + ERR in-game confirmed.** 6 `World -
DivineTowers/Evergaols/MinorErdtrees/GrandLifts/Dungeons/LegacyDungeons` built LIVE from
`WorldMapPointParam.iconId` (`build_live_landmarks()`, `src/worldmap/map_entry_layer.cpp`). `[LANDMARKLIVE]
built 114` (counts match off-disk), positions correct. Ghost = existing `WorldHostileNPC` (no work). All
default OFF. Circle-glyph followup outstanding (task chip spawned; see memory note).

**Miquella's Cross — added (branch `feat/mapgenie-group2-portal`).** DLC iconId 208 (13 rows) reuses the
same landmark pass — one enum entry + iconId branch. Built clean, deployed to `dll/offline/`. Restart ERR
+ grep `[LANDMARKLIVE]` (should now show `MiquellaCross 13`). Default OFF.

**Miquella's Cross + GROUP 1 landmarks — MERGED to master** (`5febe5f`).

**Portal (Group 2) — IMPLEMENTED on branch `feat/mapgenie-portal`, build-clean + deployed, NOT yet
in-game verified.** RE fully solved (`docs/re/windows_portal_aeg_re_findings.md`): a portal = an
**`AEG099_510`** sending-gate asset whose EntityID is bound as **arg[2] of EMEVD warp template
`90005605`** (the mod-agnostic "actually warps" signal; isolates ~23 real gates from the model's ~180
placements). Runtime pass: `msbe::parse_emevd_portal_gates` harvests the gate entity set from
`event/*.emevd` (shared with `load_emevd_world_feature_flags`), `build_disk_portal_markers` emits each
`AEG099_510` disk asset (aegRow 99510) in that set, dedup by entity. Label = PlaceName 6108700 "Sending
Gate". Default OFF.

**Next, in order:**
1. **In-game verify Portal (user).** Restart ERR, toggle `World - Portals`; grep `[LOOTDISK] ... Portal
   markers (AEG099_510 bound to warp template 90005605; N entities harvested, M LOD-dup collapsed)` —
   expect ~23 gates at real sending-gate spots (Four Belfries, Siofra, Leyndell, DLC, …). On pass, merge
   `feat/mapgenie-portal` to master.
2. **Landmark GLYPHS followup (user "for later").** Circle now; each WMPP row has a real iconId →
   `map_point_rect(iconId)` (`SB_MapCursor`). Quick win: `category_gpu_iconId` for the 4 single-value
   categories (DivineTower→23/Evergaol→9/MinorErdtree→30/GrandLift→21). Dungeon/LegacyDungeon/MiquellaCross
   need per-marker source iconId through `push_marker` (bosses share this gap). Task chip spawned.
   Portals could reuse `AEG099_510`'s SB_MapCursor glyph too if one exists.
3. **Rest of GROUP 2 — recon done, NOT quick wins. See `docs/re/windows_group2_landscape_re_findings.md`.**
   Portal was the clean one *because* it had a harvestable EMEVD template (`90005605`). The rest do NOT:
   - **Elevator / Smithing Table are ObjAct-bound, not EMEVD-template-bound.** Anchors found:
     Smithing Table = ActionButtonText 7030 / ActionButtonParam **6250**; Elevator = "Descend" 3301 /
     ActionButtonParam **5010**. But those ABP ids do NOT co-occur with their assets in EMEVD args
     (`_probe_g2_actionbtn.py`), and no candidate AEG model matches the counts (`AEG099_630` = 235 broad
     placements, not 40 lifts). Next step = an **ObjActParam/AssetObjActParam** parse (ObjAct row whose
     button=5010/6250 → MSB assets carrying that ObjAct), a new param path per category — bigger than Portal.
   - **Hidden Passage** = hit-detected illusory walls, NO action button → no static signal (hardest).
   - **Wandering Mausoleum** = dynamic moving entity (hard). Martyr Effigy = already `WorldSummoningPools`;
     Dragon Shrine folds into Churches; Landmark(172) = editorial → skip.
   Recon artifacts: `tools/_probe_g2_templates.py` (template→model map), `_probe_g2_actionbtn.py`.
   NB: offline SoulsFormats probes now need temp files in the REPO dir (`os.path.abspath('.')`), not
   `%TEMP%` — Defender started denying `%TEMP%` writes mid-session (`WinError 5`).
4. **DONE — Farmable.** `WorldFarmableCollectible` ("Loot - Farmable Drops") shipped + ERR-verified
   (`[LOOTDISK] … 70 farmable-notable`): respawning enemy drops of notable mats (Smithing Stones /
   Golden Runes / Gloveworts), all-8-slots scan (notable item is in slot 2), ~70 markers, off by default.
   `WorldFarmableEnemy` DROPPED (floods, no boss filter). Tuning knobs (notable set / per-item icons /
   dedup granularity) documented in `docs/memory/features/mapgenie-landmark-categories.md`.

## RESUME HERE (2026-07-01z9) — overlay_hot_reload_playwright_plan Slice C nearly done, only LoadLibrary mechanism left

Phase 1 and Phase 2 Slices A + B are COMPLETE + MERGED. Slice C (the consolidated
`goblin::overlay_api::*` wrapper layer covering ~110+ cross-DLL call sites — config/ui/
worldmap_probe/markers/kindling/collected/debug_events/input/disk_loot/`native_item_icon` family
— plus rewiring all 6 render-side files to use it) is DONE, IN-GAME CONFIRMED, MERGED. Full
design/audit history + the "read the real declaration, don't guess from a name" lesson from
several grep→compile-error→fix passes are in `docs/plans/overlay_hot_reload_playwright_plan.md`
(don't re-derive any of it — audit is complete and correct as merged). One real merge conflict
with the parallel name-aliases-runtime/data-purge session was hit and resolved along the way
(their `lookup_name_alias_en_utf8` retirement in favor of `lookup_name_en_disk_utf8` collided with
this session's wrapper rename — kept both, confirmed working in-game after).

**Only remaining Slice C piece: the actual `LoadLibrary`/`GetProcAddress` vtable mechanism** for
the host→render call direction (`draw_panel`/`draw_worldmap_markers`/`draw_minimap_hud`) — genuinely
new work, not started: needs `extern "C"` stable-name exports for the 3 draw functions, a
function-pointer table the host resolves at `LoadLibrary` time, the actual two-target CMake build
when `GOBLIN_OVERLAY_HOTRELOAD=ON` (currently a scaffold that always builds one DLL), plus the
ImGui-context-sharing/threading risks the plan already flags. Needs its own design pass before
coding — full detail + the risk list in the plan doc's Slice C/Phase 2 sections. Then Slice D
(file-watcher + real reload) can start.

## ⚠️ IN PROGRESS — baked-data → runtime/disk migration (build_pipeline.py deletion is the END state)

Authoritative plan: **`docs/plans/baked_data_full_removal_plan.md`** (full inventory + 6-phase
sequencing). `build_pipeline.py` can NOT be deleted yet — it's the LAST step (Phase 5), not the
first: it still generates authored tables with no runtime source (`category_exceptions`,
`name_aliases`, `world_feature_models`, boss list, region tables), and `build.bat` calls it 3×.

**Landed so far:** Phase 1 (dead `goblin_enemy_names` bake removed, enemy-drop labels now resolve
mod-agnostic via `NpcParam.nameId → GetMessage`) — merged, deployed, in-game verified on ERR. Phase
A regen (all 4 profiles now `MAP_ENTRY_COUNT 0`, non-ERR DLLs rebuild clean via clang/ninja) — done
at the build level; **the in-game vanilla sanity-check is still the one open item** (mod-agnosticism
can only be proven in-game on a non-ERR install — deploy `build-vanilla/MapForGoblins.dll` to a
vanilla install to close this). `.MASSEDIT` chain proven fully dead (not just baked=0) — the 14
dead `generate_*_massedit` stages + `goblin_massedit.{cpp,hpp}` (orphaned, never compiled) deleted.
`item_icon_table.json`'s baked category-exception override deleted + recovered LIVE via
`EquipParamGoods.sortId` (`goods_sort_id()`, `+0x20`, cross-checked vs `goodsType@0x3e`) —
in-game verified on ERR (every split repopulated correctly). `grace_position_index` bake dropped
(was already offline-only/dead-to-DLL, no in-game check needed).

**Remaining follow-ups (not blocking, note for whoever does Phase 5):** Reforged item families +
a few DLC key items (goods ids 2008025-2008037) still fall to the "Loot - Crafting Materials"
catch-all (colliding in-cell sortIds) — need dedicated `sortId` rules or accept the catch-all. The
offline mirror (`tools/taxonomy_classifier.py`, `_validate_taxonomy_map.py`) still applies the
deleted exceptions — resync or retire. `generate_loot_massedit` still emits a now-unread
`.MASSEDIT` alongside its live JSON — drop that emission when convenient.
**DONE 2026-07-01:** the tracked dead `.MASSEDIT` artifacts (`data/massedit` + `data/massedit_generated`,
113 files / ~9.9 MB) were purged + gitignored — the chain is proven dead (no runtime/pipeline consumer;
category-test tools read `regulation.bin`/params + `items_database.json`, not `.MASSEDIT`, so this is
safe for the pending MapGenie-coverage work). `generate_loot_massedit` still (re)writes
`data/massedit_generated` locally each run — now ignored, not tracked.

`goblin_name_aliases_en` migrated + bake DELETED (2026-07-01, `feat/name-aliases-runtime`): F1 English
search aliases now resolve live from the active install's `msg/engus/*.msgbnd.dcx` off disk
(`src/worldmap/name_fmg_en.cpp`; two-pass loose-mod-wins / packed-vanilla-fills; FMG-v2 group-table
lookup keyed on marker `name_id`), replacing the ERR-frozen table. Offline 2754/2756 vs the old bake;
**IN-GAME verified on ERR (cross-language) AND vanilla** via the me3 CLI (`[NAMEEN] 9877` vanilla names,
≠ ERR's count → reads the active install — see `docs/memory/tooling/me3-cli-nonerr-launch.md`).

**Next session — pick the next baked artifact to eliminate** (easiest→hardest, per the plan's
inventory): `goblin_tile_tabs`/`goblin_major_regions` (real + identical on all 4 profiles → dedup into
`generated_shared/`, pure housekeeping); `goblin_region_anchors`/`goblin_name_regions` (assess vs
`WorldMapPointParam`+`WorldMapPlaceName`); the icon atlas (baked overlay atlas, the prime-directive
example, biggest remaining item — see "Baked-atlas removal" below for why it's deferred). The regen
pipeline + all 4 profile builds are runnable on the Windows box (clang/ninja), so the confirm loop
is doable there.

## Baked-atlas removal — audited, DEFERRED until native coverage widens

Gate before deleting the baked overlay atlas: prove which categories actually need it per mod. The
`[ICONTIER]` census tool (`map_renderer.cpp`) tags each icon's resolve tier (mp_name/mp_id/item/
rep/atlas/circle) and tallies per draw pass — kept in the tree for re-auditing. Last audit (ERR vs.
vanilla, unioned since the census is per-view): **~15 categories still depend on the baked atlas**
(vanilla: Hostile NPC, Spirit Springs, Stakes of Marika, Kindling Spirits, Interactables, Spiritspring
Hawks; ERR: Cookbooks, Crystal Tears, Consumables, Scadutree Fragments, Pots-n-Perfumes,
Bell-Bearings, Crafting Materials, Golden Runes Low, Rune Arcs; `World - Maps` hits `circle`, no
glyph anywhere) — **verdict: gate NOT passed, keep the atlas.** To re-run: open the map ~5s on ERR
and on a vanilla launch (`internals/modengine/./bin/me3 launch -g eldenring -e "<steam exe>" -p
vanilla.me3`), grep `[ICONTIER]` both times, diff — native/rep in ERR but atlas/circle in vanilla is
what removing the atlas would regress. Follow-up not yet investigated: several loot categories that
hit `atlas` instead of `item`/`rep` are lot-backed treasures where per-item resolution runs but
yields `item_icon_id=0` — a one-shot debug log in `push_marker` dumping `lotId`/`lotType` + the
resolved key would disambiguate "fundamentally iconId=0" from "a fixable lot-resolution gap".

## OPEN — deferred for later

- **Lag-spike hunt, real suspect `refresh.collected.*`.** `refresh.collected.read_wgm` shows spikes
  2-5ms (~30x its avg) in the `[SPIKE]` log despite supposedly already using a good lookup — not yet
  root-caused. Use the `[SPIKE]` warns + the bench spikes column to localize a hidden per-marker/
  per-frame O(n) cost or cache miss. (Cosmetic nit noticed along the way: the spike-ratio display
  divides by a near-zero baseline for quiet timers, e.g. "~600x its 0.01ms avg" — harmless but ugly,
  floor the avg in the ratio display whenever touching that code.)
- **Map-exit input softlock.** Root cause for the general "soft key lock at screen edge" turned out
  to be **external** — Deskflow (cursor-sharing KVM), not ER or this mod; fix is Deskflow-side, see
  `docs/re/windows_input_softlock_re_prompt.md`. The F1-mouse-dead half of the original report was a
  separate, already-fixed bug (see the Alt+Tab fix chain in `changelog.md`). Low priority to revisit.
- **Item-name localization (FR/EN) — DONE 2026-07-01, the bake is gone.** (Superseded the earlier
  "keep the bake" call.) The cross-language English alias no longer ships baked: it's read live from
  the active install's engus msgbnd off disk at init (`name_fmg_en.cpp`). The feared ~10 MB Oodle cost
  is a one-time init decompress (~1.5 MB retained index), not per-frame — acceptable, and it's the only
  mod-agnostic option (baked shipped ERR's names into every profile). See the baked-data section above.
- **Is non-ERR/vanilla a hard support target?** Still an open policy question — decides whether the
  baked atlas (and similar ERR-leaning bakes) can eventually go fully or must stay as a permanent
  vanilla-compat net.

## Known standing gotcha — double-DLL-load, not a code bug

If both `MapForGoblins.dll` (ERR build) and `MapForGoblins_vanilla.dll` are present in the mods
folder, BOTH load into the same process → doubled everything (double ImGui draw, double
MsgRepository PlaceName patch producing `?PlaceName?`, double hook installs, discovered-grace
markers appearing to vanish) — confirmed by log diff, not a real code bug in any of the affected
systems. **Fix (immediate):** only ship/deploy ONE DLL — the launcher should load `MapForGoblins.dll`
only (renaming it does NOT disable the mod; the launcher falls back to the stale `_vanilla.dll` — to
truly test mod-off, remove BOTH). **Fix (strategic): DONE 2026-07-02 — the single-DLL migration
retired the per-profile variants; new packages ship one `MapForGoblins.dll` for every install, so a
fresh install can no longer double-load. (Stale old `_vanilla.dll` files in EXISTING installs can
still double-load until removed — the named-mutex hardening below still has value.)** **Hardening TODO, not yet implemented:** a named-mutex check at init
(`CreateMutexW`) so a second instance bails before installing any hooks/ImGui/PlaceName patch and
shows a clear on-screen "double load detected, check your launcher config" banner instead of silent
double-draw corruption — currently the failure mode is confusing, not caught.

## Key findings / non-obvious facts (icon/glyph work)

- The 7 mod-added POI categories (Spirit Springs / Summoning Pools / Stakes / Material Nodes / Bell
  Bearings / Interactables / Spiritspring Hawks) have NO ERR-custom glyph — their massedit iconIds
  (374+) point to glyphs absent from all current menu files (numeric glyphs cap at 261). Recover via
  a real `SB_MapCursor` glyph where one fits (e.g. summon→89), else circle.
- `MENU_MAP_ERR_*` (boss/grace) are ERR-only names; on non-ERR they won't resolve → circle if the
  baked fallback is ever removed.
- Offline KRAK decompress works on Linux via `internals/launcher/liboo2corelinux64.so.9`.
- Extracted glyph sheets (gitignored scratch): `tools/extracted/*.png` — regenerate via
  `bash tools/build_menu_tex_extract.sh && ./tools/menu_tex_extract`.
