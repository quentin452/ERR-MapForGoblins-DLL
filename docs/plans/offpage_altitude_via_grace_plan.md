# Plan: off-page altitude badge via nearest grace

Status: NOT STARTED — design locked, ready to implement. Created 2026-06-30.

## Problem

The altitude badge (`draw_altitude_badge`, map_renderer.cpp) shows ▲/▼ when a marker sits above/below
the **player** (`worldY − g_player_world_y`). It is gated to the player's current map page
(map_renderer.cpp:345: `group != g_player_group → return`) because `worldY` is block-local and the
player Y only makes sense on the player's own page. So markers on a DIFFERENT page (an underground area
the player isn't in) get NO altitude cue at all — the gap this plan fills.

## Decisions (user)

- **Reference = nearest grace, automatic, NO cursor.** Each off-page marker badges relative to the
  nearest grace (XZ) in its SAME area, precomputed at build. Always-on, zero interaction.
- **Distinct badge, reusing the altitude triangle.** Same ▲/▼ primitive as the player-relative badge,
  but a DIFFERENT tint when grace-relative, so the changed reference is visible at a glance.

## Why it's sound

- Markers already carry `worldY` (MSB block-local posY) and `raw_area`.
- Grace Y exists in the live data (`BonfireWarpParam` row has posX/**Y**/Z); the capture currently
  drops Y. The XZ legacy-fold / dungeon-translation in `capture_live_graces` does NOT touch Y, so a
  grace's posY stays block-local — the SAME frame as a marker's `worldY` **within one area**. Hence the
  hard rule: reference grace must be the **same `area`** as the marker (not just same page/group).

## Steps

1. **`LiveGrace += float posY`** (goblin_inject.hpp) and capture `row.posY` in `capture_live_graces`
   (goblin_inject.cpp, alongside `row.posX, row.posZ`). Do NOT fold/translate Y. Confirm the BonfireWarp
   row struct exposes `posY` (between posX/posZ); add the field/offset if missing.
2. **Grace marker Y** (grace_layer.cpp): set `m.worldY = e.posY` (currently unset → 0).
3. **`Marker += float ref_grace_y = 0; bool has_ref_grace = false;`** (marker_layer.hpp).
4. **Precompute at build** (map_entry_layer.cpp, in `build_buckets_impl`, AFTER graces are captured):
   build a per-area index of grace (x,z,y) from `live_graces()`, then for every non-grace marker find
   the nearest grace in its `raw_area` (XZ distance) and set `ref_grace_y` + `has_ref_grace`. Skip if no
   grace in that area. Verify `capture_live_graces()` runs before the bucket build; if not, compute
   lazily/once when both are ready.
5. **Badge** (`draw_altitude_badge`): pass the marker (or ref). 
   - On the player's page (`group == g_player_group`, player Y valid) → `worldY − player_y`, current
     warm/cool tint.
   - Else if `has_ref_grace` → `worldY − ref_grace_y`, DISTINCT tint (grace-relative, e.g. green above /
     teal below). Reuse the same triangle geometry + deadzone.
   - Else skip.
6. **Tooltip (optional, low cost):** when grace-relative, note the reference (e.g. "▲ 14 m above grace").

## Open / risks

- BonfireWarp row `posY` field/offset — confirm in the row struct (likely already there next to X/Z).
- Build ordering: graces captured before `build_buckets_impl`. If not guaranteed, precompute on first
  render once `live_graces()` is non-empty (cache the result; markers in g_buckets are rebuilt rarely).
- Multi-level areas: nearest-by-XZ grace may be on a different vertical level → a misleading delta.
  Accepted for v1 (nearest is the best cheap heuristic; a later pass could pick nearest in 3D).
- No grace in a marker's area → no badge (acceptable; rare).
