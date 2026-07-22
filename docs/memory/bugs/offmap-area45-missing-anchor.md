# Off-map markers: ERR area 45 has no dungeon→overworld anchor

**Status:** root cause found 2026-07-23, FIX pending (needs area-45 entrance world coords).

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

## Root cause

Area 45 is a **dungeon** (dungeon areas span 10–45) carrying **ERR-added** content (name_ids `900xxx`
= ERR custom text). Dungeon interiors are placed on the overworld by a static anchor table
**`data/dungeon_to_world.json`** (keys `m<area>_<block>`). That table has anchors for areas
**[10, 11, 12, 14, 16, 30, 31, 32, 34, 39] only — NOT 45**. Area 45 is also absent from the vanilla-
derived `data/map_name_regions.json` and `data/WorldMapPointParam.json` (0 rows) — it is ERR content,
not in the shipped data. With no anchor, `marker_world_pos(area=45, …, conv_underground=true)` returns
the block-local position → the marker lands at ~origin.

## Fix path (later)

1. Get area 45's **overworld entrance** world position (where the dungeon connects to the map) — from
   the running game (RPC: dungeon entrance / ConnectCollision / WarpPoint) or an ERR MSB extract.
2. Add an `m45_XX` entry to `data/dungeon_to_world.json` with that anchor.
3. Rebuild.

**Test first:** open the ER map + move so the LIVE engine converter runs (`worldmap_probe::project`).
If area 45 markers snap into place, the engine knows the converter and the static anchor is only a
fallback gap; if they stay at origin, ERR area 45 has no engine converter → the manual anchor is
required. Mod-agnostic note: the general fix is to make the live projection cover ANY area, with the
static table as fallback — hardcoding m45 only patches this one ERR area.

Related: `data/dungeon_to_world.json`, `marker_world_pos` / `virtual_anchor_fix`
(`src/worldmap/map_entry_layer.cpp`), the m19 Chapel "no converter accepts it" note in
[marker-sections-spoiler-clustering](../features/marker-sections-spoiler-clustering.md).
