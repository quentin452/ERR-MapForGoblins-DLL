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

---

## Boss enemy-supplement OOB leak — sub-map bosses pile off-map (FIXED colosseum 2026-07-23)

`build_live_bosses` (map_entry_layer.cpp) has an ENEMY-SUPPLEMENT pass: for every parsed MSB enemy whose
model resolves to an ALREADY-marked boss name, it emits a boss marker at the enemy's raw `(area,gx,gz)`.
It had NO check that the tile projects to a real overworld spot, so bosses in non-overworld sub-maps piled
off-map. Two live examples (both model **c2500 Crucible Knight**, found via `vmap ename`):
- **m45 Royal Colosseum** (c2500 ×2) — `legacy_fold` DECLINES (`matched=0`) → snapped to map ORIGIN `(0,0)`.
- **m11_10 Roundtable Hold** (c2500 ×2) — folds via a close conv row to overworld `~(7676,8546)`; the WHOLE
  hub (Enia/Hewg merchants too) projects there.

**Fix (gate in the supplement loop):** for a sub-map source area (`!legacy_fold::is_terminal(area) &&
area!=12`), compute `legacy_fold::fold(...)` and `continue` (skip the supplement) when `!matched` OR
`max_fallback > 3`. Added `Folded::max_fallback` (worst nearest-base grid distance across the fold chain;
0 = all exact rows) + `lookup()` out-param. Marked `fold()` `GOBLIN_RENDER_API` (render `map_entry_layer`
now calls host `legacy_fold`; both split DLLs green). **Live-verified:** the m45 colosseum `(0,0)` Crucible
Knights are GONE; legit world Crucible Knights (Ordovis m30, Devonia m61, Hirnan/Siluria m12, area60…) kept.

**STILL OPEN — Roundtable m11_10:** it folds LEGITIMATELY (`matched=1`, `max_fallback≤3`) to overworld
area 60, so the projectable gate does NOT catch it (by design — it IS projectable). Dropping it is a HUB
policy question, not a supplement bug: ERR's `WorldMapLegacyConvParam` has a conv row placing Roundtable
onto the overworld at ~(7676,8546), and ALL m11_10 content (merchants + bosses) follows. Note the existing
`marker_fragment_flag` unmappable-area hider (goblin_world_position.cpp:1099) keys on the FOLDED area, which
is 60 here → it slips through. Decide with the user whether m11_10 (Roundtable Hold) should be a
non-mappable hub across ALL passes (like its grace already is) before hiding it.
