# RE brief — far-terrain elevation source (the "fake 3D" distant terrain) for full-map relief

**Goal:** find the height data the engine uses to render **distant terrain** — the terrain you SEE far away
in-game but which has **no Havok collision** (visual LOD / imposter only). The raycast heightfield
(`windows_terrain_raycast_heightfield_re_findings.md`) is **loaded-region-only**: it can only hit collision
streamed in a small radius around the player, so it can never cover the visible far map. To draw a relief
backdrop for the WHOLE map (mod-agnostically), MFG needs the far-LOD **elevation** source instead of (or
alongside) the collision raycast. Objects/collision stay out of scope — this is purely the ground height
field of the distant landscape.

Static Ghidra on `D:\ghidra_proj2\ER`, App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`, read-only; the
DLL is in-process (Linux/Proton) and can read resident GPU textures or decompress disk assets in-process.

## Why the raycast can't do this (context)
- Cast = `FUN_140c70360` on the single `CS::PhysWorld` hknpWorld (`ctx = *(*(er+0x3d76060)+0x98)`), filter
  `0x5e` = walkable ground. It queries **physics** → only hits collision bodies loaded around the player.
- The distant terrain is rendered from a **separate low-detail source** (a heightmap texture and/or a coarse
  LOD mesh) with **no collision body** → invisible to the cast. Confirmed empirically: cells beyond the
  player's loaded block return pure MISS (no wrong-hit), i.e. no collision there, not a frame error.

## What we need (priority order)
1. **The far-terrain elevation source.** How does ER store/render distant ground height? Identify which it is
   (one may not exclude the other) and WHERE it lives:
   - a low-res **global/regional heightmap TEXTURE** sampled by the terrain shader (most likely) — format
     (R16_UNORM? R8? float?), dimensions, how many (one per map dimension M00/M01/M10/M11? per big-tile?);
   - a coarse **LOD terrain MESH** (vertex Y) streamed for the far field;
   - a **param / data table** carrying per-cell elevation.
   For each: is it a **resident GPU texture** we can read draw-free (like the icon-harvest resident-texture
   path, `goblin_icon_harvest.cpp`), or a **disk asset** we decompress no-bake (Oodle/dvdbnd, like the map
   tiles in `maptile.cpp`)? Prefer a source readable WITHOUT an engine call.
2. **World→value decode.** The mapping from world `(X,Z)` (marker frame `worldX=mapU+7040, worldZ=-mapV+16512`)
   to the texel/UV or mesh vertex, and how the stored value decodes to a world **Y** (scale + bias, byte
   format, endianness). Enough to sample "ground height at world (x,z)" for any cell on the map.
3. **Coverage + resolution.** What extent does the source cover (whole Lands Between? per big-tile? the DLC
   Land of Shadow separately?) and at what cell size — so MFG knows the achievable relief resolution and any
   gaps.

## Optional (say if cheap)
4. **Surface normal / slope** — derivable from the heightmap (finite differences) if no normal channel; note
   if a normal map ships alongside (better hillshade).
5. **Sea/water level** — reuse the raycast prompt's #3: a global water-plane Y or per-region `GXSR
   WaterInteractionManager`/`WaterHeightMap` to classify far cells as sea vs land.
6. **Material / biome id** channel for a biome tint, if one ships with the far-terrain data.

## Leads / anchors
```
raycast (collision, loaded-only)  FUN_140c70360   ctx=*(*(er+0x3d76060)+0x98)   filter 0x5e   (the thing this REPLACES for the far field)
map-tile system (COLOR, already RE'd)  71_MapTile BHF4, dims M00/M01/M10/M11, LOD L0..L4  (src/worldmap/maptile.cpp)  <- check for a SIBLING height/normal archive or channel
water                              GXSR WaterInteractionManager / WaterHeightMap  (sea classification)
resident-texture read pattern      goblin_icon_harvest.cpp  (draw-free resident GPU texture → sub-rect → ID3D12Resource)
disk no-bake pattern               maptile.cpp (BHF4 parse + Oodle extract)  /  the dvdbnd/Oodle path
```

## The two runtime shapes to weigh (say which is feasible)
- **A — resident GPU texture read.** If the far-terrain height is a resident texture, read it draw-free like
  the icon harvest does (find the ID3D12Resource + format + the world→UV map) → no engine call, off-thread
  safe, mod-agnostic (reads the ACTIVE install). Best if it exists resident.
- **B — disk no-bake.** If it's a disk asset (heightmap in the map archives), decompress in-process
  (Oodle/dvdbnd) like the map tiles → also mod-agnostic, covers the whole map regardless of what's streamed.

## Constraints
- **Mod-agnostic / runtime-disk over baked** (prime directive): read the ACTIVE install's real far-terrain
  data (resident texture or disk), NOT a baked snapshot — so it's correct for any mod that reshapes the world.
- **Whole-map coverage** is the whole point (beyond the collision radius). Note any per-tile stitching needed.
- **Read-only, no world mutation.** Ideally no engine call (texture/asset read only).

## Deliverable
A far-terrain elevation source (resident texture RVA/slot **or** disk asset path + format) + the world→Y
decode + coverage/resolution + a water level — enough for the DLL to fill the map BEYOND the loaded-collision
radius with relief, mod-agnostically. Pairs with the collision raycast (near field = precise cast, far field =
this) for a seamless full-map hillshade. Findings → `docs/re/far_terrain_heightmap_re_findings.md`.
Platform: static Ghidra to find the shader/texture binding + the disk archive; runtime (Linux in-DLL) to read
the resident texture / decompress the asset. Objects/collision are OUT of scope.
