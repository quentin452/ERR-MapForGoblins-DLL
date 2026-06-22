# RE brief — read WorldMapLegacyConvParam LIVE + replicate the fold (kill baked LEGACY_CONV)

**Goal:** read `WorldMapLegacyConvParam` from the live regulation (SoloParamRepository) and
replicate the engine's legacy-dungeon → overworld FOLD in C++, so we can project any
(area,grid,pos) to the unified overworld/UG frame **without the baked `LEGACY_CONV` snapshot
and without the live WorldMapViewModel** — i.e. it works MAP-CLOSED (the param is always
resident), which the VM-based live projection cannot do.

App 2.6.2.0 / ERR 2.2.9.6, DLL in-process. Resolve as `[er_base+RVA]+offsets` / AOBs. Read-only.

## Why (the wall we hit)
The live world→map projection (`windows_world_to_mapspace_projection_re_findings.md`) calls the
engine on the live `CS::WorldMapViewModel` — but that VM only exists while the world map is
OPEN. The **minimap is a gameplay HUD (map CLOSED)** and needs a UNIFIED frame to place the
player + same-page markers across legacy/underground areas. Today that unified frame comes from
our baked `LEGACY_CONV` fold (`project_dungeon_row_to_overworld`). To DELETE the baked snapshot
(246 lines, per-mod drift) we must reproduce the fold from data that's resident map-closed = the
regulation param itself. Same read-live-not-bake philosophy as the projection + params elsewhere
(we already read SoloParam/MsgRepository live).

## What's ALREADY known (from the projection findings §2)
The engine fold is `FUN_1408775e0` (RVA `0x8775e0`), called per converter when its
`+0x28` legacyConvNode is set:
- RB-tree lookup keyed by the packed id (`*(node+0x10)` tree; compare `row+0x1c` vs the key).
- On hit: write the **dst** packed id (`*param = row[4]`, i.e. `row+0x4`) and **translate** the
  world pos by the conv base offset (`row+0x24` / `row+0x2c`), then `FUN_140877840` re-normalizes
  id vs grid. Predicate `FUN_140660fe0` short-circuits to identity for ids that don't fold.
- Our mod's `project_dungeon_row_to_overworld` already APPROXIMATES this (it substitutes the dst
  grid directly + drops the base offset → the area-16 "wrong region" bug). The exact base-offset
  translation is what we want from the real param.

## What we NEED (decompile + runtime-confirm)
1. **The `WorldMapLegacyConvParam` paramdef** (PARAM row layout): the field names + byte offsets
   the fold reads — the SOURCE key (areaNo/gridX/gridZ pack at `row+0x1c`?), the DST id
   (`row+0x4`: dst area/grid), and the **base-offset** translation (`row+0x24`/`row+0x2c` — are
   these f32 world deltas? grid deltas?). Map the raw offsets above to the paramdef fields so we
   can read them by name.
2. **The param id / how to fetch it live** from SoloParamRepository (the same path we use for
   other params): the param name string ("WorldMapLegacyConvParam"?), its index/id, and the
   row-iteration (id list + row data ptr + stride). Confirm it's resident MAP-CLOSED.
3. **The exact fold algorithm to replicate** (mirror `FUN_1408775e0` + `FUN_140877840`):
   given (srcArea,srcGridX,srcGridZ,posX,posZ): find the matching row (the RB-tree key match),
   apply `dstId = row.dst`, `pos += row.baseOffset`, renormalize grid — yielding the dst
   (area,grid,pos) in the overworld/UG frame. Include the multi-hop case (does the game iterate
   until no further fold, e.g. m16 Raya Lucaria → … → 60?) or is it single-hop?
4. **Identity predicate** (`FUN_140660fe0`): which ids skip the fold (overworld 60/61 pass
   through) — so we don't mis-fold a non-legacy point.

## Deliverable
- The `WorldMapLegacyConvParam` paramdef offsets (key / dst / base-offset) + the live fetch
  (param id + SoloParamRepository access), runtime-confirmed by dumping a few rows.
- A C++ `fold(area,gx,gz,px,pz) -> (area,gx,gz,px,pz)` spec mirroring `FUN_1408775e0` exactly,
  with a worked example: a known legacy dungeon (e.g. m16 Raya Lucaria interior, or m12 Siofra)
  → its folded overworld coords, matching what the live VM projection produced for the same point.
- Whether the fold is single- or multi-hop.

## Plan once answered
Read `WorldMapLegacyConvParam` live (lazy-build a lookup once, refresh on regulation change),
replace `project_dungeon_row_to_overworld` + the baked `LEGACY_CONV` table with the live fold +
the known overworld affine (origin gridbase·256, bias 128, scale 1, Z-flip from the converter
dump). Then `marker_world_pos` / the minimap project map-closed from live param data → DELETE the
baked `LEGACY_CONV` (both generated/ + generated_vanilla/) and `dlc_ug_eyeball`. The VM-based
`worldmap_probe::project` stays as the map-OPEN fast path (already validated); this is the
map-CLOSED equivalent built from the same regulation data → no drift, no FFDEC-style bake.

## Why
Removes the last baked, drift-prone projection asset and unblocks the minimap from the live
projection (it can't reach the VM map-closed). Mirrors the projection + live-param wins: the
engine's own regulation data, read live, replaces our snapshot.
