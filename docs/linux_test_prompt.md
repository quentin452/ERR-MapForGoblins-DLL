# Linux test brief — disk-MSB loot, validate on Linux (offline + Proton runtime)

Hand this to a Claude agent running on the Linux box. Goal: reproduce, **on Linux**, the
disk-MSB-loot validation already green on Windows — both the **offline parser/Oodle path**
(native Linux) and the **in-game runtime** (Elden Ring via Proton). The Windows side proved
all 4 profiles parse 100% of maps with **0 KRAK skipped**; we want the Linux column of
`docs/loot_from_disk_test_status.md` filled in.

## ⚠️ DON'T FORGET: rebuild + deploy the DLLs
The repo's `MapForGoblins.dll` is a **Windows PE** (it's injected into eldenring.exe, which
runs under Proton/Wine on Linux). It is cross-compiled with **clang-cl + lld-link + xwin** — the
SAME toolchain file works on Linux (xwin is built for Linux→Windows cross-compile). So on Linux:
1. **Rebuild** each DLL you intend to test (clang-cl/xwin), and
2. **Deploy** it into the mod's DLL folder
BEFORE launching the game. A stale or un-deployed DLL is the #1 way this test silently uses old
code. Verify the deployed DLL's timestamp after copying.

## State / branch
- Branch `feat/msbe-disk-parser` (12 commits, UNPUSHED on the Windows box — pull it / coordinate).
- Feature config: `loot_from_disk_msb` (bool, default off) + `loot_msb_dir` (string, empty=auto).
- Key code: `src/worldmap/{msbe_parser,loot_disk,map_entry_layer}.*`, `tools/gen_nonerr_stubs.py`.
- Read first: `docs/loot_from_disk_test_status.md`,
  `docs/re/windows_msbe_dummyasset_unreachable_re_findings.md`.

## Part A — offline parser/Oodle on NATIVE Linux (the ◑ cells)
The parser is `<windows.h>`-free and offline-testable. Two host verifiers exist:
- `tools/msbe_test/build.sh` → builds the **DFLT (zlib/stb)** verifier. Run it on a few mod
  `map/MapStudio/m*_00.msb.dcx` (e.g. ERR's `m10_00_00_00` → expect 113 treasures).
- `tools/msbe_test/build_oodle.sh` → builds the **KRAK (Oodle)** verifier. It needs an Oodle:
  either the native `liboo2corelinux64.so.9` ERR ships (look in the ERR install
  `internals/launcher/`) or `oo2core_6_win64.dll` under Wine. Run it on a KRAK map (a vanilla
  `m*_00.msb.dcx`) and confirm it decompresses + parses (`MSB ` header, treasures listed).
- Optional but high-signal: port the Windows coverage probe (replicate the `parse_msb` section
  walk in Python over each profile's `map/MapStudio`) and confirm the same per-profile counts as
  the Windows run (see the coverage table in the status doc). This validates the parser+Oodle on
  Linux without the game.

## Part B — build the DLL(s) on Linux (clang-cl + xwin)
Mirror `[[build-toolchain-clang-xwin]]` (the Windows recipe), adapted to Linux paths:
```
cmake -B build-clang -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/clang-cl-xwin.cmake \
  -DXWIN=<path to your xwin splat> -DGENERATED_SUBDIR=generated \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
ninja -C build-clang MapForGoblins      # -> build-clang/MapForGoblins.dll (Windows PE)
```
- MUST be `Release` (Debug picks a CRT xwin doesn't splat).
- ERR uses the committed `src/generated/` (25 files) — builds out of the box.
- **Non-err profiles (erte/convergence/vanilla)**: their `src/generated_<profile>/` are
  git-ignored + Windows-pipeline-generated. If those dirs are present on the Linux box, build each
  with `-DGENERATED_SUBDIR=generated_<profile>` in a separate build dir AND first run
  `python tools/gen_nonerr_stubs.py <profile>` (writes empty ERR-only tables, or the configure
  fails with "No SOURCES"; `build_pipeline.py` does this automatically when the pipeline runs).
  If the non-err generated dirs are NOT on the Linux box (the data pipeline is Windows-only:
  needs pythonnet + Andre.SoulsFormats), either copy them from the Windows box or just validate
  **ERR** on Linux + the other 3 via Part A (offline) — the loot path is profile-independent.

## Part C — deploy + runtime via Proton
- **ERR** ships Linux launchers: `3/4 - Launch ELDEN RING Reforged ... (Linux).sh` in the ERR
  install. It uses the bundled ModEngine3 (`internals/modengine/`); the offline-mode DLLs live in
  `dll/offline/`. **Deploy** `build-clang/MapForGoblins.dll` → `<ERR>/dll/offline/MapForGoblins.dll`,
  set `dll/offline/MapForGoblins.ini` `loot_from_disk_msb = true` (+ `loot_msb_dir` = the ERR mod
  dir, e.g. `<ERR>/mod`), then run the offline Linux launcher.
- **ME3 under Proton**: the bundled `me3` has `--windows-binaries-dir` for Proton. The `.me3`
  configs are identical to Windows; `resolve_oodle()` does `GetModuleHandleW("oo2core_6_win64.dll")`
  which resolves inside the Proton process (the game loads the Windows oo2core), so KRAK works at
  runtime under Proton with no code change.
- For each profile launched: load a save in the overworld, open the world map (triggers the
  one-time build — it's now pre-built at init, so it may already be in the log), exit.

## What to capture / report
From `<dll folder>/logs/MapForGoblins.log`, the `[LOOTDISK]` block:
```
[LOOTDISK] reading MSBs from <dir>
[LOOTDISK] Oodle (KRAK maps) available            <- must say "available", not "NOT found"
[LOOTDISK] N _00 MSBs parsed (... DummyAsset dropped); 0 KRAK skipped   <- 0 is the target
[LOOTDISK] emitted .. / .. covered / .. unclassified
[LOOTDISK] replaced .. baked lot rows with disk placements
```
Compare to the Windows in-game numbers (status doc "In-game runtime results" table):
ERR 651 maps / 3235 replaced, ERTE 458 / 3226, Convergence 468 / 3227, Vanilla 949 / 3062 —
**0 KRAK skipped everywhere**. Then update `docs/loot_from_disk_test_status.md`: flip the **Linux**
column from ◑ to ✅ for what you actually ran, and add a short "Linux runtime results" note.

## Gotchas
- Rebuild **and** deploy before every launch (see top). Check the deployed DLL timestamp.
- The INI auto-migrates on launch; it preserves your `loot_from_disk_msb`/`loot_msb_dir` values.
- `[LOOTDISK]` logs at init/first-map-build — present even if the game later crashes (esp. a
  partial Convergence overlay), so grab the log regardless of how the session ends.
- If `Oodle ... NOT found`: the game hasn't loaded oo2core yet at build time, or the module name
  differs — log `GetModuleHandleW` result; under Proton the name is still `oo2core_6_win64.dll`.
