# Plan — remove the per-profile map-data bake (`generated_*`)

Status: not started. Supersedes the "migrate vanilla/ERTE/Convergence onto live DiskMSB" framing in
`docs/memory/process/one-dll-externalize-mapdata.md` — that framing is **stale**, see Finding below.

## Why this plan exists

User asked (2026-06-30) to fully remove `generated_*`. Investigation before writing this plan found the
premise was wrong: it is **not** all removable, and **most of what looked unmigrated already isn't**.
Don't re-derive this — read this section first.

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
| `goblin_region_anchors.cpp`, `goblin_quest_steps.cpp`, `goblin_model_aliases.cpp`, `goblin_tile_tabs.cpp`, `goblin_quest_gates.cpp`, `goblin_major_regions.cpp`, `goblin_name_regions.cpp` | differs from non-ERR | identical across vanilla/erte/convergence | same | same | Real per-mod-family content (ERR's extra regions/dungeons vs vanilla layout) — **not removable**, but the non-ERR three are identical to each other, so a "non-ERR" shared bucket would dedup 3× → 1×. Low priority (files are 0.4–2.5 KB each; this is a maintainability win, not a size win). |

**Conclusion: "fully remove `generated_*`" is not the right end-state.** Only the marker-position bake
(`goblin_map_data.cpp`/`.hpp`) and its 3-4 dead consumer call sites are fully removable. Everything else in
`generated_*` is MapForGoblins' own authored data (localization, region anchors, quest text, UI labels) —
not derived from the active mod's files, so there's nothing on disk to read live instead. The
`GENERATED_SUBDIR` CMake mechanism stays; it just stops carrying marker-position data.

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
   stays for the tables in the table above).
2. Move `goblin_category_exceptions.cpp` + `goblin_name_aliases_en.cpp` into `generated_shared/` too
   (confirmed ERR==vanilla; confirm ==erte==convergence once Phase A regenerates them).
3. (Optional, low priority) dedup the non-ERR-identical tables (region_anchors/quest_steps/model_aliases/
   tile_tabs/quest_gates/major_regions/name_regions) into a "non-ERR shared" bucket if it's not more
   complexity than it's worth — these are tiny files, skip unless touching this area for another reason.

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
