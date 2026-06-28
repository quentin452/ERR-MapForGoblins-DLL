---
name: build-toolchain-clang-xwin
description: How to build + deploy MapForGoblins.dll on THIS Windows box (no MSVC) via clang-cl + xwin + ninja
metadata: 
  node_type: memory
  type: project
---

**This box has NO Visual Studio / MSVC** (vswhere absent, `cl` absent) so `build.bat` (which
needs VS2022) does NOT work here. The repo is designed for **clang-cl + xwin** (toolchain file
`clang-cl-xwin.cmake`). Toolchain installed 2026-06-20 (non-admin):

- **LLVM 22.1.8** (clang-cl, lld-link, llvm-rc, llvm-mt) via **scoop** (user-scope, no admin):
  `~\scoop\apps\llvm\current\bin`. (winget LLVM upgrade fails: needs admin. scoop is the way.)
- **ninja** `C:\ninja-win\ninja.exe`; **cmake** `C:\Program Files\CMake\bin\cmake.exe` (v4.1).
- **xwin 0.9.0** at `D:\mfg_toolchain\xwin-0.9.0-x86_64-pc-windows-msvc\xwin.exe`; **MSVC CRT+SDK
  splat at `D:\mfg_toolchain\xwin-sdk`** (618 MB, x86_64). Re-splat cmd (cache MUST be same drive
  as output or cross-drive move fails; `--disable-symlinks` still errors on one convenience
  symlink `sdk/include/10.0.26100`→`.` but the splat is FUNCTIONALLY COMPLETE — toolchain uses flat
  paths, ignore that error):
  `xwin --accept-license --arch x86_64 --cache-dir D:\mfg_toolchain\.xwin-cache splat --output D:\mfg_toolchain\xwin-sdk --disable-symlinks`

**Build + deploy (PowerShell tool), from repo root.** ⚠️ Pass the `-D` args via a PS **array**,
NOT inline with backtick line-continuation: the inline form mangles `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`
→ CMake sees value `"3"` (the `.5` is dropped) → FetchContent deps (minhook/spdlog) fail
`cmake_minimum_required`. The array form preserves it (confirmed 2026-06-27). Only bites on a
*reconfigure* (CMakeLists touched); pure incremental `ninja` skips configure so it was masked:
```
$env:PATH = "$env:USERPROFILE\scoop\apps\llvm\current\bin;C:\ninja-win;$env:PATH"
$cmakeArgs = @(
  '-B','build-clang','-G','Ninja',
  '-DCMAKE_MAKE_PROGRAM=C:/ninja-win/ninja.exe',
  '-DCMAKE_TOOLCHAIN_FILE=<repo>/clang-cl-xwin.cmake',
  '-DXWIN=D:/mfg_toolchain/xwin-sdk','-DGENERATED_SUBDIR=generated',
  '-DCMAKE_BUILD_TYPE=Release','-DCMAKE_POLICY_VERSION_MINIMUM=3.5')
& "C:\Program Files\CMake\bin\cmake.exe" @cmakeArgs
& "C:\ninja-win\ninja.exe" -C build-clang MapForGoblins
Copy-Item build-clang\MapForGoblins.dll "<windows_downloads>\ERRv2.2.9.6-541-2-2-9-6-1780861369\ERRv2.2.9.6\dll\offline\MapForGoblins.dll" -Force
```
Output DLL = `build-clang\MapForGoblins.dll` (~5.1 MB). src/generated/ is committed (25 files) so
NO need to run the data pipeline for a code-only rebuild.

**Per-PROFILE DLL builds (erte/convergence/vanilla)** — use a separate build dir + the profile's
generated subdir (CMake sets MFG_VANILLA for non-err, MFG_PROFILE_VANILLA for vanilla):
`cmake -B build-<p> ... -DGENERATED_SUBDIR=generated_<p> ...` then `ninja -C build-<p> MapForGoblins`.
GOTCHA: the pipeline does NOT emit the 5 ERR-only generated tables (quest_gates/quest_steps/
region_anchors/name_regions/model_aliases) for non-err, but the DLL references them → configure
fails ("No SOURCES"). FIX: `py tools/gen_nonerr_stubs.py <profile>` writes empty COUNT=0 stubs
(build_pipeline.py now runs it automatically at the end of a non-err run). All 4 profiles built OK
2026-06-24 (build-clang/erte/convergence/vanilla). Loot path is profile-independent; stubbed
features (quest browser, region labels) are just absent on non-err.

**Gotchas (all hit + solved 2026-06-20):**
1. **Must be Release** — `CMAKE_MSVC_RUNTIME_LIBRARY` picks the DEBUG CRT (`libcmtd.lib`/`libcpmtd0.lib`)
   in Debug, which xwin does NOT splat → link fails. Always pass `-DCMAKE_BUILD_TYPE=Release`.
2. **CMake 4.1 vs old deps** — minhook's `cmake_minimum_required(<3.5)` is rejected → pass
   `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`.
3. **PowerShell arg splitting** — pass cmake `-D...` args quoted / via an array, and an ABSOLUTE
   toolchain-file path (a bare `clang-cl-xwin.cmake` got split into `clang-cl-xwin` + `.cmake`).
4. FetchContent pulls minhook + spdlog (needs internet on first configure).
5. **★ clang-cl ELIDES SEH around raw loads/stores (THE big one, 2026-06-20).** The codebase
   guards cross-memory access with `__try{ *p / memcpy }__except` and relies on MSVC SEH to
   catch faults. **clang-cl proves a plain load "can't fault" (UB: deref assumed valid) and
   DROPS the guard** → bad/freed/unmapped pointers fault UNHANDLED → 0xC0000005. MSVC builds
   (<user>'s) are fine; clang-cl builds crash at every such site. Crash seen at RVA ~0x97Exx
   = `seh_scan_region_for_vft` (kindling heap-scan for EcTestDistance vft), confirmed via
   `llvm-objdump -d` at the exact fault RVA. **FIX (applied 2026-06-20): convert every raw
   `__try`-guarded memory access to ReadProcessMemory / WriteProcessMemory** (opaque kernel
   calls clang can't elide; return false on bad addr). Converted: goblin_kindling.cpp
   (seh_read_qword/dword, safe_write_byte, seh_scan_region_for_vft), goblin_markers.cpp
   (seh_copy), goblin_collected.cpp (safe_read, safe_write_byte), goblin_debug_events.cpp
   (safe_copy), + goblin_worldmap_probe.cpp (already RPM). `__try` that wraps a CALL (not a raw
   deref) is PRESERVED by clang-cl → those are fine (goblin_messages, inject hooks, dllmain).
   Rule for clang-cl builds: NEVER `__try` a raw load/store; use RPM/WPM (or `volatile`).

Deploy target dir (offline mode DLLs): `<windows_downloads>\ERRv2.2.9.6-541-2-2-9-6-1780861369\ERRv2.2.9.6\dll\offline\`
(also holds MapForGoblins.ini, er_console_mod.dll). See [[mapforgoblins-pipeline-setup]],
[[windows-tooling-gotchas]]. <user> still runtime-tests in-game.
