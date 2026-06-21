# RE brief — the game's runtime world→map-space projection (kill the baked LegacyConv)

**Goal:** find the game function that places a world position onto the world-map (the projection
the native map uses for EVERY icon), so we can call it LIVE and DELETE all our baked/hand-tuned
projection data: `LEGACY_CONV` (WorldMapLegacyConvParam snapshot), the `world_to_mapspace` affine
(`mapU=worldX-7040; mapV=-worldZ+16512`), the DLC-underground "eyeball" approximation, and the
area→page/group classification. One game-authoritative projection replaces all of it and Just Works
for every area — including ones with NO LegacyConv entry (e.g. m19 Chapel of Anticipation, which the
param genuinely doesn't map — confirmed 0 area-19 rows in WorldMapLegacyConvParam base+DLC).

App 2.6.2.0 / ERR 2.2.9.6, our DLL is in-process. Resolve everything as `[er_base+RVA]+offsets` /
AOBs, runtime-verified — same style as the SOLVED player-pos
(`docs/re/windows_player_pos_RESOLVED_re_findings.md`: WorldChrMan=[er+0x3D65F88], LocalPlayer=
[+0x1E508], pos +0x6C0).

## What we do today (baked, to be replaced)
For a marker (areaNo, gridXNo, gridZNo, posX, posZ) we: (1) project legacy/underground areas onto the
overworld via baked `LEGACY_CONV` (`project_dungeon_row_to_overworld`); (2) `world = gridX*256+pos`;
(3) `mapU = world.x - 7040`, `mapV = -world.z + 16512` (agent-confirmed affine); (4) project mapU/mapV
with the live pan/zoom (WorldMapArea +0x378/+0x380) to the backbuffer. This is brittle: missing conv
rows (Chapel) project to garbage, the DLC-UG page is a hand-tuned eyeball, page/group is guessed.

## What we NEED (decompile + runtime-confirm)
The native map builds `CSWorldMapPointIns` icons and positions each. Find the routine that maps a
point's world/param coords → the map-dialog UV/local position. Likely near the icon build/reconcile:
prior leads in this repo — `FUN_140a82a80` (build/discover), `FUN_140a832a0` (reconcile),
`FUN_140d82770` (suspected world→map-UV), the marker-affine site at `eldenring.exe+0xa8397e`
(`marker_affine_hook_re_findings.md`), `CSWorldMapPointMan=[er+0x3D6E9B0]`. Report:
1. **The projection function**: signature + how it's called per icon. Inputs (a WorldMapPointParam
   row? a raw world Vec? area+grid+pos?) → outputs (map-space UV / dialog-local x,y).
2. **The WorldMapLegacyConvParam consumption**: does this fn read the conv param live (so legacy
   dungeons fold onto the overworld inside it)? If yes, we can drop our baked LEGACY_CONV and let the
   fn do it. Where/how it reads it (param id, the lookup).
3. **The transform constants** it applies (compare to our `-7040 / +16512`), incl. the per-page /
   per-area branches (overworld, base underground m12, DLC overworld m61, DLC underground m40-43 —
   the eyeball we never solved).
4. **A callable entry**: can we call this fn from our DLL with our own (area,grid,pos) to get map-UV,
   or must we replicate it? If callable, the `this`/manager pointer + arg layout. If not, the exact
   math to replicate.
5. **Page/group**: how the fn (or the dialog) decides which map PAGE a point belongs to (overworld vs
   underground vs DLC) — so we can drop our `marker_group_from` heuristic.

## Deliverable
- The projection fn RVA/AOB + a worked example: feed a known marker (e.g. a Limgrave grace
  area 60 grid X,Z pos) → the map-UV it produces, so we validate our live call against it.
- Confirmation of whether m19/Chapel-type no-conv areas are simply not placed by the game (then we
  gate them) or placed via some fallback.

## Why
Replaces ~all of our projection guesswork with the engine's own math → every page correct (incl. the
DLC-UG eyeball), self-adapts to ERR/mods, and removes the LegacyConv/mapspace/eyeball baked data.
The player-pos already feeds this (its `+0x6C0` is the WorldMapPointParam posX/posZ frame).
