# Tooling

Guides for the tools and reverse-engineering workflows behind the fork: Ghidra, live-memory RPM,
Cheat Engine, build pipelines, offset resolution, and file-format parsers. Read these before writing
one-off scripts — most workflows are already reusable.

## Build & deploy
- **clang-cl + xwin + ninja** [active] — the DLL builds without MSVC/Wine via `clang-cl-xwin.cmake`;
  Release + `CMAKE_POLICY_VERSION_MINIMUM=3.5`, per-profile build dirs, case-exact `Xinput.lib`. → [build-toolchain-clang-xwin](build-toolchain-clang-xwin.md)
- **Linux cross-build + deploy** [active] — live DLL is `<ERR_ROOT>/dll/offline/MapForGoblins.dll`
  (no hot-reload); **builds/deploys MUST run sandbox-disabled** or a COW overlay ships a stale DLL;
  ninja may falsely say "no work to do" after a revert (touch + verify md5). → [mapforgoblins-linux-build](mapforgoblins-linux-build.md)
- **4-profile data pipeline** [active, Windows-only] — pythonnet `build_pipeline.py`; err bake byte-identical
  vs ERR 2.2.9.6; vanilla/convergence/erte git-ignored. → [mapforgoblins-pipeline-setup](mapforgoblins-pipeline-setup.md)
- **`generate_data.py` is NOT deletable** [resolved] — still owns 5 live tables + the map_data stub;
  the `_map_entries_full.cpp` intermediate is gone. → [delete-generate-data-path](delete-generate-data-path.md)
- **Windows dev-box quirks** [active] — invoke .bat via PowerShell; per-call COW FS snapshots; stale
  redirects; forward-slash env paths. → [windows-tooling-gotchas](windows-tooling-gotchas.md)

## Dev / test infra
- **Overlay test harness** [proposed] — AI-driven "interactive Playwright" for the worldmap/minimap:
  Route A offline ImGui harness (mock data + imgui_test_engine + screenshots, deterministic, no game),
  Route B in-game debug-RPC + framebuffer grab. → [overlay-test-harness](overlay-test-harness.md)

## Ghidra & static RE
- **Reusable Ghidra tools** [active] — `rtti_index.txt` (9760 classes) + `query.java`; index + on-demand
  query beats bulk decompile; RVAs are build-specific, regenerate after a patch. → [ghidra-re-tooling](ghidra-re-tooling.md)
- **Worldmap RE master log** [active] — canonical Ghidra findings: live-icon refresh, affine, projection,
  layer getter, grace suppression, gamepad fix. → [ghidra-worldmap-re](ghidra-worldmap-re.md)

## Live memory (RPM / CE / in-process reads)
- **External Python RPM** [active] — ctypes `ReadProcessMemory` against running `eldenring.exe`; ASLR base
  via Toolhelp. Distinct from the in-process RPM-to-self that was dropped for perf. → [rpm-live-memory-tooling](rpm-live-memory-tooling.md)
- **clang-cl SEH-elision safe reads** [active] — `__try` around a bare load is silently elided; use
  `__try` around a **noinline CALL** (`raw_copy`/`raw_store8`) for hot in-process reads, RPM otherwise. → [clang-cl-seh-noinline](clang-cl-seh-noinline.md)
- **Linux/Proton RPM walk trap** [active] — per-node RPM-to-self is a Proton freeze cliff; bulk-read
  whole structs then parse, throttle, wrap in `GOBLIN_BENCH_QUIET`. → [linux-rpm-walk-danger](linux-rpm-walk-danger.md)
- **ER Console as coords tool** [active] — Nexus 9365 readout; `coords` = block-local Havok frame,
  `tp` = larger chunk frame. → [er-console-mod](er-console-mod.md)

## Offsets & signatures
- **Runtime offset resolution** [resolved] — offsets lifted live from the exe's own access instructions
  (`modutils::resolve_field_offset` + AOBs); `check_param_offsets.py` is build-time advisory only. → [param-offset-source-of-truth](param-offset-source-of-truth.md)
- **Central AOB signature registry** [active] — ~18 AOBs + 3 RVAs in `re_signatures.hpp`; startup
  `[SIG] PASS/FAIL` health check — first place to look when an ER update breaks resolution. → [re-signatures-registry](re-signatures-registry.md)
- **Offset-validation recipe** [active] — 4 checks (raw bytes + anchor + ± samples + runtime sanity);
  never ship an offset hand-derived from paramdef packing. → [re-offset-validation](re-offset-validation.md) · [param-struct-offset-verification](param-struct-offset-verification.md)
- **Pinned MSB Enemy/NpcParam offsets** [active] — no-bake enemy-drop pass; row size 736, `partType@+0x0c==2`. → [msbe-enemy-loot-offsets](msbe-enemy-loot-offsets.md)
- **Item-classification regression guard** [active] — `[ITEMCLASS]` census → `item_classification.md`,
  diff after any classify change. → [item-classification-guard](item-classification-guard.md)

## Parsers & file formats
- **MSBE parser = no-bake loot source** [resolved] — disk DCX_DFLT/zlib + Oodle KRAK treasure-event parser. → [msbe-parser-supersedes-bake](msbe-parser-supersedes-bake.md)
- **Resident/disk MSB layout** [executed] — resident offsets are absolute VAs, disk entry-relative; parse `_00` only. → [runtime-msb-resident-plan](runtime-msb-resident-plan.md)
- **DarkScript3 EMEVD/ESD decompile** [active] — hidden CLI batch-decompiles ERR EMEVD to grep-able JS
  (oo2core in cwd); always MSB-confirm entity→name (the Iji=Ranni lesson). → [darkscript3-emevd-decompile](darkscript3-emevd-decompile.md)
- **ESD grace menu spike** [idea, parked] — grace menu lives in talk-ESD; ESD-merge is GO-but-poor,
  stay DLL-native. → [grace-menu-esd-spike](grace-menu-esd-spike.md)
- **dvdbnd packed reader** [open, unmerged] — RSA-2048 + BHD5 + prime-0x85 path hash + AES-128-ECB;
  spec validated, C++ impl on a branch only, no code in `src/`. → [dvdbnd-packed-reader](dvdbnd-packed-reader.md)
- **Virtual-FS alias** [superseded] — `system` alias regresses under ERR/ME3; ancestor-walk stays primary. → [virtualfs-alias-modroot-anchor](virtualfs-alias-modroot-anchor.md)
- **Runtime icon residency** [resolved] — no global icon page; offline bake is the path. → [runtime-icon-coverage](runtime-icon-coverage.md)
- **Overlay icon atlas** [active] — PNG-embedded atlas, stb runtime decode; regen needs FFDEC. → [overlay-icon-atlas](overlay-icon-atlas.md)
- **ERR save format** [active] — `ER0000.err` BND4 (28967888 B); SteamID64 in 11 slots + per-slot MD5. → [err-save-file-format](err-save-file-format.md)
