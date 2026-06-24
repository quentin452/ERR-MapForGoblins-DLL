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
| **ERTE** profile | ❌ (non-err DLL build blocked) | ✅ 458 maps / 3320 asset-lots | ✅ (offline probe) | ◑ portable |
| **Convergence** profile | ❌ (non-err DLL build blocked) | ✅ 468 maps / 3623 asset-lots | ✅ (offline probe) | ◑ portable |
| **Vanilla** profile | ❌ (non-err DLL build blocked) | ✅ 949 maps / 3193 asset-lots | ✅ (offline probe) | ◑ portable |
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

## Why the 3 non-ERR profiles aren't runtime-tested
The data pipeline bakes 4 profiles (`src/generated_<profile>/`), but only the **ERR
DLL** is wired to build: `CMakeLists.txt` references ERR-only generated files
(`goblin_quest_gates`, `goblin_quest_steps`, `goblin_region_anchors`,
`goblin_name_regions`, `goblin_model_aliases`) that the non-err pipeline doesn't
emit (those dirs have 11–15 files vs ERR's 25). So a non-err DLL build fails at
configure ("No SOURCES given"). Making non-err DLLs buildable (conditional CMake
sources + code that doesn't reference those symbols off-ERR, or stubs) is a
separate task — it is the only thing blocking in-game testing of ERTE / Convergence
/ Vanilla. The disk-loot feature itself is profile-independent (reads MSBs live);
it is fully validated **statically** on all four.

## Regeneration status (this session)
- ERR: committed bake (current).
- ERTE: regenerated 2026-06-24 (449s).
- Convergence: regenerated 2026-06-24 (421s).
- Vanilla: regenerated 2026-06-24 (330s).
- All 3 non-err dirs have 15 files (vs ERR's 25) — confirms the ERR-only generated
  files (quest_gates/quest_steps/region_anchors/name_regions/model_aliases) are
  absent off-ERR, i.e. the non-err DLL build blocker stands.
