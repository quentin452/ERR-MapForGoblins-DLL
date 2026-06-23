# RPM walk audit — for the background RPM-thread move

> Linux/Wine makes every `ReadProcessMemory`/`WriteProcessMemory` a wineserver IPC
> round-trip (cheap kernel path on Windows). Per-node walks on the **game thread** are the
> Wine freeze cliffs (see `linux-rpm-walk-danger`). This is the complete inventory of every
> RPM site, the thread it runs on, whether it's already bulk-read, and whether it's
> `GOBLIN_BENCH_QUIET`-wrapped — the input for moving the harvest walks onto a background poll.

## RPM wrapper definitions

| File | Wrapper | Line | Direction |
|---|---|---|---|
| goblin_worldmap_probe.cpp | `seh_read8` / `seh_read4` / `seh_read_i32` | 109 / 114 / 119 | read |
| goblin_worldmap_probe.cpp | `seh_write_i32` / `seh_write_f32` | 125 / 130 | write |
| goblin_inject.cpp | `icon_rpm_i32` / `icon_rpm_ptr` | 1658 / 1663 | read |
| goblin_inject.cpp | `res_w32` | 2734 | write |
| goblin_markers.cpp | `seh_copy` | 496 | read |
| goblin_kindling.cpp | `seh_read_qword` / `seh_read_dword` | 213 / 223 | read |
| goblin_debug_events.cpp | `safe_copy` | 155 | read |
| goblin_collected.cpp | `safe_read` / `safe_write_byte` | 129 / 142 | read/write |

## GAME THREAD (find_detour / res_tick_detour hooks) — the move targets

| Function | File:line | Per-node / bulk | Bench-wrapped |
|---|---|---|---|
| `harvest_repo_icons` | goblin_inject.cpp:2204 | BULK (1 RPM/node, 0x58 B) | ✅ `harvest.repo_walk` @2245 |
| `harvest_twin_map_icons` | goblin_inject.cpp:2396 | BULK (1 RPM/node, 0x58 B) | ✅ `harvest.twin_walk` @2410 |
| `cache_map_sprite_from_img` | goblin_inject.cpp:2336 | per-field | ❌ |
| `cache_icon_from_img` | goblin_inject.cpp:1827 | per-field | ❌ |
| `harvest_resident_icons` | goblin_inject.cpp:1954 | per-field walk (≤4096) | ✅ `harvest.resident_walk` |
| `icon_find_gpu_tex` | goblin_inject.cpp:1673 | PER-NODE BFS | n/a — **DEAD CODE, no callers** (superseded by node+0x50 direct read) |
| `res_walk_groups` | goblin_inject.cpp:2744 | PER-NODE (F1 BINDTEST debug) | ✅ `bindtest.group_walk` |
| `run_force_grace` | goblin_inject.cpp:~2790 | engine CreateImage calls (not a walk), throttled | n/a — not an RPM cliff |
| `get_warp_area` / `warp_pin_detour` | goblin_inject.cpp:3428 / ~3445 | per-call, 1-3 reads | not wrapped — too small to matter |

**Wrap status:** every game-thread RPM **walk** is now bench-visible — the relocated
`harvest_repo_icons`/`harvest_twin_map_icons` (probe thread), plus `harvest.resident_walk` +
`bindtest.group_walk` (game thread, dev/debug-gated). `icon_find_gpu_tex` is dead; the small
per-call sites (warp_pin/get_warp_area) and the throttled engine-call grace-force are not Wine
RPM cliffs, left unwrapped.

## REFRESH background thread (already off the game thread)

| Function | File:line | Per-node / bulk | Bench-wrapped |
|---|---|---|---|
| `read_wgm_snapshot` | goblin_collected.cpp:312 | BULK (vec + geom_ins + MSB-part) | ✅ `refresh.collected.read_wgm` |
| `read_geof_from_memory` | goblin_collected.cpp:536 | BULK (singleton table) | ✅ `refresh.collected.read_geof` |
| `kindling::refresh` | goblin_kindling.cpp:619 | per-condition seh_read | ✅ `refresh.kindling.total` |
| `seh_scan_region_for_vft` | goblin_kindling.cpp:280 | BULK (1MB chunks) | inside refresh timing |

## WORLDMAP PROBE background thread (100ms) — the move destination

| Function | File:line | Per-node / bulk | Bench-wrapped |
|---|---|---|---|
| `probe_loop` | goblin_worldmap_probe.cpp:704 | per-call (cursor tracking) | ❌ (monitor) |
| `project` | goblin_worldmap_probe.cpp:922 | per-call (hot path) | ❌ |
| `get_live_view` | goblin_worldmap_probe.cpp:961 | per-call | ❌ |
| `dump_native_pins` | goblin_worldmap_probe.cpp:672 | **PER-NODE RB-tree** (debug) | ✅ `debug.dump_native_pins` |
| `dump_converters` | goblin_worldmap_probe.cpp:513 | **PER-NODE** (debug) | ✅ `debug.dump_converters` |

NB `project` + `get_live_view` are called from the **overlay-render thread** (map_renderer.cpp
414/457/844/1150), not the probe loop.

## Publish globals + mutexes

| Global | File:line | Type | Guard |
|---|---|---|---|
| `g_map_icon_rects` / `g_map_icon_named` | goblin_inject.cpp:1964 | `std::map<…,MapIconRect>` | `g_map_icon_mtx` @1966 (writer `store_map_icon_rect` @1971 + getters @3154/3164 all lock it) |
| `g_grace_sprite` / `g_grace_locked` | goblin_inject.cpp:2017 | `goblin::ItemSprite` + bool | `g_harvest_mtx` (set @2365, getter @2942) |
| `g_harvest` (icon cache map) | goblin_inject.cpp:2012 | `std::unordered_map<int,ItemSprite>` | `g_harvest_mtx` @2011 |
| `g_active_cursor` | goblin_worldmap_probe.cpp | atomic | std::atomic |

Writer (was game thread, now probe thread) and reader (render thread) were ALREADY different
threads, so the locking above already covers the cross-thread case — the move adds no new race.

## Move plan inputs

- **Move OFF game thread:** `harvest_repo_icons`, `harvest_twin_map_icons` (both already BULK)
  + their feeders `cache_map_sprite_from_img` / `cache_icon_from_img` / `harvest_resident_icons`.
  Publish into `g_map_icon_rects` (under `g_harvest_mtx`) and `g_grace_sprite` (under
  `g_grace_locked`); render/by-name lookups read the published cache.
- **Destination:** the `probe_loop` background thread (goblin_worldmap_probe.cpp:704, "always
  runs"). Hook the harvest there instead of `find_detour`.
- **Before moving:** wrap every unwrapped game-thread RPM site in `GOBLIN_BENCH_QUIET` to expose
  any remaining Wine cliff.
- **Already safe (no move):** read_wgm / read_geof (refresh thread, bulk), probe_loop / project
  (probe / render threads).
