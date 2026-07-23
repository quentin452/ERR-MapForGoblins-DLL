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

## Off-grid template tiles (grid > 0x3F) piled OOB — universal push_marker guard (FIXED 2026-07-23)

ERR parks its Roundtable Hold COPY at **m31_90 (grid 90)** — grid 90 is OUTSIDE the engine's tile grid
`0..0x3F (63)` (both `WorldMapLegacyConvParam` and the fold's out-of-range clamp cap at 0x3F). m31_90 has
no conv row (fold declines) AND no EMEVD file (dead template), so its WHOLE region snapped OOB to
~(22700,-280): captured via `vmap` region-extract as **32 area-31 markers** (landmarks 112200000 /
graces 200721100 / summoning 900301540 / dungeon-entrance icons 500800010 / the 2nd Mad Tongue Alberich).
These come from MANY passes (live WorldMapPointParam landmarks/graces + disk), so the entity-home loot
filter ([[disk-parser-coverage-gaps]]) covered only the Treasure slice, not these.

**Fix (universal, `push_marker` in map_entry_layer.cpp):** every marker source funnels through push_marker,
so one guard there — `if (d.gridXNo > 0x3F || d.gridZNo > 0x3F) return;` — drops the phantom tile across
ALL passes. NOT a magic coordinate: 0x3F is the engine's own tile-grid max. On-grid declines (area-45
colosseum at grid 0) and low-grid underground (area 12) are untouched; no legit tile exceeds 0x3F
(overworld maxes ~58).

**Live-verified:** `vmap find Alberich` = 1 (was 2: the m31_90 dup at (22776,-274) gone); Codex still 1;
Leyndell Alberich armor (grid 0) + area-45 origin markers kept. The region-extract at ~(22700,-280) that
had 32 markers now clears.

---

## push_marker early-return CRASHED the smithing pass → moved to a post-build sweep (FIXED 2026-07-23)

The off-grid `grid>0x3F` guard + the m33 Trial-arena filter were first put as EARLY RETURNS inside
`push_marker`. That crashed the build worker (`0xC0000005` in `build_disk_smithing_markers` → `cell_of`,
symbolized via `tools/resolve_crash.py`): **many builders do `g_buckets[cat].back()` immediately after
`push_marker`** (smithing/elevator/spring/… read the just-pushed marker's cell). ERR's m33 Trial arena has
a "Smithing Anvil" → `push_marker(area33)` returned early → `.back()` on an EMPTY smithing bucket = UB.

**Fix:** removed BOTH guards from push_marker; drop phantom-tile markers in ONE post-build sweep in
`build_buckets_impl` (after every builder, before `annotate_item_stacks`), erasing markers with
`raw_gx>0x3F || raw_gz>0x3F || raw_area==33` from every g_buckets bucket. The `.back()` contract holds.
Graces are filtered separately at `capture_live_graces` (their own layer, no `.back()`).

**Live-verified:** no crash; `pruned 33 phantom-tile markers`; Cipher Pata / Codex = 1 (real m11_10),
Mad Tongue Alberich = 1, all m31_90 + m33 copies gone. See also
[[disk-parser-coverage-gaps]] (entity-home loot filter) — the two together cover the ERR Roundtable copies.

### m45 Royal Colosseum added to the arena prune (2026-07-23)

Same class as m33: the vanilla **Royal Colosseum** arena interior (m45 — 2 c2500 Crucible-Knight dummies,
c4191 Tear Scarabs, a "Menu" NPC 10000300, the "Royal Colosseum" landmark) DECLINES the fold → piles at
origin (0,0). It is a teleport-only PvP arena, not an overworld location (vanilla shows only its entrance
icon). Added `raw_area==45` alongside `raw_area==33` to the build_buckets_impl phantom-tile prune + the
capture_live_graces grace filter. Live: `pruned 37 phantom-tile markers`; the area-45 (0,0) Crucible Knight
+ its co-located items are gone; the real world Crucible Knights (m30/m60/m11) stay. NOTE: only the Leyndell
colosseum (m45) is handled — if the Limgrave/Caelid colosseums also leak, add their area ids too.

---

## Two co-located graces at the Roundtable — NOT a bug (2026-07-23)

`vmap find` at the Roundtable spot (m11_10, w~7704,8560) shows TWO graces, both `src=Live`
(BonfireWarpParam runtime), both `area11 grid(10,0)`: **"Table of Lost Grace"** (the Roundtable, gets
discovered) + **"Gilded Court"** (undiscovered). This is NOT a mis-projection / copy (unlike m33/m31_90):
**Gilded Court is real ERR content** — the endgame version of the Roundtable Hall (where you access the
Trial of Recollection). Both graces physically live in the m11_10 hall → same grid → same overworld
projection → they overlap. USER DECISION 2026-07-23: **KEEP both** (they're real; spiderfy separates them
on hover). Don't "dedup" or suppress — if a future session sees two graces here, this is expected.
