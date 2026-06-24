# Loot-from-disk-MSB — test status (runtime vs static, Windows/Linux)

Status of the "loot markers from the active mod's real `map/MapStudio/*.msb.dcx`
files" feature (config `loot_from_disk_msb`). Updated 2026-06-24, branch
`feat/msbe-disk-parser`. See `docs/re/windows_msbe_dummyasset_unreachable_re_findings.md`
and the [[handoff-loot-from-real-files]] memory for the full RE.

## Legend
- ✅ done / verified · ❌ not done · ◑ feasible but not exercised on that platform
- **Runtime** = in the running game (Elden Ring is a Windows binary → runtime Linux = N/A).
- **Static** = offline: decompress + parse the MSBs + measure coverage, no game.

## Test matrix

| Item | Tested **runtime** (in-game) | Tested **static** (offline, not runtime) | **Windows** | **Linux** |
|---|---|---|---|---|
| **ERR** profile (full disk loot) | ✅ in-game: 3235 replaced, 0 KRAK skipped, no first-open hitch | ✅ | ✅ (game + DLL build) | ◑ parser/build.sh portable, not run |
| **ERTE** profile | ◑ DLL built, ready (not yet run in-game) | ✅ 458 maps / 3320 asset-lots | ✅ (offline probe + DLL build) | ◑ portable |
| **Convergence** profile | ◑ DLL built, ready (not yet run in-game) | ✅ 468 maps / 3623 asset-lots | ✅ (offline probe + DLL build) | ◑ portable |
| **Vanilla** profile | ◑ DLL built, ready (not yet run in-game) | ✅ 949 maps / 3193 asset-lots | ✅ (offline probe + DLL build) | ◑ portable |
| **DFLT** decompress (zlib / stb) | ✅ (ERR in-game) | ✅ | ✅ | ✅ (stb in-tree, `tools/msbe_test/build.sh`) |
| **KRAK** decompress (Oodle / oo2core) | ✅ (ERR in-game, 0 skipped) | ✅ (oo2core via ctypes, 4 profiles) | ✅ | ◑ via Wine + oo2core (`build_oodle.sh`), not run here |
| **DummyAsset filter** (`PART +0x0c`) | ✅ (178 → 21 in-game) | ✅ (487 maps) | ✅ | ✅ |
| **recover-later** tracking (3 lots) | ✅ (in-game) | ✅ | ✅ | ✅ |
| **pre-build at init** (no map-open hitch) | ✅ (in-game) | n/a | ✅ | ◑ |

## Offline coverage (with Oodle, all `_00` maps, 0 failures)
| profile | `_00` maps | parsed | asset-lots | DummyAsset |
|---|---|---|---|---|
| ERR | 651 | 651 | 3361 | 319 |
| ERTE | 458 | 458 | 3320 | 307 |
| Convergence | 468 | 468 | 3623 | 312 |
| Vanilla | 949 | 949 | 3193 | 315 |

## Non-ERR DLL build — RESOLVED (empty ERR-only stubs)
The data pipeline bakes 4 profiles (`src/generated_<profile>/`) but does NOT emit the
ERR-only tables (`goblin_quest_gates`, `goblin_quest_steps`, `goblin_region_anchors`,
`goblin_name_regions`, `goblin_model_aliases`), which the DLL references
unconditionally → a non-err DLL used to fail at configure ("No SOURCES given").
Fix: `tools/gen_nonerr_stubs.py <profile>` writes EMPTY versions (COUNT=0) of those
files, making every consumer a no-op (those features are simply absent off-ERR).
`build_pipeline.py` now runs it automatically at the end of a non-err run, so it
survives a clean regen. The loot path never touches those symbols, so this does NOT
affect loot fidelity — the loot bake (`goblin_item_icons` / `goblin_map_data`) is
real per-profile. **All 4 profile DLLs now build** (build-clang / build-erte /
build-convergence / build-vanilla). In-game runs of the 3 non-err DLLs are the only
step left (deploy each to its mod + set loot_msb_dir + open the map).

## Regeneration status (this session)
- ERR: committed bake (current).
- ERTE: regenerated 2026-06-24 (449s).
- Convergence: regenerated 2026-06-24 (421s).
- Vanilla: regenerated 2026-06-24 (330s).
- All 3 non-err dirs have 15 files (vs ERR's 25) — confirms the ERR-only generated
  files (quest_gates/quest_steps/region_anchors/name_regions/model_aliases) are
  absent off-ERR, i.e. the non-err DLL build blocker stands.
