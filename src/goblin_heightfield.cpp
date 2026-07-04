#include "goblin_heightfield.hpp"

#include "goblin_inject.hpp"   // get_player_world_pos (ray origin)

#include <spdlog/spdlog.h>

#include <atomic>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace goblin::heightfield
{
namespace
{
// FUN_140c70360 — line cast: (ctx, filter, start[3], segDir[3], outPoint[3], outNormal[3], &outDist)->hit.
using CastRayFn = int (*)(void *, uint32_t, float *, float *, float *, float *, uint32_t *);

// Resolved by fixed RVA (ERR 2.2.9.6 dev box). The on-disk exe is Steam/VMProtect-wrapped so an AOB can't
// be derived from disk — RVA now, AOB-harden later (heightfield_relief_plan.md). Lazy resolve (NOT at
// boot — the boot AOB-scan crash gotcha; resolve on first use, always long after the module is mapped).
constexpr uintptr_t CASTRAY_RVA   = 0xc70360;
constexpr uintptr_t PHYSWORLD_RVA = 0x3d76060;   // CS::PhysWorld FD4Singleton static slot (holds instance ptr)
constexpr uint32_t  FILTER_GROUND = 0x5e;        // walkable-ground / map query type

CastRayFn g_cast = nullptr;
void **g_physworld_slot = nullptr;   // *(er+0x3d76060) = PhysWorld instance
std::atomic<bool> g_ready{false};
std::atomic<bool> g_probe_pending{false};
bool g_logged_ctx = false;

void resolve()
{
    if (g_ready.load()) return;
    uintptr_t er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (er)
    {
        g_cast = reinterpret_cast<CastRayFn>(er + CASTRAY_RVA);
        g_physworld_slot = reinterpret_cast<void **>(er + PHYSWORLD_RVA);
    }
    g_ready.store(true);
    spdlog::info("[HEIGHTFIELD] resolve: er={:#x} cast={} physworld_slot={} ({})", er, (void *)g_cast,
                 (void *)g_physworld_slot, (g_cast && g_physworld_slot) ? "OK" : "MISS");
}

// The raw game call, isolated so the __try wraps a lone opaque CALL (clang-cl keeps that; a __try
// around inline loads gets elided — clang-cl-seh-noinline). ctx resolved OUTSIDE, passed in.
__declspec(noinline) static int call_cast(void *ctx, uint32_t filter, float *start, float *dir,
                                          float *pt, float *nrm, uint32_t *dist)
{
    return g_cast(ctx, filter, start, dir, pt, nrm, dist);
}
} // namespace

bool cast_down(float x, float y_high, float z, float depth, uint32_t filter, RayHit &out)
{
    out.hit = false;
    if (!g_ready.load()) resolve();
    if (!g_cast || !g_physworld_slot) return false;

    // Resolve ctx = *( PhysWorld_instance + 0x98 ), all guarded (the singleton may be null pre-world).
    void *ctx = nullptr;
    SIZE_T got = 0;
    void *instance = nullptr;
    if (!ReadProcessMemory(GetCurrentProcess(), g_physworld_slot, &instance, sizeof(instance), &got) ||
        got != sizeof(instance) || reinterpret_cast<uintptr_t>(instance) < 0x10000)
        return false;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<uint8_t *>(instance) + 0x98, &ctx,
                           sizeof(ctx), &got) ||
        got != sizeof(ctx) || reinterpret_cast<uintptr_t>(ctx) < 0x10000)
        return false;
    if (!g_logged_ctx)
    {
        g_logged_ctx = true;
        spdlog::info("[HEIGHTFIELD] instance={} ctx={}", instance, ctx);
    }

    float start[3] = {x, y_high, z};
    float dir[3]   = {0.f, -depth, 0.f};   // segment vector; END = start + dir (straight down)
    float pt[3]    = {0.f, 0.f, 0.f};
    float nrm[3]   = {0.f, 0.f, 0.f};
    uint32_t dist  = 0;

    int hit = 0;
    __try
    {
        hit = call_cast(ctx, filter, start, dir, pt, nrm, &dist);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    if (!hit) return false;
    out.pt[0] = pt[0]; out.pt[1] = pt[1]; out.pt[2] = pt[2];
    out.nrm[0] = nrm[0]; out.nrm[1] = nrm[1]; out.nrm[2] = nrm[2];
    // outDist is a packed u32 (fraction·rayLength per RE); expose raw for now.
    out.dist = static_cast<float>(dist);
    out.hit = true;
    return true;
}

void request_probe() { g_probe_pending.store(true); }

void tick_game_thread()
{
    if (!g_probe_pending.exchange(false)) return;

    // Validation probe (D2.1): cast a down-ray at the player and compare the ground Y to the player's
    // foot Y. start well above, span well below, so we bracket any terrain the player stands on.
    float px = 0.f, py = 0.f, pz = 0.f;
    if (!goblin::get_player_world_pos(px, py, pz))
    {
        spdlog::warn("[HEIGHTFIELD] probe: no player pos (not in-world)");
        return;
    }
    RayHit h{};
    bool ok = cast_down(px, py + 2000.f, pz, 4000.f, FILTER_GROUND, h);
    if (ok)
        spdlog::info("[HEIGHTFIELD] probe @player({:.1f},{:.1f},{:.1f}): GROUND y={:.2f} (Δfoot={:.2f}) "
                     "nrm=({:.3f},{:.3f},{:.3f}) dist={:.0f}",
                     px, py, pz, h.pt[1], h.pt[1] - py, h.nrm[0], h.nrm[1], h.nrm[2], h.dist);
    else
        spdlog::warn("[HEIGHTFIELD] probe @player({:.1f},{:.1f},{:.1f}): MISS (no terrain hit / unresolved)",
                     px, py, pz);
}
} // namespace goblin::heightfield
