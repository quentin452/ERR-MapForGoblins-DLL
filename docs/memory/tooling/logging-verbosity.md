---
name: logging-verbosity
description: "How MapForGoblins logging verbosity is controlled; the [BENCH] flood fix; the pending category-logging rewrite plan."
metadata:
  node_type: memory
  type: tooling
---

# Logging verbosity — control knobs + the flood fix

**Logger.** Single spdlog logger "mapforgoblins", `setup_logger()` in `src/dllmain.cpp:159`; one
`basic_file_sink_st` truncated per session to `logs/MapForGoblins.log`, `flush_on(info)`. Previous
sessions are zipped to `logs/archive/` (store-only, uncompressed) by `archive_and_rotate()` at attach
(`goblin_log_archive.cpp`, `keep_n=20`).

**Control mechanisms (as of 2026-07-23, 3 uneven ones).**
- `debugLogging` (ini `debug_logging`, default false) → sets the logger level to `debug`. NOTE: it does NOT
  silence the flood, which is all at `info`; it only *adds* the 22 `spdlog::debug` lines.
- `benchLogIndividual` / `benchLogSession` (`[Debug]`) — per-call / per-session `[BENCH]` gates
  (`goblin_bench.hpp`). `benchLogIndividual` is now **default false** (see below).
- 23 `debug*/diag*` config bools — most gate a FEATURE (install a hook / draw a viz), not just a log line,
  so they are feature toggles, not pure logging knobs.

**The [BENCH] flood (fixed 2026-07-23, Phase 0).** `[BENCH]` was 88 % of a real 36 759-line log, dominated
by a single site: `render.minimap` (`goblin_overlay_render.cpp:204`) used the LOUD `GOBLIN_BENCH` macro on
a per-frame HUD (28 390 lines, 77 % of the whole file) while its sibling `render.worldmap` used
`GOBLIN_BENCH_QUIET`. Fixes:
1. `render.minimap` → `GOBLIN_BENCH_QUIET` (aggregate-only; session report + `[BENCH][SPIKE]` warns still
   fire). This is the rule: **any per-frame / per-refresh bench scope must be `_QUIET`**, never loud
   `GOBLIN_BENCH` — the loud macro is for one-shot / init spans only.
2. `benchLogIndividual` default `true`→`false`. Per-call `[BENCH] label: X ms` lines are a dev-profiling
   flood; the end-of-session summary keeps the useful avg/min/max/total, and spike warns are independent.

Result: a normal session (`debugLogging` off, diags off) drops from ~37k to well under ~3k lines.

**Per-tick diagnostic spam (fixed 2026-07-23, second pass).** After Phase 0, live testing surfaced
per-refresh / per-second repeats that were NOT bench and fired even at the shipped default:
- `[CURSORDIAG]`/`[KBDIAG]` 1 Hz dumps (`goblin_overlay.cpp`) — instrumentation for the RESOLVED Proton-11
  cursor-lock + Alt+Tab keyboard bugs, at `info`, ungated. Now gated behind `debug_cursor_diagnostic`
  (the existing crosshair-viz flag, default off).
- `[KINDLING] no kindling conds found` (`goblin_kindling.cpp`) — the scan runs every refresh (~2 Hz); the
  steady-state line is now logged **on state change only** (0↔N transitions still log once each).
- `[GEOF] Memory: N flag-save entries` (`goblin_collected.cpp:902`) — `debug`-level, per-refresh; now
  **on-change only** (count change).
Guardrail: a diagnostic that runs on a per-refresh / per-frame cadence must be gated by a dev flag OR
logged on-change, never an unconditional per-tick `spdlog::info`. `debug_logging=true` is a common dev
setting, so gating repeats behind `debugLogging` alone does NOT make them quiet for those users.

**Category-logging rewrite — NOT pursued (design only).** `docs/plans/logging_rewrite_plan.md` Phase 1
designed a `goblin::log` category + per-level system (`GLOG_*` macros, `[Logging]` config). User decided
against it 2026-07-23: Phase 0 already removed ~92% of the flood, so the ~300–400-site migration isn't
worth the churn. Design kept in the plan if the flood ever regrows.

**Logs-dir clutter — FIXED 2026-07-23 (Phase 2).** `is_session_log()` in `goblin_log_archive.cpp` now ALSO
folds `MapForGoblins_{crash,freeze,load_stall}_*.{txt,dmp}` into the per-session archive zip + prunes to
`keep_n=20`. Matched by NAME PREFIX so it never sweeps user-named `MapForGoblins.log.<label>` baselines or
dev probe dumps (`*.bmp`, `paramdump_*.txt`, `markers.csv`, `MapForGoblins_wmprobe.log`). Still deferred:
the archive zip writer is store-only (uncompressed) — deflate would shrink `archive/` a lot but is a bigger
change to `write_store_zip()`.
