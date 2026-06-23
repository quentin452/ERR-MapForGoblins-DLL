# RE brief — map-fragment FOG-OF-WAR reveal state (gate the overlay markers)

## Goal
Our ImGui-rendered world map (`src/worldmap/`) draws marker icons on the **foreground
draw list**, ON TOP of the native map — so it ignores the game's fog of war. With
`require_map_fragments = true` and ZERO fragments discovered, icons still leak onto
UNDISCOVERED (fogged) tiles. The native map never shows them because the engine's
renderer skips icons on un-revealed map pieces, regardless of any flag.

`require_map_fragments` is currently a coarse approximation (a baked tile→fragment table,
`goblin/goblin_map_tiles.hpp` `MapList` + `GetMapFlagFromTile`). It only covers tiles
enumerated in `MapList`; tiles absent from it return flag 0 = "always show" = the leak.

We need the **real per-piece reveal state** so the overlay can skip a marker whose map
piece is still fogged. This is NOT DLC-specific (it applies from the Limgrave start too).

## What we already know (from docs/re/windows_re_live_refresh_grace_lead.md)
- `FUN_1408882d0(toggleByte, areaIdx)` (RVA `0x8882d0`) walks `CS::WorldMapPieceParam`
  rows (lookup `FUN_140d56d90`, **row id = `areaIdx*100 + i`, i in `0..0x1f`** = up to 32
  pieces per area) and returns a **32-bit "revealed pieces" mask** (bit i = piece i
  revealed). When the toggle byte (`0x143d6cfc0`, NewMenuSystemWarp2) ≠ 0 it returns
  `0xffffffff` (all revealed); else it computes the legit set from the pieces' flags /
  `_GetMapVariation`.
- `FUN_1408890b0` (RVA `0x8890b0`) diffs old-vs-new mask stored at
  `[manager + 0x39c + idx*4]` and incrementally creates the newly-revealed piece display
  objects. Per-frame diff-reconcile.
- Event-flag read we already use (replicate, don't re-RE): AOB `IS_EVENT_FLAG`
  `48 83 EC 28 8B 12 85 D2` + the event-flag manager slot (see `src/re_signatures.hpp`),
  wrapped as `goblin::ui::read_event_flag(uint32_t)`.

## Two candidate implementations — tell us which is cleaner/safer
### Path A (preferred if viable) — read `WorldMapPieceParam` live + per-piece flag
Replicate what `FUN_1408882d0` computes, in pure data:
1. Read `WorldMapPieceParam` live (same `from::params` path we use for
   `WorldMapPointParam`). **Q-A1: dump the `WORLD_MAP_PIECE_PARAM_ST` paramdef** — exact
   field names + byte offsets, especially: the piece's **reveal event flag** (the
   "openEventFlagId"/equivalent the engine tests), and the piece's **map geometry**
   (areaNo + gridX/gridZ position + grid width/height, or whatever defines the tile
   rectangle the piece covers).
2. **Q-A2: how does a map TILE (overworld gridX,gridZ — or our map-space U,V) map to a
   piece row?** Confirm whether the piece row carries position+extent we can test a tile
   against, OR whether the engine derives the piece index some other way (e.g. a fixed
   grid of `areaIdx*100+i`). We need the exact tile→piece function the engine uses.
3. **Q-A3:** ERR remaps these flags (per memory). Reading the LIVE param's flag id gives
   the current (ERR-remapped) value — confirm that's what the engine actually tests at
   reveal time (i.e. no separate indirection we'd miss).

### Path B (fallback) — call/read the engine's computed mask
1. **Q-B1:** a VMP-resilient AOB for `FUN_1408882d0` (or its container `FUN_1408890b0`)
   and the manager base holding `[manager+0x39c + idx*4]` masks, so we can READ the mask
   (do NOT write) per area without calling into VMP'd code if avoidable.
2. **Q-B2:** the `areaIdx` → our area-number mapping (we use 60 overworld / 61 DLC /
   12 underground …). How does `areaIdx` (the `*100` multiplier key) relate to those?
3. Still needs the same tile→piece-index mapping as Q-A2.

## The crux (both paths)
**tile → piece**: for a marker at overworld tile (gridX,gridZ) [base/underground/DLC],
which `WorldMapPieceParam` piece (areaIdx, i) covers it? This is the one piece of geometry
we cannot guess — please nail it exactly (field offsets or the engine's index formula).

## Deliverable
- The `WORLD_MAP_PIECE_PARAM_ST` field layout (Path A) and/or the AOB + mask-read recipe
  (Path B).
- The exact tile→piece mapping.
- A recommendation: A or B, with the safety trade-off (param-read vs touching VMP code).

We will then, mod-side: build a tile→reveal-flag (or tile→piece-bit) table once, and in
`src/worldmap/map_renderer.cpp` skip a marker whose piece is unrevealed when
`config::requireMapFragments` is on. Keep the toggle; this becomes its real backing.
