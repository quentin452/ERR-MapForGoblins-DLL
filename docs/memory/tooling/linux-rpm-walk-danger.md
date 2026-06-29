---
name: linux-rpm-walk-danger
description: "Per-node ReadProcessMemory walks (RB-tree/array scans) are ~free on Windows but a Wine FREEZE cliff on Linux/Proton; bulk-read whole structs, throttle re-walks, and wrap game-thread walks in GOBLIN_BENCH_QUIET so they stay visible"
metadata: 
  node_type: memory
  type: feedback
---

In this DLL, code that reads game memory with one `ReadProcessMemory` (icon_rpm_*) PER FIELD PER NODE is the #1 Linux-only performance trap. On Windows RPM-on-self is nearly free; under Wine/Proton each RPM call has real syscall-ish overhead, so a walk of ~1000 nodes × ~6 reads = thousands of RPM = visible micro/big freezes. Windows devs never see it. Confirmed cases: `read_geof`/`read_wgm` (collected scan, [[collected-geof-bruteforce-scan]]) and `harvest_repo_icons`/`harvest_twin_map_icons` (FD4 image-repo RB-tree walks, 2026-06-23).

**Why:** Wine's ReadProcessMemory is orders slower than native. The freeze hides from the [BENCH] report because these walks run on the GAME thread (inside find_detour/enum hooks), not the overlay render path the per-frame bench times.

**How to apply when adding/reviewing a memory walk:**
1. BULK-READ: one `ReadProcessMemory` of the whole struct/region, then parse fields from the local buffer — not a read per field. (RB-tree node = 0x58-byte header: _Left@0x00 _Right@0x10 _Isnil@0x19 key@0x28 cap@0x40 value@0x50.) ~6× fewer RPM.
2. THROTTLE re-walks: gate on a real change AND a min interval (e.g. `GetTickCount() - last < 400ms` in production) so churning counters can't trigger a walk-storm. Data we need (map symbols/graces) loads once and persists in cache.
3. KEEP IT BENCH-VISIBLE: wrap the walk in `GOBLIN_BENCH_QUIET("label")` (aggregate only, no per-call log spam — thread-safe, mutexed). Then a future regression shows in the [BENCH] count/avg/max instead of being an invisible Linux freeze. Gate any per-walk spdlog line on `dumpIconTextures` (log I/O under Wine is itself slow).

**Also:** don't gate PRODUCTION feature paths behind the dev flag. `install_icon_texture_probe` early-returned `if (!dumpIconTextures)`, so the find/res-tick/CreateImage hooks (which feed GPU graces + map_icon_rect via harvest) only installed under the laggy debug flag — GPU icons were blank without it. Fixed: install the production hooks under `graceOverlay || nativeItemIcons`, keep only the heavy dev machinery (Oodle/calibration/verbose dumps) under `dumpIconTextures`. See [[native-icon-render-pipeline]], [[overlay-rendered-markers]], [[param-struct-offset-verification]].
