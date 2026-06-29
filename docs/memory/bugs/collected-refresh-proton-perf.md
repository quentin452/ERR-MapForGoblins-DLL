---
name: collected-refresh-proton-perf
description: "The Proton-only ~20fps collected-refresh stutter — ROOT-CAUSE FIX = drop RPM-to-self for direct in-process reads. Plus the gate/bound work and the no-getter Ghidra verdict."
metadata:
  node_type: memory
  type: project
---

# Collected-refresh lag on Proton — FIXED at the source 2026-06-27

## The problem
`goblin::collected::refresh()` → `read_wgm_snapshot()` (goblin_collected.cpp) walked `CSWorldGeomMan`'s
RB-tree + per-instance MSB chain every tick via `safe_read` = **ReadProcessMemory-to-self**. On **Proton**
every RPM is a wineserver IPC (~10µs); the single-threaded wineserver serialises the whole process →
the per-refresh RPM flood starves the game thread = **constant ~20fps stutter** (581ms cold-tile spikes).
**Windows is UNAFFECTED** (RPM-to-self = cheap kernel path, `read_wgm ~0.00ms`) → can't repro on the dev
box; diagnose from the RPM *count*, not the bench.

## ★ THE FIX (merged to master — merge 83a723f, commit 85cece4): drop RPM, read in-process
We are injected INTO eldenring.exe, so a **raw deref reads the same memory for free** — RPM-to-self was
only a clang-cl **crash-safety** workaround (clang-cl elides `__try` around a raw load, so bad/freed
pointers faulted unguarded). Proper pattern: keep the SEH but wrap a **noinline CALL** —
`__declspec(noinline) raw_copy/raw_store8`, then `__try { raw_copy(...); } __except {...}`. clang-cl
PRESERVES `__try` around a *call* → bad pointers still fault-and-return-false, ZERO wineserver round-trips.
`safe_read` + `safe_write_byte` converted. **This is the real root-cause fix** — it removes the per-read
IPC everywhere collected reads, so the lag is gone regardless of how much the walk enumerates.
LESSON (supersedes the old [[build-toolchain-clang-xwin]] gotcha #5 "use RPM/WPM"): the cheaper clang-cl-
safe answer for hot in-process reads is `__try` around a `noinline` call, NOT RPM.

## Secondary work (branch perf/gate-collected-refresh — UNMERGED, now largely redundant)
Built BEFORE the RPM fix to cut how OFTEN/how MUCH the walk runs; keep or drop given RPM is gone:
- **Gate**: dllmain runs the collected refresh only when `fast_phase(<30s) || world_map_open() ||
  showMinimap` (graying is only visible on the map / minimap HUD). **★ `world_map_open()` =
  `CSMenuMan+0xCD == 7`, CONFIRMED on 2.6.x** (log-validated: read_wgm benches vanish while map closed).
- **Vicinity-bound**: when only the minimap is up, `read_wgm_snapshot` skips per-instance resolution for
  tiles outside player-tile ±minimap-radius (player tile from `get_player_raw_pos` = RAW frame = WGM
  block_id frame). Safe via sticky carry-forward.

## Ghidra verdict — there is NO by-id "is collected" getter (the user wanted one; it doesn't exist)
Decompiled the @geom-probe hits (`<ghidra_project>\ER`, query.java):
- **Loaded (WGM):** collected/inactive = `byte[CSWorldGeomIns+0x263] >= 0x80` (bit7), compared INLINE
  everywhere (hit#0 `FUN_1406c6050`: `if (0x7f < byte[+0x263]) return;`; hit#1 `FUN_1406a92a0` bulk
  per-block update loop). Both MUTATE state → not callable as pure queries. No separate getter.
- **Unloaded (GEOF):** the by-id primitive IS `FUN_1406ea190` (er+0x6ea190) — binary-search lower_bound
  over the GeomFlagSaveData DLFixedVector (key=tile_id, stride 0x10, count @ mgr+0x189d0, mgr singleton
  `DAT_143d69ba8` = our `world_geom_man_slot`). Its 5 callers (6e9140/94a0/9a10/96d0/95e0) are table
  MUTATORS (insert/erase). **We already read this exact table** in `read_geof_from_memory` (one bulk read).
So the "native getter" = the field/table reads we already do — and now (RPM gone) they're free.
- ⚠️ **Correctness lead:** engine alive/collected = **bit7 (0x80)** of +0x263 (`< 0x80` = alive); our code
  uses `(f263 & 0x02)` (bit1) + `!(f26B & 0x10)`. Different bit — our graying may misread some assets;
  candidate fix = switch to `byte[+0x263] < 0x80`. Validate in-game before changing.

## @geom probe tooling (merged, master)
`goblin_field_probe`: `arm_raw(addr,len,rw)` + spec `@geom[:off[:len[:rw]]]` (default +0x263:1:r) — defers
arming; the WGM walk arms DR0 on the first tracked ALIVE instance (`[GEOMPROBE]` logs which), `[FWA]`
logs the game read RIP. INI `probe_field_access=true`+`probe_field_spec=@geom`, EAC-bypassed/offline.
See [[fragment-gate-maplist-gap]], [[overlay-render-perf-followups]] (different render-loop perf item).
