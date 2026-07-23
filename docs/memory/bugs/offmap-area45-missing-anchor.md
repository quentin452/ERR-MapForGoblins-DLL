# Off-map markers: ERR area 45 has no dungeon→overworld anchor

**Status:** root cause re-diagnosed live 2026-07-23 (the earlier `dungeon_to_world.json` root cause was
WRONG — that file is offline-only). FIX pending: a mod-agnostic fallback anchor needs the overworld→area-45
ENTRANCE position, which is NOT currently parsed. See "Fix path" for the real options.

## Symptom

Some markers render "off the map" — piled a few units from the world origin. Caught with the new
vmap **marker extractor** (`Extract region` dev button → `[VMEXTRACT]` in `logs/MapForGoblins.log`,
`near0` flag). A selected off-map cluster logged 4 markers, all `raw_area 45`, group 0:

```
near0,0,48,45,45,0.0,-0.3,31423700     # WorldBosses
near0,0,61,45,45,1.8,-23.2,900301540   # WorldStakesOfMarika (ERR name_id 900xxx)
near0,0,61,45,45,-3.1,-23.1,900301540   # WorldStakesOfMarika
near0,0,62,45,45,4.7,-26.0,900301690   # WorldSummoningPools
```

The tiny worldX/worldZ (1.8, -23, …) are **block-local** coords (`posX/posZ`) with no area→world offset.

## Root cause (re-diagnosed live 2026-07-23 — supersedes the json theory)

Area 45 is a **dungeon** carrying **ERR-added** content (name_ids `900xxx` = ERR custom text).

**The runtime does NOT use `data/dungeon_to_world.json`.** That file is read only by
`tools/rune_piece_map.py` (an offline rune tool) — grep proves it: zero references in `src/`. The earlier
note blaming it for on-map placement was wrong. The DLL projects every dungeon marker through the **LIVE**
converter: `marker_world_pos` → `project_dungeon_row_to_overworld` (`src/goblin_world_position.cpp:110`) →
**`goblin::legacy_fold::fold`** (`src/goblin_legacy_fold.cpp`), which reads the resident
**`WorldMapLegacyConvParam`** regulation directly (full-block key `area<<16|gx<<8|gz`, chained folds, no
bake). `virtual_anchor_fix` is only a pre-fold *inset* remap (Roundtable-class) built at runtime from live
graces — not the converter.

Area 45 piles at origin because **ERR's `WorldMapLegacyConvParam` has no fold row/chain for area 45.**
`legacy_fold::fold(45,…)` returns `matched=false` with `available()==true` → `project_...` returns false →
the marker keeps its block-local `posX/posZ` (~origin). ERR added the dungeon but never registered its
overworld projection.

**Live verification (2026-07-23, in-world):**
- `proj 10 0 0` → `u=3331 v=6810 page=0` (converter warm); `proj 45 45 45` → `converter unresolved or
  declined`. (Note `proj` tests `worldmap_probe::project` = base affine only; it also declines 34/39/42/43,
  which DO project via `legacy_fold` chains — so `proj` alone is not the marker path.)
- Real marker path: `vmap find 900301540` (Stakes of Marika) → instances across areas 10–43 land onmap,
  but the **area-45** ones sit at `w(2,-23) / w(-3,-23) / w(5,-26)` = still block-local origin-pile.
  So `legacy_fold` covers 34/35/42/43 but NOT 45.

Corollary: **no area-45 marker of any type is projected** (loot, boss, stake, grace all share the one
failing fold), so there is no already-placed area-45 marker to borrow an anchor from — the grace is not
special. Diagnosed with the new `vmap ename` / `vmap find` + `proj` verbs.

## Fix path — mod-agnostic direction (the real options)

The gap is a **missing overworld anchor for area 45**, and no runtime source currently supplies it:

1. **Mod-agnostic (preferred, but a real feature — not a quick patch):** at runtime, build an
   `area → overworld-entrance-position` map and use it as the fallback anchor whenever `legacy_fold`
   declines an area. The entrance position is overworld-frame and independent of the missing conv param.
   BLOCKER: the source isn't parsed today — `msbe_parser` reads only Enemy(type 2) + Asset(13) parts, not
   **ConnectCollision** (type 5), and the overworld→dungeon transition usually lives in an **EMEVD warp
   event** (WarpPlayer target `m45…`, triggered by the cave-mouth asset whose overworld pos we'd anchor to)
   rather than a plain MSB field. So this needs its own RE (which source carries the m45 link + its pos)
   + an msbe/EMEVD parse extension + a fallback branch in `project_dungeon_row_to_overworld`.
2. **Interim, ERR-specific:** hardcode a single `m45` overworld anchor (via `virtual_anchor_add` or a small
   fallback table) once its entrance world pos is known. Fast, but patches only this one ERR area — violates
   the mod-agnostic prime directive.
3. **Stopgap:** keep the `near0`/off-map flag hiding these markers so they don't clutter the origin.

**Do NOT** re-add area 45 to `dungeon_to_world.json` — that file has no runtime effect.

Related: `src/goblin_world_position.cpp` (`project_dungeon_row_to_overworld` / `virtual_anchor_fix`),
`src/goblin_legacy_fold.cpp` (`WorldMapLegacyConvParam` fold), `src/worldmap/msbe_parser.cpp` (part-type
parse to extend), the m19 Chapel "no converter accepts it" note in
[marker-sections-spoiler-clustering](../features/marker-sections-spoiler-clustering.md).
