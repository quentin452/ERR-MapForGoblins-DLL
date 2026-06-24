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
| **ERR** profile (full disk loot) | ✅ in-game: 651 maps, 0 KRAK skip, 3235 replaced, no hitch | ✅ | ✅ (game + DLL build) | ◑ offline parse ✅ (708 DFLT maps→4075 tre + 256 KRAK 0-fail; m10=113); DLL build+runtime not run |
| **ERTE** profile | ✅ in-game: 458 maps, 0 KRAK skip, 3226 replaced | ✅ 458 maps / 3320 asset-lots | ✅ (game via ME3 + build) | ◑ portable |
| **Convergence** profile | ✅ in-game: 468 maps, 0 KRAK skip, 3227 replaced | ✅ 468 maps / 3623 asset-lots | ✅ (game via ME3 + build) | ◑ portable |
| **Vanilla** profile | ✅ in-game: 949 maps, 0 KRAK skip, 3062 replaced | ✅ 949 maps / 3193 asset-lots | ✅ (game via ME3 + build) | ◑ portable |
| **DFLT** decompress (zlib / stb) | ✅ (ERR in-game) | ✅ | ✅ | ✅ (stb in-tree, `tools/msbe_test/build.sh`) |
| **KRAK** decompress (Oodle / oo2core) | ✅ (ERR in-game, 0 skipped) | ✅ (oo2core via ctypes, 4 profiles) | ✅ | ✅ **native** `liboo2corelinux64.so.9` (no Wine), `build_oodle.sh` |
| **DummyAsset filter** (`PART +0x0c`) | ✅ (178 → 21 in-game) | ✅ (487 maps) | ✅ | ✅ |
| **recover-later** tracking (3 lots) | ✅ (in-game) | ✅ | ✅ | ✅ |
| **pre-build at init** (no map-open hitch) | ✅ (in-game) | n/a | ✅ | ◑ |

## In-game runtime results — all 4 profiles (2026-06-24, via the bundled ME3)
Launched each via `internals/modengine/bin/win64/me3.exe launch -p <profile>.me3` (vanilla =
DLL-only, no package; ERTE/Convergence = DLL + the staged data package). **0 KRAK skipped on
every profile** → the game's Oodle covers 100% of maps live. Vanilla went 0 → full coverage.

| profile | `_00` maps | KRAK skipped | replaced | unclassified | disk-only | recover-later |
|---|---|---|---|---|---|---|
| ERR | 651 | 0 | 3235 | — | 21 | 3 |
| ERTE | 458 | 0 | 3226 | 179 | 19 | 7 |
| Convergence | 468 | 0 | 3227 | 489 | 17 | 4 |
| Vanilla | 949 | 0 | 3062 | 191 | 14 | 3 |

Note: Convergence shows 489 unclassified (vs ~180 elsewhere) — Convergence items missing from its
`ITEM_ICONS` classifier; they fall back to the bake (not lost). Worth a look for 100% Convergence.

## Offline coverage (with Oodle, all `_00` maps, 0 failures)
| profile | `_00` maps | parsed | asset-lots | DummyAsset |
|---|---|---|---|---|
| ERR | 651 | 651 | 3361 | 319 |
| ERTE | 458 | 458 | 3320 | 307 |
| Convergence | 468 | 468 | 3623 | 312 |
| Vanilla | 949 | 949 | 3193 | 315 |

## Linux offline results (2026-06-24, Part A) — native, no game, no Wine
Ran the offline parser/Oodle path on the local Linux box against ERR's real files
(`<ERR>/mod/map/MapStudio`, 964 `.msb.dcx`):
- **DFLT** (`tools/msbe_test/build.sh`, native clang++/stb): `m10_00_00_00` → **113 treasures**
  (107 with part), exact `items_database.json` match. Full sweep: 708 DFLT maps → **4075 treasures,
  0 failures**.
- **KRAK** (`tools/msbe_test/build_oodle.sh` → `msbe_oodle_native`): **NATIVE**
  `liboo2corelinux64.so.9` via `dlopen` (System-V ABI) — **no Wine**. `m45_01_00_00` decompressed
  17604 → 840384 bytes, parsed ok. All 256 ERR KRAK maps decompress+parse, 0 failures (ERR's KRAK
  maps are treasureless — ERR re-saves its loot maps as DFLT; KRAK loot appears on the
  vanilla/non-ERR profiles, where the native `.so` covers it the same way).
- Correction to the brief: the native `.so` path is now wired (`msbe_oodle_native.cpp`), so offline
  KRAK on Linux does **not** need Wine. The Wine exe (`msbe_oodle_test.cpp`) remains as a fallback.
- **Not run on Linux yet:** Part B (cross-build the DLL) + Part C (in-game via Proton/ME3).

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
