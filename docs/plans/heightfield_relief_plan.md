# Plan — raycast heightfield relief (Track D2 of the ImGui-only map)

Status: **DESIGN 2026-07-04, starting.** The mod-agnostic terrain backdrop for the vmap: sample the
LIVE 3D world with down-rays → ground height + normal → hillshade + sea/land. Correct for ANY mod that
reshapes the world (kills the Convergence trap). RE is DONE (static):
`docs/re/windows_terrain_raycast_heightfield_re_findings.md`.

## The primitive (from RE)
```
int FUN_140c70360(void* ctx, u32 filter, float start[3], float segDir[3],
                  float outPoint[3], float outNormal[3], u32* outDist);  // -> 1 hit / 0 miss
ctx    = *(DAT_143d76060 + 0x98)        // CS::PhysWorld singleton er+0x3d76060; ctx+8 = hknpWorld
filter = 0x5e                            // walkable ground / map (snap-to-ground query type)
```
Per cell (x,z): `start={x, Yhigh, z}`, `segDir={0, -(Yhigh-Ylow), 0}` → `outPoint[1]` = ground Y,
`outNormal` = slope. Same world frame as markers (`worldX=mapU+7040, worldZ=-mapV+16512`) — drops
straight onto the vmap projection, no transform.

## Hard constraints (RE §6)
- **Thread:** call on the GAME UPDATE THREAD, NOT present/RPC (hknp queries race the physics step; a
  present-thread deadlock is documented for the AEG streamer / geom_spawn). ← the key infra question.
- **Loaded-region only:** rays hit only STREAMED-IN collision. Sample around the player; misses = nodata;
  extend coverage by warping (warp is RE'd + now correct).
- **In-process only:** a pure RPM heightfield is impossible — the cast must execute in-process.

## Resolution (AOB vs RVA)
The on-disk `eldenring.exe` is Steam/VMProtect-wrapped → **cannot derive AOBs from disk**. Two paths:
- **Now (validate):** resolve `FUN_140c70360` + `DAT_143d76060` by **fixed RVA** (`er_base + 0xc70360`
  / `+0x3d76060`). Build-specific (ERR 2.2.9.6 = the dev box), like the WorldChrMan RVA fallback. Gate
  the whole feature behind a config flag; flag it needs-AOB-hardening.
- **Harden later:** a Windows Ghidra pass (or a runtime `mem_dump` at the live address + hand-craft) to
  produce byte signatures for `re_signatures.hpp`, so it survives a patch / works mod-agnostic.

## Slices
### D2.1 — validate the primitive — ✅ DONE + LIVE-VERIFIED 2026-07-04 (`abddc05`+float-fix)
Cold-boot RPC test (`hf_probe` → open map → `[HEIGHTFIELD]` log) confirmed:
- Resolution OK (cast fn + PhysWorld singleton; ctx = *(*(er+0x3d76060)+0x98) — both plausible ptrs).
- **Cast HITS with a valid UP normal** `(0.075, 0.994, -0.085)` = walkable ground. Ran on the GAME
  thread via `hk_c32f0` — no crash/deadlock. `outDist` is FLOAT bits (hit fraction 0..1; 0.527 matched
  2077/4000 = the y=2088→10.67 drop).
- **KEY FINDING — the cast frame is Havok BLOCK-LOCAL, NOT the world frame.** The probe used the
  player's LocalPlayer+0x6C0 block-local coords (-80.4, 52.0) and hit ground directly under them. So the
  RE-doc's "same world frame as markers" is wrong for this path: D2.2 MUST convert world XZ → the
  current physics-chunk-local frame before casting (inverse of marker_world_pos: subtract the chunk/tile
  world origin), then convert the hit XZ back to world for drawing. This ties the sampler to the
  loaded chunks around the player (consistent with "loaded-region only").
- Δfoot=-77 in the probe = the save spot sits ~77u above base terrain (not a bug).

### D2.1 (original) — validate the primitive (de-risk) — FIRST
- New `src/goblin_heightfield.{cpp,hpp}`. Resolve fn + singleton by RVA (lazy, NOT at boot — the
  boot-scan gotcha). SEH-guarded noinline call wrapper (clang-cl pattern).
- One-shot test: cast a down-ray at the player XZ (`start={px, py+2000, pz}`, `segDir={0,-4000,0}`,
  filter 0x5e); log `outPoint.y`, `outNormal`, `outDist`. **Success = outPoint.y ≈ player foot Y.**
- Call point: on the game-update thread if one exists; else try a one-shot on the present thread
  (SEH-guarded, read-only — a single query may be safe enough to validate the ABI; escalate to a
  game-thread hook if it deadlocks/crashes). RPC verb `hf_probe` to trigger it.

### D2.2 — grid sampler
- Sample an N×N grid (config extent + resolution) around the player; a FEW cells/frame (rate-limited)
  on the safe thread → store `{groundY, normal, hit}` matrix in world XZ. Nodata for misses.

### D2.3 — render hillshade on the vmap
- `shade = dot(normalize(normal), lightDir)`; sea = `groundY < seaLevelConst` (config, per page) → blue;
  land = shaded grey/biome tint. Draw as a texture (or per-cell quads) UNDER markers/tiles on the vmap
  canvas (same AddImage slot as `s_tiles`). Toggle in the toolbar.

### D2.4 — coverage extension + persistence
- Accumulate the matrix as the player moves/warps; persist so a revisit doesn't re-sample. Log what's
  nodata (no silent gaps).

## Water / biome (follow-ups, RE §4/§5)
- Sea: heuristic `y < seaLevel` first. Escalate to `GXSR WaterHeightMap` sampling only if coastlines
  look wrong under a mod (separate RE).
- Biome tint: the cast's hit body carries a material id; mapping not decoded (follow-up).

## Anchors (this build, er-relative — RE §7)
```
CS::PhysWorld singleton   er+0x3d76060   ctx = *(singleton + 0x98)
ray cast (full)           er+0xc70360    (ctx, filter, start, segDir, &pt, &nrm, &dist) -> hit
terrain filter            0x5e
player pos (ray origin)   WorldChrMan[er+0x3d65f88]+0x1e508 LocalPlayer +0x6C0
```
