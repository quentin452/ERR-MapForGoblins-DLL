# Logging / crash-artifact rewrite plan

Status (2026-07-23): **Phase 0 landed, Phase 2 landed. Phase 1 NOT pursued** — user decision: Phase 0 already
removed ~92% of the flood, so the runtime verbosity is effectively solved and the ~300–400-site category
migration isn't worth the churn. Phase 1 kept below as a documented design if the flood ever regrows.

Goal (user ask 2026-07-23): the log is very verbose on disk AND at runtime (`[BENCH]` especially);
"keep the useful or gate certain logging by default in config, and simplify the logging system." Plus:
prune the crash-artifact clutter in the logs dir.

## Findings (what makes it verbose)

Measured on a real session log (`MapForGoblins.log`, 36 759 lines):

| Source | Lines | % of log | Mechanism |
|---|---|---|---|
| `[BENCH] render.minimap` | 28 390 | 77 % | per-frame HUD used loud `GOBLIN_BENCH` (bug — sibling `render.worldmap` is `_QUIET`) |
| `[BENCH] refresh.*` (per-marker-refresh) | ~2 900 | 8 % | loud per-call bench, `benchLogIndividual` default `true` |
| `[LOOTDISK]` | 1 236 | 3 % | ungated `spdlog::info` in the disk-loot build path |
| `[LOOTDIAG]`,`[KBDIAG]`,`[CURSORDIAG]`,`[KINDLING]`,`[GEOF]`,`[PARAMSCAN]`… | ~4 000 | 11 % | mix: some gated by a `debug*/diag*` bool, some ungated |

**Existing control mechanisms (3, uneven):**
- `debugLogging` (default false) → `spdlog::set_default_logger()->set_level(debug)`. But the flood is all at
  `info`, so this master toggle does **not** silence it — it only *adds* the 22 `spdlog::debug` lines.
- `benchLogIndividual` / `benchLogSession` — clean per-call / per-session gates for `[BENCH]` only.
- 23 `debug*/diag*` config bools — but most gate a **feature** (install a hook, draw a viz overlay), not
  just a log line, so they are NOT pure logging knobs and must stay as feature toggles.

**Volume:** 845 spdlog call sites (538 info, 187 warn, 89 error, 22 debug). Noise is a subset of the 538
`info` calls. Warnings/errors + one-shot boot/init `info` are the "useful" tier and stay always-on.

**Host↔render split:** render-side code (`src/worldmap/*`, `src/overlay_panel/*`) calls spdlog directly; in
the hot-reload split a sink bridge (`goblin_render_log_bridge.cpp`) forwards render lines to the host logger
and seeds the render logger's level from the host. Any log helper must be header-safe for BOTH sides
(mark exported API `GOBLIN_RENDER_API`, or keep it header-only/inline like `goblin_bench.hpp`).

## Phase 0 — landed (zero-risk quick wins, −85%+ of the flood)

1. `src/goblin_overlay_render.cpp:204` `render.minimap` → `GOBLIN_BENCH_QUIET` (kills the 28 390-line
   per-frame flood; session report + `[BENCH][SPIKE]` warns still fire).
2. `benchLogIndividual` default flipped `true`→`false` (`goblin_config_schema.cpp`) — the per-call `[BENCH]`
   lines are a dev-profiling flood; the session summary keeps the useful avg/min/max/total.

A normal session (`debugLogging` off, diags off) now logs well under ~3k lines vs ~37k.

## Phase 1 — category + level logging (`goblin::log`) — NOT PURSUED (design kept for reference)

One uniform mechanism that gates the runtime `info` flood and subsumes `debugLogging`'s global knob.
Design keeps it **simpler**, not additive: it does NOT try to absorb the 23 feature-diag bools (those gate
features), and leaves `[BENCH]` as its own subsystem.

### API (header-only, both-DLL safe)

```cpp
// goblin_log.hpp — category is a compile-time enum handle (O(1) array lookup, NO per-line hash).
enum class Cat : uint8_t { General, LootDisk, Kindling, Geof, ParamScan, Kbd, Cursor, /*…~40…*/ Count };
GOBLIN_RENDER_API bool goblin::log::want(Cat c, spdlog::level::level_enum lvl); // threshold check

#define GLOG(cat, lvl, ...) \
    do { if (::goblin::log::want(::goblin::log::Cat::cat, lvl)) \
         spdlog::log(lvl, "[" #cat "] " __VA_ARGS__); } while (0)
#define GLOG_INFO(cat, ...)  GLOG(cat, spdlog::level::info,  __VA_ARGS__)
#define GLOG_DEBUG(cat, ...) GLOG(cat, spdlog::level::debug, __VA_ARGS__)
#define GLOG_WARN(cat, ...)  GLOG(cat, spdlog::level::warn,  __VA_ARGS__)
#define GLOG_ERR(cat, ...)   GLOG(cat, spdlog::level::err,   __VA_ARGS__)
```

- The macro **auto-prefixes the `[TAG]`** from the category name → the 300-ish migrated message strings
  DROP their literal `[TAG]` prefix (SSOT for the tag; grep string unchanged).
- Threshold lives in a `Cat::Count`-sized `level_enum` array, seeded at config load. `want()` = one array
  read + compare. Cheap enough for per-frame paths (which stay `debug`/off by default anyway).

### Category → default-level table (SSOT, one place)

A single `struct { Cat c; const char* tag; level_enum def; }` table. Defaults: genuinely-useful categories
→ `info` (on); per-refresh / per-frame / RE-diagnostic categories → `debug` or `off` (silent by default).

### Config `[Logging]` (2 keys, no per-tag schema churn)

- `log_default_level = info` — global floor (replaces the meaning of `debugLogging`; keep `debug_logging`
  as a back-compat alias that sets this to `debug`).
- `log_categories =` — comma overrides, e.g. `LOOTDISK=off,GEOF=debug,KBDIAG=trace`. Parsed into the table
  at load. Scales to any tag without adding schema entries.

### Migration (the large, mechanical part — ~300–400 sites)

- Convert **noisy `info` categories** (the per-refresh / per-frame / build-diagnostic tags) to `GLOG_*`
  with a default of `debug`/`off`. Leave `warn`/`err` and one-shot boot/init `info` as plain spdlog (or a
  `General`/on category). Do NOT churn the 187 warn + 89 err.
- Partition by file, mechanical, patterned → sonnet subagents, but **serialize the build** (single repo,
  one `build/` — 2 concurrent builds clobber; use worktrees if parallelizing).
- After each file batch: `ninja -C build-linux MapForGoblins` AND
  `ninja -C build-linux-hotreload MapForGoblins goblin_overlay_render` (host↔render boundary must stay green;
  render-side files migrate too, so verify the bridge still compiles).

## Phase 2 — crash-artifact / logs-dir retention — LANDED 2026-07-23

Crash system itself is structurally fine (triage `.txt` always + minidump only if the fault is in our DLL +
freeze/load watchdogs). The problem is accumulation: `MapForGoblins_{crash,freeze,load_stall}_<pid>.{txt,dmp}`
pile up forever — `archive_and_rotate()` only touches `MapForGoblins*.log` + `flagcapture.txt`.

- DONE: extended `is_session_log()` in `goblin_log_archive.cpp` to ALSO fold auto-generated crash artifacts
  (`MapForGoblins_{crash,freeze,load_stall}_*.{txt,dmp}`) into the per-session zip + prune, same `keep_n=20`.
  Matched by NAME PREFIX, so it never sweeps user-named `MapForGoblins.log.<label>` baselines or dev probe
  dumps (`*.bmp`, `paramdump_*.txt`, `markers.csv`, `MapForGoblins_wmprobe.log`).
- **Deferred (next-session candidate):** the archive zip writer is store-only (uncompressed) → `archive/`
  was ~60 MB. Switching to real deflate is a separate, larger change to the self-contained zip writer in
  `write_store_zip()`. Also unhandled: the 4×5.9 MB `vmap_slice*.bmp` / `spawn_before.bmp` dev probe dumps
  are deliberate captures (left alone) but are the biggest single logs-dir consumers if stale.

## Risk / sequencing

- Phase 0: done, both builds green.
- Phase 1 infra (header + config): additive, low-risk, coexists with plain spdlog → land + build first.
- Phase 1 migration: high-churn (hundreds of edits, both builds must stay green each batch) → the real cost.
- Phase 2: self-contained, independent of Phase 1.
