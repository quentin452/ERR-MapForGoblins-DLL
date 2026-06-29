---
name: fragment-gate-maplist-gap
description: "require_map_fragments leak fix: hand-authored MapList had interior coverage holes the no-bake disk pass populated → neighbour-majority fallback. Merged."
metadata:
  node_type: memory
  type: project
---

# require_map_fragments leak: MapList coverage gaps (FIXED, merged to master 2026-06-27)

**Symptom:** with `require_map_fragments` on (and the region's map fragment NOT owned), most markers
correctly hide but some stay VISIBLE (user's example: Reforged rune pieces / "éclat runique").

**Root cause (NOT a missing per-pass setter — the user's first guess):** `push_marker` ALWAYS sets
`m.fragment_flag = marker_fragment_flag(tile)` for every marker. The gate is `requireMapFragments &&
m.fragment_flag && !flag_owned` (map_renderer.cpp ~1350) — it **short-circuits to "show" when
fragment_flag == 0**. `GetMapFlagFromTile` (goblin_logic.cpp) returns 0 when the tile isn't in the
**hand-authored** `MapList` (src/goblin/goblin_map_tiles.hpp). That table was built for the static
bake's marker set; the **no-bake disk pass places markers on a few INTERIOR overworld tiles MapList
omits** → fragment_flag=0 → leak. (Offline: only 5 rune-piece tiles / 15 pieces, all distance-1 holes.)

**Fix (merged, commit c5399dd):** in `GetMapFlagFromTile`, for an overworld tile (X==60||61) with no
exact match, inherit the MAJORITY map-fragment of its 8 immediate (±1) neighbours. Interior holes take
their region's fragment; a genuinely isolated tile (ocean / uncovered DLC) has no covered neighbour →
stays 0 (always-shown default preserved, no regression). Build-time only (push_marker / cluster
labels), never per-frame.

**Followup (task_065530a2, low priority — "very few markers"):** tiles **distance>1** from any covered
tile still leak (a whole uncovered sub-region / DLC area the hand table never listed). Close by either
a build-time diag logging every marker with marker_fragment_flag()==0 then adding the tiles to MapList,
OR the real fix = read the game's OWN tile→map-fragment mapping LIVE (see [[worldmap-unsearched-fog-mask]]
fog oracle FUN_140886560) to replace the hand table entirely.
