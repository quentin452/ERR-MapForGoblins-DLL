# Plan — remove the per-profile map-data bake (`generated_*`)

Status: not started. Supersedes the "migrate vanilla/ERTE/Convergence onto live DiskMSB" framing in
`docs/memory/process/one-dll-externalize-mapdata.md` — that framing is **stale**, see Finding below.

## Why this plan exists

User asked (2026-06-30) to fully remove `generated_*`. Investigation before writing this plan found the
original framing was wrong twice over — first that vanilla/ERTE/Convergence needed a live-DiskMSB
migration (they don't, see Finding below), then a second correction below: most of the *other* baked
tables already use the exact same "ERR-only feature, auto-stub elsewhere" pattern as the marker-position
bake. Don't re-derive this — read this section first.

## Finding (verified against the actual files + `tools/generate_data.py`, not the stale memory note)

`src/generated_vanilla/`, `src/generated_erte/`, `src/generated_convergence/` are **gitignored, untracked,
local-only build artifacts** (`.gitignore:11-23`) — nothing here was ever committed; "removal" doesn't
touch git history, only local disk + the build mechanism.

Per-file reality (sizes verified 2026-06-30):

| File | ERR | vanilla | erte | convergence | Verdict |
|---|---|---|---|---|---|
| `goblin_map_data.cpp` (marker positions) | 644 B stub | 644 B stub | 3.2 MB **stale real bake** | 3.0 MB **stale real bake** | `tools/generate_data.py`'s `generate_map_data_cpp` is **already unconditional** — it emits the 0-length stub for every profile, no per-profile branch. ERR and vanilla already prove this. erte/convergence's large files are leftover from before that change and are also each **missing** `category_exceptions`/`name_aliases_en`/`world_feature_models` (added later) — i.e. those two dirs are just out of date, not "not yet migrated." A regen replaces them with the same stub. |
| `goblin_enemy_names.cpp` | 627 B stub | 370 KB real | 370 KB real | 370 KB real | Byte-identical across vanilla/erte/convergence. Genuinely can't go live: non-ERR mods have no equivalent of ERR's Codex (the in-game source ERR's runtime path reads), so this is MapForGoblins' own authored localization table, not a bake of mod content. **Stays compiled. Not removable** — but the 3× duplication (vanilla==erte==convergence) is a dedup opportunity. |
| `goblin_category_exceptions.cpp`, `goblin_name_aliases_en.cpp` | identical to vanilla | — | missing (stale) | missing (stale) | ERR==vanilla byte-identical. Authored content, not mod-derived. **Stays compiled, dedup into `generated_shared/`.** |
| `goblin_quest_gates.cpp`, `goblin_quest_steps.cpp`, `goblin_region_anchors.cpp`, `goblin_name_regions.cpp`, `goblin_model_aliases.cpp` | real (3.4–44 KB, ERR Codex quest browser + ERR's reworked regions) | **388–398 B stub** | missing (stale — see below) | missing (stale) | **CORRECTION (caught by user pushback, re-verified):** these are explicitly **ERR-only features** per `tools/gen_nonerr_stubs.py`'s own docstring ("non-err gates/steps/browser, region... NOT goblin_inject/map_renderer/goblin_overlay" — i.e. these never feed the live marker/render path on non-ERR). `gen_nonerr_stubs.py` already writes the empty stub for any profile missing the file, and `build_pipeline.py:519` already calls it for non-ERR profiles. Verified: vanilla's copies ARE the stub already. erte/convergence are simply **missing** these files (not "real data not yet migrated") because their local `generated_*/` predates this script — same staleness as `goblin_map_data.cpp`. **Already removable for non-ERR; Phase A's regen does it automatically, zero extra code.** For ERR itself these stay (real, used feature, additive-layer-on-mod-agnostic-base is the documented acceptable pattern per `AGENTS.md`). |
| `goblin_tile_tabs.cpp`, `goblin_major_regions.cpp` | ~2.4 KB / ~0.9 KB, real | identical to ERR | identical | identical | Genuinely profile-independent (base-game world-map tab layout + major-region list, not mod-specific) and real on **every** profile, including ERR — not a stub anywhere. Small (≤2.5 KB), duplicated 4× as a build artifact. Dedup into `generated_shared/` is pure housekeeping, not a removal. |
| `goblin_enemy_names.cpp`, `goblin_category_exceptions.cpp`, `goblin_name_aliases_en.cpp` | stub / identical-to-vanilla / identical-to-vanilla | real, identical across the 3 non-ERR profiles | same | same | The one category that's genuinely authored, not MSB/regulation-derived (enemy_names: non-ERR mods have no Codex-equivalent live source; the other two are curated text-mapping dicts in `generate_data.py`, no param/MSB source at all). **Stays compiled** — dedup the 3× non-ERR duplication into `generated_shared/`, nothing more. |

**Revised conclusion: almost everything in `generated_*` already follows the "ERR-only feature → auto-stub
elsewhere" pattern** (`gen_nonerr_stubs.py`), same as the marker-position bake. Phase A's regen alone closes
nearly the whole gap — the erte/convergence "still baked" appearance was staleness, not unmigrated code, for
*every* table, not just `goblin_map_data.cpp`. The only content that's genuinely permanent everywhere is
enemy_names/category_exceptions/name_aliases_en (no live source exists for it on non-ERR mods) plus
tile_tabs/major_regions (real but tiny and identical across all 4, pure dedup). The `GENERATED_SUBDIR`
mechanism stays — ERR still needs its own real quest/region tables — but it shrinks to carrying only
genuinely ERR-specific content once Phase B dedups the rest into `generated_shared/`.

## Phase A — regenerate + verify (closes the open "vanilla+DLC verify" HANDOFF item too)

Windows required (regulation decrypt / SoulsFormats, per `docs/memory/linux.md` codegen note).

1. `PYTHONPATH=tools py tools/build_pipeline.py --profile vanilla` (already a stub locally, but rerun to
   confirm reproducibility and pick up the newer generators it was missing nothing for — sanity check).
2. `PYTHONPATH=tools py tools/build_pipeline.py --profile erte` then `--profile convergence` — confirms
   `goblin_map_data.cpp` collapses from 3.2 MB / 3.0 MB stale real-bake down to the 644 B stub, and that
   the previously-missing `category_exceptions`/`name_aliases_en`/`world_feature_models` materialize.
3. `cmake --build build-vanilla|build-erte|build-convergence`, deploy each, smoke-test in-game:
   markers still render correctly (now purely from live DiskMSB / game memory, same path already proven
   on ERR) and DLC items resolve names via the native-GetMessage refactor (`a64f4e1`) — same pass also
   closes HANDOFF's open "vanilla+DLC verification" item, don't duplicate that test.
4. Record results in `docs/HANDOFF.md` / `docs/changelog.md`.

## Phase B — collapse what's now provably identical (only safe after Phase A confirms all 4 profiles agree)

1. Move `goblin_map_data.cpp`/`.hpp` (now a byte-identical stub on every profile) into `generated_shared/`;
   drop the per-profile `GEN_DIR` source entry in `CMakeLists.txt` for it specifically (the switch itself
   stays for ERR's genuinely-real tables).
2. Move `goblin_tile_tabs.cpp` + `goblin_major_regions.cpp` into `generated_shared/` (real + identical on
   all 4 profiles, confirmed).
3. Move `goblin_category_exceptions.cpp` + `goblin_name_aliases_en.cpp` into `generated_shared/`
   (confirmed ERR==vanilla; confirm ==erte==convergence once Phase A regenerates them).
4. `goblin_enemy_names.cpp` stays per-profile-bucketed but only needs 2 buckets, not 4: ERR (627 B stub)
   vs "non-ERR" (370 KB, identical across vanilla/erte/convergence) — fold the 3 non-ERR copies into one
   shared non-ERR table if `generated_shared/` gains an `MFG_VANILLA`-gated variant, or leave as-is if not
   worth the CMake complexity (pure dedup, no behavior change either way).
5. `quest_gates/quest_steps/region_anchors/name_regions/model_aliases` need **no Phase B work** — they're
   already correctly profile-gated by `gen_nonerr_stubs.py` (stub for non-ERR, real for ERR). Nothing to
   move; this row exists to record that it was checked, not skipped.

## Phase C — the actual full removal of marker-position machinery (end state)

Once Phase B lands, `MAP_ENTRY_COUNT == 0` unconditionally, for every profile, permanently — at that point
the consumer call sites that loop over `generated::MAP_ENTRIES` are dead code for real (not just for ERR):
`src/worldmap/map_entry_layer.cpp` (2 sites), `src/goblin_markers.cpp`, `src/goblin_overlay.cpp`,
`src/goblin_debug_events.cpp`. Delete those call sites + `goblin_map_data.{cpp,hpp}` entirely. This is the
point where "the stub file is misleading" (the complaint that started this plan) stops being true because
there's no stub left — just nothing.

Do **not** attempt Phase C before Phase A is verified on all 3 non-ERR profiles in-game — these call sites
are the only thing currently gating marker behavior to "do nothing" vs "do something" per profile; deleting
them blind would be removing an untested safety net, not just dead code.

**Phase C / `feat_quests_implementation_plan.md` overlap (checked 2026-07-01):** one of the dead
`MAP_ENTRIES` consumer sites Phase C deletes is `src/worldmap/map_entry_layer.cpp:2027`
(`if (e.category == gen::Category::WorldQuestNPC) continue;`). The quest plan's §0/§5 describe this as a
still-live "legacy `WorldQuestNPC` emission (~L1891)" that `QuestNpcLayer` must retire — that's now
**stale**: current master already dropped the emission (only this dead skip-rule + a `nullptr`-icon
`category_meta.cpp:87` entry remain; `map_renderer.cpp`'s `questNpcQuestAware` gate has nothing left to
gate). Whichever plan lands first should delete this line; the other should drop that step from its own
scope rather than re-touch it. Same overlap risk doesn't apply elsewhere in Phase C's file list.

## Cross-reference — `docs/plans/feat_quests_implementation_plan.md` (read 2026-07-01)

Don't duplicate that plan's already-scoped work here:
- `goblin_quest_steps.cpp`'s `title`/`desc`/`zone` fields are genuinely permanent (hand-authored prose,
  confirmed above) — but `progress_flag`/`entity_id` are **not** meant to stay hand-coded long-term. The
  quest plan's §2 already specifies deriving them from parsed MSB Enemy parts + EMEVD (DarkScript3
  decompile / the in-overlay event-flag hook), explicitly calling hand-authoring "a bootstrap exception"
  for a demo set only. That's this removal plan's Phase-D-shaped idea (live MSB/EMEVD instead of a static
  bake) — already owned by the quest plan, don't re-propose it here.
- `goblin_quest_gates.cpp` (the *visibility-gating* table, sourced from the external EldenRingQuestLog
  project + `npc_name_text_map.json` — separate from `goblin_quest_steps.cpp`) is **not mentioned** by the
  quest plan at all. Open question for whoever implements that plan: does `QuestNpcLayer`'s
  `progress_flag`-driven step logic make `quest_gates.cpp`'s gating redundant? If so it's a 6th
  permanently-baked table that becomes removable as a side effect of that plan, not this one. Flagging
  here so it isn't missed; not resolving it in this plan.
- `tools/generate_quest_npcs.py` (the OLD static per-profile NPC generator, distinct from
  `generate_quest_gates.py`) is already noted in the quest plan's Appendix as superseded by the runtime
  `QuestNpcLayer` — its hand-curated denylist is salvaged there. Nothing for this plan to do.
