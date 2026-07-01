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

Last updated: 2026-07-01z8 (overlay_hot_reload_playwright_plan Slice C export-API layer +
call-site rewiring both DONE, IN-GAME CONFIRMED, merged to `master` — including a real merge
conflict with the parallel name-aliases-runtime/data-purge session's push to `origin/master`,
resolved and re-confirmed in-game — see below).

## RESUME HERE (2026-07-01z7) — overlay_hot_reload_playwright_plan Slice C API layer + rewiring landed, LoadLibrary mechanism not started

Phase 1, Phase 2 Slice A, and Phase 2 Slice B are all COMPLETE + MERGED to `master` (Slice B:
draw layer extracted to `src/goblin_overlay_render.{cpp,hpp}`, grace/icon-SRV helpers stayed
host-side behind thin wrappers since they turned out D3D12-coupled — full detail in the plan doc).

**Slice C — export-API layer DONE, build-verified standalone (2026-07-01, `feat/overlay-render-api`,
not yet wired in, not yet merged).** A full export-surface audit found ~110 real cross-DLL call
sites (bigger than first estimated) spanning `goblin::config` (59 of 64 globals — 5 are `inline
constexpr`, already free), `goblin::ui`, `goblin::worldmap_probe`, ~40 bare `goblin::*` functions,
`markers`/`kindling`/`collected`/`debug_events`/`input`, and `worldmap::disk_loot_*` (host despite
the name). **User decision:** one consolidated wrapper file (`src/goblin_overlay_render_api.{hpp,cpp}`)
rather than annotating ~15 existing shared headers — keeps blast radius contained. Config globals
use an X-macro (45 mutable → pointer-getters, the rest read-only but same shape for uniformity);
`GraceCandidate`/`LiveGrace`/`RuntimeEntry`/`SigHealth`/`LiveView`/`LocateDebug`/`NpcQuest` structs
all confirmed already in shared headers (free, only the functions needed wrapping). Getting ~15 of
the bare-`goblin::*` signatures right took several grep→compile-error→fix passes — audit-derived
NAMES were right but several real signatures are far more complex than guessed (`marker_world_pos`
takes 9 params with `uint8_t`/`uint32_t` area/grid encodings, `quest_step_done` lives in a
different header entirely, `gpu_want_symbol`/`gpu_want_item` return `void` not `bool`, etc.) —
**lesson for whoever continues this: read the real declaration before writing a forward, don't
infer from a name.** This layer is dead code right now (not called from anywhere) — zero runtime
risk, build-verified only, no in-game check needed yet.

**Call-site rewiring — DONE, IN-GAME CONFIRMED, MERGED.** All 6 render-side files now call
`goblin::overlay_api::*` instead of `goblin::config::*`/`goblin::ui::*`/etc. directly (generated via
a ~180-rule per-symbol sed script, not a blanket namespace replace, to avoid rewriting TYPE
references like `goblin::worldmap::DiskLootState` that must stay untouched). Found + fixed one
export-audit gap while rewiring: `goblin::ui::section_label` was missing from the API entirely.
**IN-GAME CONFIRMED 2026-07-01 22:12**: `[SIG]` 29/29 clean, both grace SRVs built,
`render.minimap`/`refresh.collected`/`refresh.category_census`/`refresh.flag_or_pairs` all firing
correctly the whole session, no crash/error.

**Real merge conflict hit + resolved (2026-07-01, `git merge` with `origin/master`).** While
merging to `master`, hit an ACTUAL conflict with the parallel session's push: it deleted the baked
`goblin_name_aliases_en` table and retired `lookup_name_alias_en_utf8` in favor of a new
`goblin::lookup_name_en_disk_utf8` (runtime engus-FMG-off-disk lookup, `src/worldmap/name_fmg_en.{hpp,cpp}`,
host-side like `disk_loot_dir`/`disk_loot_state`) — collided directly with this session's rename of
the same call site to `goblin::overlay_api::lookup_name_alias_en_utf8`. Resolved by keeping BOTH:
their new lookup function + this session's `overlay_api::` wrapper convention (renamed the wrapper
to `lookup_name_en_disk_utf8`, updated `goblin_overlay_render_api.{hpp,cpp}` to match, added the
`worldmap/name_fmg_en.hpp` include). Their other changes (113 dead `.MASSEDIT` file deletions,
`name_fmg_en.*` additions, `dllmain.cpp`/`goblin_messages.*` edits) merged cleanly, untouched.
**IN-GAME RE-CONFIRMED 2026-07-01 22:22** after the merge: `[DVDBND]`/`[GAMEFILE]` show the engus
FMG files (item/menu msgbnd) actually loading from disk through the new wrapper chain, `[SIG]`
29/29 clean, no crash/error. Merge commit + everything above now on `master`.

**Still remaining for Slice C:** (1) `native_item_icon`-family reverse ctx/pointer table (host owns
`g_device` etc., doesn't change); (2) `LoadLibrary`+`GetProcAddress` vtable resolution for the
host→render call direction (`draw_panel`/`draw_worldmap_markers`/`draw_minimap_hud`). Next session:
start with (1) — full detail in the plan doc.

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
