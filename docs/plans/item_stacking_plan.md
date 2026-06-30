# Plan: item stacking (merge co-located identical-item markers)

Status: IMPLEMENTED v1 2026-06-30 on branch `feat/loot-item-count` (builds + deploys clean; runtime
confirm pending). R=5 m, toggle `stack_identical_items` (default ON). Created 2026-06-30, continuation
of `loot_item_count_plan.md` (per-lot count done; this is the inter-marker merge).

v1 done: `stack_identical_markers()` in `map_entry_layer.cpp` runs at the end of `build_buckets_impl`
(gated on the toggle), connected-components over same-(area,item) markers within 5 m of MSB-local
(px,pz), collapses each component to its first member and SUMS `count` (4 Formic nodes × x1 → x4).
`rebuild_markers()` re-runs the build on toggle (disk source). Menu checkbox in `goblin_overlay.cpp`.
v2 (2026-06-30): per-member depletion DONE. The representative records every member's collected
state (`Marker.stacked` = `StackedMember{row_id, collected_flag, count}`). `marker_done` is now
stack-aware (the stack grays/checks only when ALL members are collected), and the tooltip shows the
REMAINING uncollected count when `collected_graying` is on (x4→x2 after gathering 2; back up on
respawn), the full total when off. Helpers `loot_member_collected` / `stacked_remaining_count` in
`map_renderer.cpp`.

Remaining LIMITATION (minor): rebuild on toggle may re-register graying-tracking entries
(set-deduped, harmless).

## Problem

Several identical gather nodes sit on top of each other in the world. Example: Formic Rock node
`AEG099_852` placed 4× within ≤8 m at Siofra River (`_9000.._9003`). The map draws 4 separate yellow
triangles. The user wants them merged into ONE marker reading the combined count — "Formic Rock x4"
(4 nodes), not 4 loose icons.

This is "clustering ITEM" (merge same-item co-located markers), DISTINCT from the existing tile
"clustering" (generic pile by map-space tile, gated on live projection — KO underground at Siofra).
Item stacking must work UNDERGROUND, so it keys on raw MSB world position, NOT map projection.

NB: NOT a per-lot-count issue — that's fixed (`lot_item_count` now returns the deterministic quantity;
the Formic node is `x1` per node, so 4 merged nodes → `x4`).

## Design

- **Where:** a build-time merge pass in `map_entry_layer`, after all `push_marker` calls have filled
  `g_buckets`, BEFORE the layer is published. World positions (`Marker.raw_px/raw_pz`, plus area) are
  set at push time and are projection-independent → works underground.
- **Group key:** `(area, item identity, world cell)`.
  - item identity = the resolved encoded key (`resolve_loot_item_textid`) or `primaryGoodsId`; only
    merge markers of the SAME item.
  - world cell = union-find / connected-components over markers within radius **R** in (px,pz).
    Connected-components (not fixed-grid snap) so two nodes straddling a grid line still merge. O(n²)
    per (area,item) bucket — counts are tiny (≤ tens).
- **Threshold R:** default **8 m** (covers the Formic cluster; tune). Same-area only.
- **Merge result:** keep ONE representative marker (first member's position/icon/name). Sum member
  `count` → representative `count` (4 nodes × x1 = `x4`). Drop the other members from the bucket but
  keep their row-ids so collected-graying still works (see below).
- **Collected-graying / depletion:** the merged marker stores its member row-ids. The tooltip/badge
  count shows REMAINING uncollected members (mirror the cluster pile's `marker_done` remaining sum),
  so gathering nodes one by one decrements `x4 → x3 → …`. Needs `Marker` to carry a small member list
  (e.g. `std::vector<uint32_t> stacked_rids`) or a `stacked` count + per-rid lookup.
- **Scope guard:** only stack loot-ish categories where identical co-located pickups are expected
  (LootMaterialNodes + lot-backed loot). NEVER stack bosses/graces/NPCs. Optionally a config toggle
  `stack_identical_items` (default on).
- **Interaction with tile-clustering:** the merged marker is a single marker → participates in tile
  clustering normally afterward. No conflict (item-stack runs first, at build; tile-cluster at render).

## Steps (~6 edits)

1. `Marker`: add `std::vector<uint32_t> stacked_rids` (or `int stacked = 1` + a side map). Default empty.
2. `map_entry_layer`: after buckets are built, per (area, item-key) bucket, union markers within R,
   collapse each component to its representative (sum counts, record member rids), erase the rest.
3. Tooltip/count: representative shows `xN` where N = remaining-uncollected member count (graying-aware)
   or total when `collectedGraying` off. Reuse the pile remaining logic.
4. Graying: a stacked marker is "done" only when ALL members are collected; partial → still shown,
   count decremented.
5. Config: `stack_identical_items` toggle (category_meta), default on.
6. Verify in-game: Siofra Formic cluster → one "Formic Rock x4"; gather one → `x3`.

## Open decisions (confirm before impl)

- R = 8 m default? (or 5 / 10)
- Count semantics on a stack: number of NODES (x4) — confirmed by user ("y a que 4 Formic Rock").
- Always-on vs config toggle.
