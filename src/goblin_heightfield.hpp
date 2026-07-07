#pragma once
// Terrain heightfield via Havok ray-cast — the mod-agnostic relief backdrop for the virtual map
// (Track D2). Sample the LIVE 3D world with down-rays → ground height + surface normal, per grid cell,
// then hillshade it. Correct for ANY mod that reshapes the world (no baked map art).
//
// RE: docs/re/windows_terrain_raycast_heightfield_re_findings.md. Primitive:
//   FUN_140c70360(ctx, filter, start[3], segDir[3], outPoint[3], outNormal[3], &outDist) -> hit?
//   ctx = *( *(er+0x3d76060) + 0x98 )   (CS::PhysWorld instance → world holder); terrain filter 0x5e.
//
// THREAD SAFETY (RE §6): the cast reads the live hknpWorld and must run on the GAME thread, NOT the
// present/RPC thread. So the sampler ticks from the game-thread world-map step hook (hk_c32f0).
//
// FRAME (D2.1 finding): the cast is in Havok BLOCK-LOCAL coords (the player's chunk), NOT the world
// frame. The sampler converts world XZ → local by translating by the player's (world − local) offset,
// captured live at sample start, then stores the hit under its WORLD XZ for the vmap to draw.

#include <cstdint>
#include <vector>

namespace goblin::heightfield
{
struct RayHit
{
    float pt[3];   // hit point (pt[1] = ground height); in the cast's LOCAL frame
    float nrm[3];  // surface normal (frame-independent under a translation → safe for hillshade)
    float dist;    // hit fraction along the ray (0..1, float)
    bool hit;
};

// One sampled grid cell, stored in WORLD XZ (same frame as vmap markers/tiles).
struct Cell
{
    float wx, wz;      // world XZ (cell centre)
    float groundY;     // ground height at the cell (LOCAL-frame Y ≈ world height; Y is not tiled)
    float nx, ny, nz;  // surface normal
    bool hit;
    bool sea = false;  // M1 sea-tag: hit seabed below the sea-level heuristic → render water-blue, not land.
};

// Cast a downward ray from (x, y_high, z) spanning `depth` world units down, in the cast's LOCAL frame.
// filter 0x5e = walkable ground. SEH-guarded. MUST be called on the GAME thread.
bool cast_down(float x, float y_high, float z, float depth, uint32_t filter, RayHit &out);

// Queue a one-shot validation probe (D2.1): next game-thread tick casts at the player + logs. RPC `hf_probe`.
void request_probe();

// Queue a grid SAMPLE around the player (D2.2): extent = world-units square side, res = cells per side.
// The next game-thread ticks capture the player frame, then cast cells rate-limited. RPC `hf_sample`.
void request_sample(float extent, int res);

// Thread-safe snapshot of the last completed heightfield (for the vmap renderer, D2.3). Returns cell
// count; fills `out`. Empty until a sample completes.
size_t snapshot(std::vector<Cell> &out);

// World-units per cell (quad size) of the last completed field. 0 until a sample completes.
float snapshot_step();

// True while a grid sample is actively casting (for a "sampling…" UI hint).
bool sampling();

// Run pending heightfield work on the GAME thread. Call from the world-map step hook (hk_c32f0).
void tick_game_thread();

// EXPERIMENT (D2.2): the map-open path (hk_c32f0) can't cast — ER unloads world collision while the
// map is open. Test casting from the PRESENT thread during GAMEPLAY (map closed, world active) instead.
// Findings warn of a present-thread deadlock, but that was a streaming WRITE; a read-only cast may be
// safe. request_present_probe() queues a one-shot; tick_present() (called from hk_present) fires it only
// when the world is playable and the map is closed. RPC `hf_probe_present`.
void request_present_probe();
// Ground-shape probe (a): cast at the player + scan the query ctx for an hknp shape vtable → logs whether
// the ground is a heightfield GRID (readable direct) or a compressed mesh (raycast-only). See [HFSHAPE] log.
void request_shape_probe();
void tick_present();

// Synchronous ground-existence check at an arbitrary tile-local (x, z) column — the teleport
// guard's LIVE validity test (replaces trusting a fixed distance cap). Casts the wide relief
// window (kCastAbove above y_hint, kCastDepth down) with the walkable-ground filter: a hit means
// loaded collision exists in that column; a MISS means void / a deep-water kill column / not
// streamed — teleporting there free-falls, and a mid-fall autosave poisons the save (both
// observed 2026-07-07: the 11 km MapId-void jump AND a 40 m hop into Agheel Lake's floorless
// middle). Runs the cast directly when called on the present thread (the proven cast context);
// otherwise queues to tick_present and waits (~200 ms cap).
// Returns 1 = ground found (*ground_y filled), 0 = no collision in the column (do NOT teleport),
// -1 = unavailable (loading / native map open / cast unresolved / present thread stalled) —
// callers fall back to a conservative distance cap.
int ground_check_sync(float x, float y_hint, float z, float *ground_y);
} // namespace goblin::heightfield
