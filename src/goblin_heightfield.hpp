#pragma once
// Terrain heightfield via Havok ray-cast — the mod-agnostic relief backdrop for the virtual map
// (Track D2). Sample the LIVE 3D world with down-rays → ground height + surface normal, per grid cell,
// then hillshade it. Correct for ANY mod that reshapes the world (no baked map art).
//
// RE: docs/re/windows_terrain_raycast_heightfield_re_findings.md. The primitive:
//   FUN_140c70360(ctx, filter, start[3], segDir[3], outPoint[3], outNormal[3], &outDist) -> hit?
//   ctx = *( *(er+0x3d76060) + 0x98 )   (CS::PhysWorld instance → world holder); terrain filter 0x5e.
//
// THREAD SAFETY (RE §6): the cast reads the live hknpWorld and must run on the GAME thread, NOT the
// present/RPC thread (a scene query racing the physics step reads garbage / the geom_spawn present-
// thread deadlock). So the sampler ticks from the game-thread world-map step hook (hk_c32f0), which
// only runs while the map is open — exactly when the relief is wanted.

#include <cstdint>

namespace goblin::heightfield
{
struct RayHit
{
    float pt[3];   // world hit point (pt[1] = ground height)
    float nrm[3];  // surface normal (→ hillshade)
    float dist;    // hit distance along the ray
    bool hit;
};

// Cast a downward ray from (x, y_high, z) spanning `depth` world units down. filter = collision query
// type (0x5e = walkable ground). SEH-guarded. MUST be called on the GAME thread. Returns hit + fills out.
bool cast_down(float x, float y_high, float z, float depth, uint32_t filter, RayHit &out);

// Queue a one-shot validation probe (D2.1): the next game-thread tick casts a down-ray at the player and
// logs the result. Safe to call from any thread (just sets a flag). RPC `hf_probe`.
void request_probe();

// Run pending heightfield work on the GAME thread. Call from the world-map step hook (hk_c32f0).
// No-op unless a probe/sample is pending.
void tick_game_thread();
} // namespace goblin::heightfield
