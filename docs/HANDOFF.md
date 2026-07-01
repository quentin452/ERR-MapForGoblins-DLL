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

Last updated: 2026-07-01z9 (overlay_hot_reload_playwright_plan Phase 2 Slices A/B/C all MERGED to
`master` except Slice C's `LoadLibrary` mechanism — see below).

## MapGenie category coverage — GROUP 1 MERGED; GROUP 2 (Portal) RE in progress (2026-07-01)

**GROUP 1 landmarks — MERGED to master + ERR in-game confirmed.** 6 `World -
DivineTowers/Evergaols/MinorErdtrees/GrandLifts/Dungeons/LegacyDungeons` built LIVE from
`WorldMapPointParam.iconId` (`build_live_landmarks()`, `src/worldmap/map_entry_layer.cpp`). `[LANDMARKLIVE]
built 114` (counts match off-disk), positions correct. Ghost = existing `WorldHostileNPC` (no work). All
default OFF. Circle-glyph followup outstanding (task chip spawned; see memory note).

**Miquella's Cross — added (branch `feat/mapgenie-group2-portal`).** DLC iconId 208 (13 rows) reuses the
same landmark pass — one enum entry + iconId branch. Built clean, deployed to `dll/offline/`. Restart ERR
+ grep `[LANDMARKLIVE]` (should now show `MiquellaCross 13`). Default OFF.

**Portal (39) — RE done, NOT a clean add. See `docs/re/windows_portal_aeg_re_findings.md`.** Model
IDENTIFIED: **`AEG099_510`** = the sending-gate asset (WMPP "Sending Gate" row sits 0.4u on it). BUT it
has 180 placements (all entity-bearing): only ~13 are map-scripted interactive gates; ~148 are a shared
`10456xxxx` range dominated by a 94-member decorative cluster in Leyndell. `entity_required` does NOT
filter it. Isolating the ~39 real portals needs an **EMEVD warp-template join** (find the sending-gate
warp event's entity arg — same shape as the existing `seal_emevd`/`hero_tomb_emevd` joins). NOT wired yet.

**Next, in order:**
1. **Merge Miquella's Cross + decide Portal depth.** Miquella's Cross is a clean, buildable add — merge
   `feat/mapgenie-group2-portal` once its `[LANDMARKLIVE] MiquellaCross 13` is confirmed in-game. For
   Portal, pick: (a) do the EMEVD warp-template scan (`tools/datamine_emevd_*.py` style) to isolate the
   ~39, or (b) pivot to an easier Group-2 category first (Smithing Table = 1 AEG model; Elevator = AEG
   lift models) and return to Portal with the EMEVD tooling. Model fact `AEG099_510` is solid either way.
2. **Landmark GLYPHS followup (user "for later").** Circle now; each WMPP row has a real iconId →
   `map_point_rect(iconId)` (`SB_MapCursor`). Quick win: `category_gpu_iconId` for the 4 single-value
   categories (DivineTower→23/Evergaol→9/MinorErdtree→30/GrandLift→21). Dungeon/LegacyDungeon/MiquellaCross
   need per-marker source iconId through `push_marker` (bosses share this gap). Task chip spawned.
3. **Rest of GROUP 2** — Smithing Table (1, one AEG model), Elevator (40, AEG lift models), Hidden Passage
   (59, illusory walls — likely EMEVD/hit, hard), Wandering Mausoleum (dynamic, hard). Martyr Effigy =
   already `WorldSummoningPools`; Dragon Shrine folds into Churches; Landmark(172) = editorial → skip.
4. **Deferred (user) — the 2 Farmable categories** (`WorldFarmableEnemy` + `WorldFarmableCollectible`).
   Real design forks. Full rationale in `docs/plans/mapgenie_category_coverage_plan.md`.

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
truly test mod-off, remove BOTH). **Fix (strategic):** true mod-agnosticism (this mod's whole prime
directive) makes the per-mod build split unnecessary — one DLL for every mod means no variant, no
double-load possible. **Hardening TODO, not yet implemented:** a named-mutex check at init
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
