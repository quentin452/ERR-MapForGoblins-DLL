# World-map open freeze — diagnosis

**Symptom.** Opening the in-game world map freezes for ~6 s; closing it causes
smaller micro-freezes. The freeze predates all of today's marker work (it was
already present with far fewer icons), so it is not caused by any recent data change.

## Method

Added `[BENCH]` scoped timers (`src/goblin_bench.hpp`, RAII `ScopedTimer` /
`GOBLIN_BENCH("label")`, logs `[BENCH] <label>: <ms> ms` through the existing spdlog
logger) around every init and refresh phase. Read them in-game:

```
grep "\[BENCH\]" <DLL folder>/logs/MapForGoblins_<date>.log
```

Then ran a category-toggle gradient (via `show_all` / `show_all_except` in the ini)
to vary how many rows the mod injects, and timed the map open at each row count.

## What the mod costs — nothing

From a full run (`init.heavy.total` wraps every init step):

```
init.heavy.total:     ~20–31 ms     ← ALL of the mod's one-time work
  map.inject.total:     2–8 ms      (pointer-swap of the param table)
  apply_map_logic:      <3 ms
  setup_messages:      11–15 ms
refresh.* (per tick):  ~0.04 ms      (the 100 ms / 2 s background loop)
```

The mod does its heavy per-row work **once at init** (after `load_delay`), inside
`setup_mod` (`dllmain.cpp:124-133`). It installs **no runtime hooks** — `enable_hooks()`
runs an empty queue. Injection is a one-shot **pointer swap** of `WorldMapPointParam`'s
backing file (`goblin_inject.cpp:443-449`, `set_param_injection_active`
`goblin_inject.cpp:981`): the vanilla ~740-row table is replaced by the expanded table,
which then stays resident.

So on map open/close the mod runs nothing. **The freeze is entirely game-side.**

## Root cause — the game re-processes every param row on each open

The expanded `WorldMapPointParam` stays resident, so every time the player opens the
world map the **game** walks the whole table to build the marker UI. Cost scales with
the number of rows in the table, not with how many icons are actually drawn.

Confirmed: with `require_map_fragments = true` the freeze still happens, and the mod
logs `0 hidden at inject` — fragment-gating hides icons via `eventFlagId` at *draw*
time, but the rows stay in the table and the game still processes them. **Display
gating (fragments, zoom step) does not help.** Only the row *count* matters.

### Row count → freeze (measured)

| rows in param (vanilla 740 + injected) | map-open |
|---:|---|
| 740 (mod icons all off) | instant |
| 1031 (bosses only, +291) | instant |
| 7635 (+6895, dense loot dropped) | ~3 s |
| 9692 (+8952, everything) | ~6 s |

The curve is **superlinear**: the first ~6600 injected rows add ~3 s (~0.45 ms/row),
but the next ~2000 rows add another ~3 s (~1.46 ms/row). Most likely an O(n log n)
sort / spatial-index rebuild or cache-pressure effect in the game's map UI. Practical
consequence: **trimming the densest categories pays off disproportionately** — dropping
just the bulk loot categories (−2057 rows) already halved the freeze (6 s → 3 s).

## Why region-lazy injection is the wrong fix

A tempting idea is to inject only the markers near the player and re-inject on region
change. It does not fit this problem:

1. **The ER overworld map is a single pannable page** (area 60). The player scrolls and
   zooms across the whole map in one session, so lazy-loading by player position would
   hide markers in regions they pan to — it breaks the feature. This is not a
   tile-streamed minimap.
2. The mod has **no current-region / player-position reader** (would need a new AOB) and
   the expanded table is **built once and cached** (not subsettable at runtime without
   a refactor). `CSMenuMan+0xCD` (map-open state) is AOB-resolved and a 10 ms poll loop
   exists, so map-open *detection* is cheap — but detection does not help, since the
   cost is the open itself.
3. No precedent in the ER modding community; it would mean reverse-engineering the
   map-open path from scratch.

## Recommended fix — reduce the resident row count

This matches what the mod's own authors shipped (a "general icons" version "to reduce
the stutter when opening the map") and how the community trims `WorldMapPointParam` in
Smithbox. Because the cost is superlinear, cutting the high-count, low-navigational-value
loot categories gives the biggest win for the least lost content.

**Direction:**
1. Change the **default** category toggles so the densest loot is off out-of-the-box —
   candidates: Crafting Materials, Golden Runes (+Low), Smithing Stones (Low),
   Consumables, Gloveworts (+Great), Greases, Throwables, Ammo, Stat Boosts, Material
   Nodes, Utilities. Keep navigation/high-value markers on (bosses, graces, stakes,
   spirit springs, summoning pools, maps, key items, equipment, magic, rune arcs, …).
2. Document the perf/coverage trade-off (README + ini comments) so users can opt back in.
3. (Optional, larger) Investigate the superlinear per-row cost itself — if it is the
   game sorting/indexing the table per open, a cheaper row layout or a hooked map-open
   path *might* cut per-row cost without losing markers, but that is unproven and
   high-effort.

The target resident-row budget for a sub-~1 s open is being pinned with one more
gradient point (~4000–4500 rows); the recommended default set will be locked to that.

## Artifacts

- `src/goblin_bench.hpp` + `[BENCH]` timers across `dllmain.cpp`, `goblin_inject.cpp`,
  `goblin_logic.cpp`, `goblin_collected.cpp`, `goblin_kindling.cpp`, `goblin_messages.cpp`
  (commit fc95786). Timers only — no behavior change.
- Related: `docs/ersc_hosting_and_map_autohide.md` (the retired "expand only while map
  open" auto-hide, which used `CSMenuMan+0xCD`).
