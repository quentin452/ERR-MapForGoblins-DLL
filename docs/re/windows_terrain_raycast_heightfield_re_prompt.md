# RE brief — terrain raycast → heightfield (procedural relief map, mod-agnostic)

**Goal:** sample the LIVE 3D world into a height grid by casting downward rays over an XZ grid, so MFG can
render a top-down **relief/hillshade** map derived from the ACTUAL loaded geometry — automatically correct
for ANY mod that reshapes the 3D world (the real fix for the "Convergence trap": baked 2D map art goes
stale, a sampled heightfield never does). This is the "sampled matrix from the 3D world" path. Static Ghidra
on `D:\ghidra_proj2\ER`, App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`, read-only; the DLL is
in-process (Linux/Proton) and can call the resolved fn.

## What we need (in priority order)
1. **The raycast function.** A downward ray `(origin=(x, yHigh, z), dir=(0,-1,0), maxDist)` → nearest hit
   `{ point.y (ground height), normal, material/objType }`. Give: the fn RVA + signature/ABI, the physics/
   world-query manager `this` (how to resolve it live), and the ARG layout (origin/dir/length/**filter mask**
   + the out-struct). **Lead:** `FUN_140c74c70` is called by the AEG proximity streamer
   (`windows_geom_spawn_pivot2_re_findings.md`) on `DAT_143d76060` with query types `0x5d`/`0x67` — decode
   what those query-type bytes select and whether one is a world/terrain line cast. If `FUN_140c74c70` isn't
   the general cast, find the CS physics ray/line-test entry (Havok `hkpWorld` wrapper / `CS::PhysWorld`).
2. **The TERRAIN filter.** The ray must hit **map/terrain collision ONLY**, NOT dynamic objects, characters,
   or AEG props (those would give false heights — and we already have objects as markers/MSB). Which
   filter-mask / collision-group value restricts the cast to the static world/landscape? (The value to pass so
   trees/buildings/enemies are ignored.)
3. **Water level.** To separate SEA from LAND (silhouette + coastline): is there a global water-plane Y, a
   per-region water height, or a water-surface the ray can hit (material id)? Any of these lets us classify a
   cell as sea (below/at water) vs land.

## Optional (nice-to-have, say if cheap)
4. **Surface normal** at the hit → hillshade shading (much better relief than height alone). Usually in the
   same out-struct as the hit point.
5. **Material / biome id** at the hit → biome tint (grass/rock/sand/snow). If the cast returns a material or
   the hit collidable carries one.

## Constraints to confirm (runtime, for the Linux side)
6. **Thread-safety.** Which thread may call the cast safely? The streamer calls it on the game update thread;
   confirm whether an off-thread call (our present/RPC thread) is safe or must be marshalled (the geom
   `FUN_1406a5080` deadlocked from the present thread — `windows_geom_spawn_pivot2_re_findings.md` — so this
   matters). If it must run on the game thread, note the per-frame hook point we already have.
7. **Loaded-region only.** The cast only returns hits where the world is STREAMED in. Confirm there's no
   "load all collision" shortcut; MFG will sample the region around the player (and can warp to extend). Note
   the world/streaming extent if readable.
8. **World→grid frame.** Ground height sampled at world (x,z) — same frame as markers (worldX/worldZ, the
   solved projection `worldX=mapU+7040`, `worldZ=-mapV+16512`). Confirm the cast uses that same world frame.

## Anchors / leads
```
raycast lead   FUN_140c74c70   (AEG streamer proximity cast; query types 0x5d/0x67)
query manager  DAT_143d76060   (the object FUN_140c74c70 is called on)
player pos     WorldChrMan [er+0x3D65F88] +0x1E508 LocalPlayer +0x6C0 pos  (a ray origin reference)
geom streamer  FUN_140699670 / FUN_14069a9b0  (callers that do the cast + read the hit)
```

## Deliverable
The cast fn (RVA + ABI + manager + arg/out layout + the terrain filter mask) and a water-level source —
enough for the DLL to, per grid cell, cast a down-ray, get ground Y (+ optional normal/material), classify
sea/land, and build a hillshade. Findings →
`docs/re/windows_terrain_raycast_heightfield_re_findings.md`. NB objects are OUT of scope (excluded by the
filter); they come from the existing marker/MSB data.
