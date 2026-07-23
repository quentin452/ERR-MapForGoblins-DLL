# Mod-agnostic dungeon-entrance fallback anchor (fold-declined areas)

**Status:** SCOPED 2026-07-23, not started. Grounded in a live diagnosis + a code map (see
[offmap-area45-missing-anchor](../memory/bugs/offmap-area45-missing-anchor.md)).

## Problem

A dungeon area that `goblin::legacy_fold` DECLINES has no overworld anchor, so ALL its markers keep
their block-local `posX/posZ` and pile at the world origin. Confirmed for **ERR area 45**: every
area-45 marker (loot, boss, Stakes of Marika, Summoning Pool, grace) shares the one failing fold and
lands at `w(2,-23)`-ish. Vanilla dungeons don't hit this because `WorldMapLegacyConvParam` carries their
conv row; ERR added area 45 without registering one.

Runtime path today: `marker_world_pos` → `project_dungeon_row_to_overworld`
(`src/goblin_world_position.cpp:110`) → `legacy_fold::fold` (reads `WorldMapLegacyConvParam`). When
`fold` returns `matched=false` with `available()==true`, the row is left unprojected (origin pile).
(`data/dungeon_to_world.json` is NOT involved — offline rune tool only.)

## Goal

When `legacy_fold` declines an area, fall back to that dungeon's **overworld ENTRANCE** position — an
overworld-frame anchor independent of the missing conv param. Mod-agnostic: works for any mod-added
dungeon, not just ERR area 45.

## Why the easy sources fail (verified 2026-07-23)

- **`WorldMapPointParam` dungeon-entrance icons** (iconIds 4/13/14/15/16/230/231/234) DO carry an
  overworld `areaNo/gridXNo/gridZNo/posX/posZ` (`WORLD_MAP_POINT_PARAM_ST.hpp:148-162`) — but there is
  **NO target-map / target-area / target-entity field** on the row (only `eventFlagId`). So an entrance
  icon can't be directly linked to the dungeon interior it opens.
- **EMEVD portal parse** (`msbe_parser.cpp:653 parse_emevd_portal_gates`) captures the gate `entityId`
  only (`arg[2] @ a+8`) — **no destination map id** in what it reads.
- **Graces** go through the SAME `marker_world_pos`→`legacy_fold` path (`grace_layer.cpp:39`), so an
  area-45 grace fails identically — no free anchor to borrow.

So the only rigorous overworld→dungeon-area link is the **EMEVD warp event** that teleports the player
from the overworld trigger into `m45` — and its destination-map arg is not parsed today.

## Mechanism

Reuse the existing shapes:
- **Anchor value = a `ConvRow` dst** (`goblin_legacy_fold.cpp:16-21`): `dst_area,dst_gx,dst_gz,dst_px,
  dst_pz`. Entrance world = `dst_gx*256 + dst_px`, `dst_gz*256 + dst_pz` (`legacy_fold.cpp:183-184`).
  The fallback must synthesize exactly this for a declined `src_area`.
- **entityId → position join** already exists (`map_entry_layer.cpp:1008-1042`, `ent_enemy` /
  `resolve_pos`); reuse it for the overworld trigger asset (Asset parts carry `entityId` @part+0x60 +
  block-local pos, `msbe_parser.hpp:53,64-70`).

## Slices (incremental — infra is mod-agnostic even before the auto source lands)

### Slice 1 — fallback-anchor plumbing (no RE, mod-agnostic)
Add a runtime `area → EntranceAnchor{dst_area,dst_gx,dst_gz,dst_px,dst_pz}` table and consult it in
`project_dungeon_row_to_overworld` right where `fold` declines (after `legacy_fold.cpp` returns
unmatched, before the `return false`). If the area has a fallback anchor, translate the row's
block-local pos onto the entrance and return true. Empty table = today's behaviour (safe no-op).
- Files: `src/goblin_world_position.cpp` (the fallback branch), a small `entrance_anchor` store
  (either in `goblin_legacy_fold.*` next to the fold, or a new `goblin_entrance_anchor.*`).
- Decide placement per the package rule: this is generic infra → core/lib namespace, not under any
  consumer. The store is host-side data (like maptile/vworld) so mark its API `GOBLIN_RENDER_API`.

### Slice 2 — seed the table (pick a source; two candidates, verify before committing)
- **(2a) EMEVD-flag correlation** — `WorldMapPointParam` entrance rows have `eventFlagId`; ER event
  flags are area-encoded. If an entrance icon's `eventFlagId` decodes to the dungeon's area, join
  entrance-icon overworld pos → area. Cheapest IF the encoding holds for ERR; **verify** on known
  dungeons first. Mod-agnostic if the flag convention is followed.
- **(2b) Manual/config seed (interim, ERR-specific stopgap)** — one `virtual_anchor_add`-style entry
  `m45 → entrance pos` from a config line, once the entrance pos is read live. Rides Slice-1 infra;
  fixes area 45 now; not general. Use only to unblock while 2a/Slice-3 is scoped.

### Slice 3 — auto-populate from EMEVD warp (full mod-agnostic)
Extend EMEVD parsing to capture the warp's **destination map** + its **trigger entity**, then join the
trigger's overworld position (via the `ent_asset` map) → `area = dst map's AA`. Build the entrance table
for every dungeon automatically.
- Files: `src/worldmap/msbe_parser.cpp` (new `parse_emevd_warps` capturing dest-map + trigger),
  `src/worldmap/map_entry_layer.cpp` (build the entrance map + entity join).

## First RE probe (de-risk Slice 3 — do this before writing parse code)

Pick a KNOWN mapped dungeon that already has a `ConvRow` (e.g. a vanilla catacomb, area 30). Find its
overworld→interior EMEVD warp; learn (a) which instruction/template carries the destination map, (b) its
arg layout, (c) how the trigger's overworld position is reached. **Validate**: the derived entrance
(`trigger overworld pos`) must match that dungeon's `ConvRow.dst` (`dst_gx*256+dst_px`, …). If it
matches on 2-3 known dungeons, the method is proven → apply to `m45` (no ConvRow) with confidence.

## Verification

- `vmap find 900301540` (Stakes of Marika): the area-45 instances move from `w(~2,-23)` onto the real
  overworld dungeon location; `proj`/extractor `near0` flag clears for area 45.
- Regression: mapped dungeons (10/30/34/…) unchanged (fallback only fires when `fold` declines).
- Add an `[ENTRANCE]` log (or extend `vmap ename`-style diag) listing each declined area → chosen anchor
  + its source (flag-correlation / EMEVD-warp / manual).

## Risks / open questions

- **Warp instruction layout** (Slice 3): ER warp templates vary; some dungeons entered via multiple
  warps (which is "the" entrance?). Pick the warp whose trigger is an overworld (area-60/61) asset.
- **eventFlagId encoding** (Slice 2a): must confirm ERR follows the vanilla area-encoded flag convention
  — if ERR uses ad-hoc flags, 2a won't join.
- **Multi-block dungeons**: `m45` may span several blocks; the entrance anchors the whole area (one pile
  → one place), same simplification `legacy_fold` uses (`legacy_fold.cpp:435` "one dungeon = one pile").
- Keep it a FALLBACK: never override a successful `fold` (mapped dungeons must be untouched).

## Effort

Slice 1 small (plumbing). Slice 2a/2b small-medium (source + verify). Slice 3 medium (EMEVD warp RE +
parse extension). The RE probe gates Slice 3 — cheap and decisive, do it first.
