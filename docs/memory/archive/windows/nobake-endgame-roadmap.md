---
name: nobake-endgame-roadmap
description: The 3-phase endgame for the no-bake migration — drive baked→0, delete the static bake, then refactor to the "industrial" offset-free approach (paramdef-driven + format-parser + RTTI/AOB).
metadata:
  node_type: memory
  type: project
---

**<user>'s endgame plan (2026-06-26), confirmed + refined.** Three phases, in order:

**PHASE 1 — drive baked → 0 — ✅ EFFECTIVELY DONE (baked 16, all on master 2026-06-27).** baked
8419→16; ALL clean loot+map+world-feature recovery levers EXHAUSTED ([[nobake-coverage-scoreboard]]).
The remaining **16 are positionless-IRREDUCIBLE** (15 lt=2 orphan-enemy no-NpcParam + 1 lt=1 m35
(0,0,0) fallback — verified by name via [BAKED-RESIDUAL] + the recovery oracle): NO world position
exists on disk, so they CANNOT be recovered — they vanish when the bake is deleted. So "baked→0" =
**Phase 2** now, not more recovery. (Diagnostic chain to confirm any regression: [BAKED-RESIDUAL] +
[COVERAGE]+flag-src + [SKIPPED], all runtime-fed + versioned.) **→ NEXT SESSION STARTS ON PHASE 2.**
Historical detail below (original Phase-1 backlog when this was written at baked 96):

The clean per-category levers are nearly done
(🔴 baked-only=0). What's LEFT to reach literal zero is the **"accepted" residual** — and per
[[residual-irreducible-strategy]] "accepted" = an OPEN question, not a wall:
- 4 Snow Town statues (AEG110_029) — needs a non-_00 ASSET scan (asset analogue of the emevd
  `load_lod_award_entities` LOD fix). +2 misc Interactables; Ammo 3 (EMEVD lots 20650/20810 →
  weapons, mis-classed).
- The 80 loot residual (29 emevd-scripted chests + 35 enemy mislabel + 16 treasure debake) — these
  are "accepted" only because no CLEAN signature isolates them yet. Reaching true zero needs either
  (a) RE the chest-event CONVENTION (what marks an EMEVD event as a notable chest award — the m60
  rule over-matched 395 because we used structure, not the convention), and the enemy/corpse
  delivery, OR (b) ship the small curated residual as a DISK-LOADED json (not the 18 MB static
  array) — that's "no static bake" even if a few positions stay hand-curated. Decide (a) vs (b) per
  category; (b) is the pragmatic floor.

**★★ PHASE 2 — DONE + MERGED to master 2026-06-27 (merge 4a7716d, --no-ff; all 5 stacked branches
deleted). UNPUSHED — <user> pushes (`git push origin master`).** Runtime-validated (baked=0, tooltips,
gather-gray) AND regen-validated (incremental pipeline: generate_data emits stub + intermediate,
geof_models byte-identical, location_overrides parses the intermediate; the location_alt wipe-to-0 is the
pre-existing task_3c9902d0 bug, reverted). DLL 6.19→3.76 MB. **NEXT SESSION = [[delete-generate-data-path]]:
eliminate the 3.67 MB `data/_map_entries_full.cpp` intermediate by removing its only 2 consumers — START
with location_alt→runtime/delete (no src consumer found), then the geof model-substitution RE. Original
in-progress detail (now historical):** Stacked branches off master: feat/phase2-kindling-nobake (883520e + label-fix 07aa3f1) →
-geom-graying (d9c5b22) → -fmg-nobake (d81391b) → diag(92f0317, overlay cursor-hook logging) →
feat/phase2-bake-stub (279714c). Steps 1-3 (kindling/geom/FMG) RUNTIME-VALIDATED in-game: gather-gray
confirmed on a crafting node ("briseur de doigt"), tooltips confirmed, kindling SFX-discovery confirmed
(kindling graying is its OWN SFX/event mechanism — EcTestDistance heap-scan — NOT the item/geom path, so
it's separate + fragile, not a geom-graying test). Step 4 = `goblin_map_data.cpp` replaced with an empty
table (COUNT=0, 1-elem dummy; .hpp types kept). **DLL shrank 6.19→3.76 MB** (the 3.7 MB static data is
gone). NEXT: <user> runs the stubbed-bake DLL → expect `[COVERAGE] TOTAL baked=0`, every marker still
present (disk/live), tooltips+graying intact, no crash → then merge the stack. FOLLOW-UPS (polish, not
blockers): (a) update tools/generate_data.py to emit the empty stub so a regen can't resurrect the 3.7 MB
bake (the .cpp is hand-stubbed now); (b) trim the dead consumer loops (the FMG walk, the collected seed,
the DEBAKE-GAP/RESIDUAL diags) that now iterate the empty table; (c) the overlay search-box de-focus bug
(latent, boot-time, intermittent — diag deployed: watch `[OVERLAY] user32!SetCursorPos/GetCursorPos HOOK
FAILED`; if a cursor hook fails, fix = feed ImGui mouse pos from WM_MOUSEMOVE, hook-independent). Original
detail below.

**PHASE 2 — bake cleanup.** Delete `src/generated/goblin_map_data.cpp` (the MAP_ENTRIES bake — actual
size ~3.7 MB, not 18). NOT just `rm` — first cut the remaining MAP_ENTRIES *dependencies*. The marker
SOURCE is already disk/live (baked≈1 proves it); cleanup = removing the last *infra* couplings.
**★ STARTED 2026-06-27 (branch feat/phase2-kindling-nobake, commit 883520e, UNPUSHED — <user>
runtime-tests + merges).** Full dependency map (verified this session): (1) **Kindling — ✅ DONE this
step.** Was the LAST category still SOURCING its marker from the bake (baked row + disk-pos override).
Now: map_entry_layer emits a marker per disk SFX region "KindlingSpirit_000N" directly; goblin_kindling
builds its 5-slot table from the fixed ERR constants (slots 1..5, entity 1045373501..505), NO MAP_ENTRIES
(dropped the goblin_map_data.hpp include). Tie = new public `goblin::kindling::region_row_id(name)` →
row_id = entity_id, shared by marker + slot so graying (is_row_collected, keyed by row_id) still works.
Baked twin dropped when disk placed it; baked-no-disk-twin re-keys to the same entity-id (zero-regression
when feature off). Built clang-cl OK + deployed. **Runtime check: `[LOOTDISK] kindling: 5 markers emitted
from disk SFX regions, 5 baked twins dropped`; the 5 still gray on pickup.** NOTE: the native-param hide
(register_param_ptr/remap_row_ids) is DEAD CODE (never called since native injection was deleted) — left
as-is. (2) **goblin_collected geom graying — ✅ DONE (branch feat/phase2-geom-graying, commit 8882836, stacked on
the kindling branch, UNPUSHED).** Was the real verrou: only the Rune/Ember PIECES emitted a synthetic geom
row_id (kRuntimeGeomRowBase) + `register_runtime_entries`; the GATHER nodes (Material Nodes + crafting/
consumable gather pickups) emitted with `row_id=lot` and never registered → their disk markers were NOT in
the collected tracking maps (g_tile_name_to_row[tile][name]→row_ids + g_entry_positions[row_id]) → they
could only gray off the MAP_ENTRIES seed (dying in Phase 2). The graying bridge: refresh() reads GEOF/WGM →
(tile,prefix,slot)/(tile,name) → row_ids → marks collected; a marker grays iff its row_id is in those maps.
FIX: the gather branch now mirrors the piece branch — synthetic rid + RuntimeEntry (gated on a known MSB part
name; WGM alive-match keys on it), lot stays the lotId arg. Additive/safe with the bake present (baked twin
already identity-dropped; seed double-tracks harmlessly). After delete, register_runtime_entries is the SOLE
geom-tracking source. NOTE: this likely also FIXES a latent gap (gather disk markers probably never grayed —
the visually-confirmed graying was PIECES, which were registered). Runtime check: gather/material gray on
collect + un-gray on respawn, no false mass-graying, pieces still gray; `[COLLECTED] merged ~2800` (was ~1400).
(3) **goblin_messages FMG preload — ✅ DONE (branch feat/phase2-fmg-nobake, commit 12b5bcf, stacked,
UNPUSHED).** The walk over baked MAP_ENTRIES textIds built the per-id preload sets; the NON-item label
families (NpcName 700M / ActionButtonText 800M / TutorialTitle 900M / BloodMsg 950M — boss/world-feature/
enemy-drop names) would show "?" once MAP_ENTRIES is empty (item families were already whole-namespace
preloaded via copy_fmg_all_layered when disk loot is on). FIX = preload those non-item families WHOLE-
namespace too (same copy_fmg_all_layered), unconditional (matches the previously-unconditional walk). ADDITIVE
+ SAFE: kept the walk + targeted copies as a harmless deduped subset (no risky surgery in the much-burned
"?PlaceName?" FMG area) — they become empty no-ops post-delete. EventTextForMap 600M stays unsupported (menu
bank). Runtime check: boss/seal/enemy/NPC tooltips show names; `Copied ALL N NpcName/TutorialTitle/...`.
**★ KEY INSIGHT (changes the whole endgame): the final delete = replace goblin_map_data.cpp with a STUB
`const MapEntry MAP_ENTRIES[]={}; const size_t MAP_ENTRY_COUNT=0;` (keep the .hpp types).** Then EVERY consumer
loop iterates an empty array — links fine, zero baked markers — so NO consumer code needs its MAP_ENTRIES
reference removed. Only behaviors that depend on the bake's DATA had to be cut first: graying-seed (✅ steps 1-2)
+ FMG names (✅ step 3). That's it. (4) **Diagnostics** (goblin_markers dump, goblin_debug_events, DEBAKE-GAP/
RESIDUAL diags) — NO CHANGE NEEDED: they iterate the empty stub → empty dumps, harmless. (5) **The ~1
positionless residual** (Golden Rune 35000580) — vanishes with the empty array. (6) **KEEP goblin_map_data.hpp**
(Category enum + MapEntry struct drive overlay layers/buckets). **REMAINING = step 4: swap the .cpp for the
stub (or keep the generator emitting an empty array), MAP_ENTRY_COUNT=0 sanity build + full runtime test, then
trim the dead walk/seed loops as polish.** Branches to merge in order: feat/phase2-kindling-nobake →
-geom-graying → -fmg-nobake (all stacked, <user> runtime-tests + merges --no-ff).

**PHASE 3 — the "industrial" ER method: kill the manual offsets.** <user>'s real goal — stop
hand-pinning byte offsets (the [[re-offset-validation]] pain: the isEnableRepick bit5/6 leak, the
736-row NpcParam, +0xb8, +0x30…). The manual offsets fall into 3 classes, each with a robust fix:
1. **PARAM field offsets → PARAMDEF-DRIVEN.** Every param offset (NpcParam.itemLotId_map@0x30,
   AssetEnvironmentGeometryParam.pickUpItemLotParamId@0xb8, EquipParamGoods.goodsType@0x3e,
   ItemLotParam rows, isEnableRepick bit) is 100% DERIVABLE from the applied PARAMDEF (field name →
   bit-packed offset). The OFFLINE pipeline already does this (`param_to_dict` reads by field NAME).
   The RUNTIME hardcodes them = the biggest + riskiest class. INDUSTRIAL FIX: load the paramdefs at
   runtime (shipped defs / regulation) and resolve field offsets by NAME → zero manual param offsets,
   version-proof. Kills most of the RE-offset-validation risk in one move.
2. **Disk file structures → FORMAT-SPEC PARSER.** MSB part+0x20 pos, EMEVD instruction layout, DCX
   headers — STABLE, documented formats (SoulsFormats is the canonical reader). The runtime already
   parses the DISK files (msbe_parser is disk-format-driven, NOT resident RAM — RAM is relocated +
   version-variant). Keep the disk path; widen to SoulsFormats-grade completeness as needed. This
   class is mostly already "industrial."
3. **Live memory offsets → RTTI + AOB SIGNATURE SCANNING.** The genuinely version-fragile ones
   (WorldChrMan, MapIns+0x460, ChrIns/EnemyIns vtables, player-pos chain, EcTestDistance vft). RE'd
   per ER patch today. INDUSTRIAL FIX: resolve structures by PATTERN, not static address — RTTI class
   lookup (the mod already has `tools/ghidra/rtti_index.txt` + `query.java`, see [[ghidra-re-tooling]])
   + AOB/signature scans (what mature ER tooling does). Pin the SIGNATURE once; it survives patches
   that move the address. This is the only class that can't be data-derived — minimize its surface.

**Net:** the "industrial ER method" = (1) paramdef-driven param access + (2) disk-format parsing +
(3) RTTI/AOB for the irreducible live structs. The mod ALREADY has all three ingredients (offline
paramdef reads, the disk msbe_parser, the Ghidra RTTI index) — Phase 3 is making them the DEFAULT
instead of committed offsets. Do Phase 3 AFTER 1+2 prove the migration end-to-end (it's the
"never hand-pin an offset again" refactor, not a prerequisite). See [[re-offset-validation]],
[[nobake-coverage-scoreboard]], [[runtime-msb-resident-plan]], [[ghidra-re-tooling]].
