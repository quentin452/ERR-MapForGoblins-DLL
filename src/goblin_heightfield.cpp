#include "goblin_heightfield.hpp"

#include "goblin_inject.hpp"   // get_player_world_pos (local ray origin) + get_player_map_pos (world)

#include <spdlog/spdlog.h>

#include <atomic>
#include <mutex>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace goblin::heightfield
{
namespace
{
// FUN_140c70360 — line cast: (ctx, filter, start[3], segDir[3], outPoint[3], outNormal[3], &outDist)->hit.
using CastRayFn = int (*)(void *, uint32_t, float *, float *, float *, float *, uint32_t *);

// Resolved by fixed RVA (ERR 2.2.9.6). On-disk exe is Steam/VMProtect-wrapped → no disk AOB; RVA now,
// AOB-harden later. Lazy resolve (NOT at boot — the boot AOB-scan crash gotcha).
constexpr uintptr_t CASTRAY_RVA   = 0xc70360;
constexpr uintptr_t PHYSWORLD_RVA = 0x3d76060;   // CS::PhysWorld FD4Singleton static slot (instance ptr)
constexpr uint32_t  FILTER_GROUND = 0x5e;        // walkable-ground / map query type

CastRayFn g_cast = nullptr;
void **g_physworld_slot = nullptr;
std::atomic<bool> g_ready{false};
bool g_logged_ctx = false;

// ── one-shot validation probe ──
std::atomic<bool> g_probe_pending{false};
std::atomic<bool> g_present_probe_pending{false};

// ── grid sampler ──
std::atomic<bool> g_sample_pending{false};   // request → capture-frame-and-build on the next game tick
float g_req_extent = 4096.f;
int   g_req_res = 48;
bool g_sampling = false;                      // actively casting queued cells
std::vector<std::pair<float, float>> g_queue; // world XZ cells still to cast
size_t g_qi = 0;
std::vector<Cell> g_accum;                     // cells cast this run (world XZ)
// player frame captured at sample start: world→local translation is (local0 - world0).
float g_world0x = 0.f, g_world0z = 0.f, g_local0x = 0.f, g_local0y = 0.f, g_local0z = 0.f;
int g_hits = 0;
constexpr int kCellsPerTick = 48;             // rate-limit: cells cast per game frame

std::mutex g_snap_mtx;
std::vector<Cell> g_snapshot;                  // last completed field (renderer reads this)

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

// noinline so the __try wraps a lone opaque CALL (clang-cl-seh-noinline).
__declspec(noinline) static int call_cast(void *ctx, uint32_t filter, float *start, float *dir,
                                          float *pt, float *nrm, uint32_t *dist)
{
    return g_cast(ctx, filter, start, dir, pt, nrm, dist);
}

// Resolve ctx = *( PhysWorld_instance + 0x98 ), all guarded. Returns null pre-world.
void *resolve_ctx()
{
    SIZE_T got = 0;
    void *instance = nullptr;
    if (!ReadProcessMemory(GetCurrentProcess(), g_physworld_slot, &instance, sizeof(instance), &got) ||
        got != sizeof(instance) || reinterpret_cast<uintptr_t>(instance) < 0x10000)
        return nullptr;
    void *ctx = nullptr;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<uint8_t *>(instance) + 0x98, &ctx,
                           sizeof(ctx), &got) ||
        got != sizeof(ctx) || reinterpret_cast<uintptr_t>(ctx) < 0x10000)
        return nullptr;
    if (!g_logged_ctx) { g_logged_ctx = true; spdlog::info("[HEIGHTFIELD] instance={} ctx={}", instance, ctx); }
    return ctx;
}
} // namespace

// Cast with an EXPLICIT ctx (diagnostic — normal callers use cast_down which resolves ctx).
static bool cast_with_ctx(void *ctx, float x, float y_high, float z, float depth, uint32_t filter, RayHit &out)
{
    out.hit = false;
    if (!g_cast || !ctx) return false;
    float start[3] = {x, y_high, z};
    float dir[3]   = {0.f, -depth, 0.f};
    float pt[3]    = {0.f, 0.f, 0.f};
    float nrm[3]   = {0.f, 0.f, 0.f};
    uint32_t dist  = 0;
    int hit = 0;
    __try { hit = call_cast(ctx, filter, start, dir, pt, nrm, &dist); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!hit) return false;
    out.pt[0] = pt[0]; out.pt[1] = pt[1]; out.pt[2] = pt[2];
    out.nrm[0] = nrm[0]; out.nrm[1] = nrm[1]; out.nrm[2] = nrm[2];
    out.dist = *reinterpret_cast<float *>(&dist);
    out.hit = true;
    return true;
}

// Read a qword via guarded RPM; returns 0 on failure.
static void *rd(void *addr)
{
    void *v = nullptr; SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), addr, &v, sizeof(v), &got) || got != sizeof(v)) return nullptr;
    return v;
}

// DIAGNOSTIC: try every plausible ctx interpretation at the player and log which one hits.
void diag_ctx(float px, float py, float pz)
{
    if (!g_ready.load()) resolve();
    if (!g_cast || !g_physworld_slot) { spdlog::warn("[HFDIAG] unresolved"); return; }
    void *inst = rd(g_physworld_slot);
    spdlog::info("[HFDIAG] slot={} *slot(inst)={} @player({:.1f},{:.1f},{:.1f})",
                 (void *)g_physworld_slot, inst, px, py, pz);
    struct Cand { const char *name; void *ctx; };
    Cand cands[] = {
        {"A *(inst+0x98)", inst ? rd(reinterpret_cast<uint8_t *>(inst) + 0x98) : nullptr},
        {"B *(slot+0x98)", rd(reinterpret_cast<uint8_t *>(g_physworld_slot) + 0x98)},
        {"C inst", inst},
        {"D *(inst)", inst ? rd(inst) : nullptr},
        {"E *(inst+0x8)", inst ? rd(reinterpret_cast<uint8_t *>(inst) + 0x8) : nullptr},
    };
    for (auto &c : cands)
    {
        if (!c.ctx || reinterpret_cast<uintptr_t>(c.ctx) < 0x10000) { spdlog::info("[HFDIAG] {} ctx={} (skip)", c.name, c.ctx); continue; }
        RayHit h{};
        bool ok = cast_with_ctx(c.ctx, px, py + 2000.f, pz, 4000.f, FILTER_GROUND, h);
        spdlog::info("[HFDIAG] {} ctx={} -> hit={} y={:.2f} nrm=({:.2f},{:.2f},{:.2f})",
                     c.name, c.ctx, ok, h.pt[1], h.nrm[0], h.nrm[1], h.nrm[2]);
    }
}

bool cast_down(float x, float y_high, float z, float depth, uint32_t filter, RayHit &out)
{
    out.hit = false;
    if (!g_ready.load()) resolve();
    if (!g_cast || !g_physworld_slot) return false;
    void *ctx = resolve_ctx();
    if (!ctx) return false;

    float start[3] = {x, y_high, z};
    float dir[3]   = {0.f, -depth, 0.f};
    float pt[3]    = {0.f, 0.f, 0.f};
    float nrm[3]   = {0.f, 0.f, 0.f};
    uint32_t dist  = 0;
    int hit = 0;
    __try { hit = call_cast(ctx, filter, start, dir, pt, nrm, &dist); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!hit) return false;
    out.pt[0] = pt[0]; out.pt[1] = pt[1]; out.pt[2] = pt[2];
    out.nrm[0] = nrm[0]; out.nrm[1] = nrm[1]; out.nrm[2] = nrm[2];
    out.dist = *reinterpret_cast<float *>(&dist);  // FLOAT bits (hit fraction 0..1)
    out.hit = true;
    return true;
}

void request_probe() { g_probe_pending.store(true); }

void request_sample(float extent, int res)
{
    if (extent < 64.f) extent = 64.f;
    if (res < 2) res = 2;
    if (res > 256) res = 256;   // 256² = 65536 cells cap
    g_req_extent = extent;
    g_req_res = res;
    g_sample_pending.store(true);
}

size_t snapshot(std::vector<Cell> &out)
{
    std::lock_guard<std::mutex> lk(g_snap_mtx);
    out = g_snapshot;
    return out.size();
}

namespace
{
// Capture the player frame + build the world-XZ cell queue. Runs on the game thread.
bool start_sample()
{
    int area = 0, grp = 0;
    float wx = 0.f, wz = 0.f;      // player WORLD XZ (vmap frame)
    float lx = 0.f, ly = 0.f, lz = 0.f;  // player LOCAL XYZ (cast frame)
    if (!goblin::get_player_map_pos(area, wx, wz, nullptr, nullptr, &grp) ||
        !goblin::get_player_world_pos(lx, ly, lz))
    {
        spdlog::warn("[HEIGHTFIELD] sample: no player pos (not in-world)");
        return false;
    }
    g_world0x = wx; g_world0z = wz;
    g_local0x = lx; g_local0y = ly; g_local0z = lz;

    const int res = g_req_res;
    const float step = g_req_extent / static_cast<float>(res);
    const float half = g_req_extent * 0.5f;
    g_queue.clear();
    g_queue.reserve(static_cast<size_t>(res) * res);
    for (int gz = 0; gz < res; ++gz)
        for (int gx = 0; gx < res; ++gx)
            g_queue.emplace_back(wx - half + (gx + 0.5f) * step, wz - half + (gz + 0.5f) * step);
    g_qi = 0;
    g_hits = 0;
    g_accum.clear();
    g_accum.reserve(g_queue.size());
    g_sampling = true;
    spdlog::info("[HEIGHTFIELD] sample START: {}x{} cells, extent {:.0f}u, step {:.1f}u, "
                 "player world=({:.0f},{:.0f}) local=({:.1f},{:.1f},{:.1f})",
                 res, res, g_req_extent, step, wx, wz, lx, ly, lz);
    // CONTROL: cast at the exact player local (the D2.1-probe condition) to confirm the cast still
    // works from this path/session before the grid loop.
    RayHit ctl{};
    bool cok = cast_down(lx, ly + 2000.f, lz, 4000.f, FILTER_GROUND, ctl);
    spdlog::info("[HEIGHTFIELD] sample CONTROL @player-local: hit={} y={:.2f}", cok, ctl.pt[1]);
    return true;
}

void step_sample()
{
    int done = 0;
    for (; g_qi < g_queue.size() && done < kCellsPerTick; ++g_qi, ++done)
    {
        const float cwx = g_queue[g_qi].first, cwz = g_queue[g_qi].second;
        // world XZ → cast LOCAL frame: translate by the captured player (local − world) offset.
        const float lx = g_local0x + (cwx - g_world0x);
        const float lz = g_local0z + (cwz - g_world0z);
        RayHit h{};
        Cell c{cwx, cwz, 0.f, 0.f, 1.f, 0.f, false};
        bool got = cast_down(lx, g_local0y + 2000.f, lz, 4000.f, FILTER_GROUND, h);
        if (g_qi < 3)
            spdlog::info("[HEIGHTFIELD] DBG cell{} world=({:.0f},{:.0f}) local=({:.1f},{:.1f}) "
                         "yhi={:.1f} -> hit={} y={:.2f} nrm=({:.2f},{:.2f},{:.2f})",
                         g_qi, cwx, cwz, lx, lz, g_local0y + 2000.f, got, h.pt[1], h.nrm[0], h.nrm[1], h.nrm[2]);
        if (got)
        {
            c.groundY = h.pt[1];
            c.nx = h.nrm[0]; c.ny = h.nrm[1]; c.nz = h.nrm[2];
            c.hit = true;
            ++g_hits;
        }
        g_accum.push_back(c);
    }
    if (g_qi >= g_queue.size())
    {
        g_sampling = false;
        // publish the completed field for the renderer.
        float ymin = 1e30f, ymax = -1e30f;
        for (const Cell &c : g_accum) if (c.hit) { ymin = c.groundY < ymin ? c.groundY : ymin; ymax = c.groundY > ymax ? c.groundY : ymax; }
        {
            std::lock_guard<std::mutex> lk(g_snap_mtx);
            g_snapshot = g_accum;
        }
        spdlog::info("[HEIGHTFIELD] sample DONE: {} cells, {} hits ({:.0f}%), groundY [{:.1f}..{:.1f}]",
                     g_accum.size(), g_hits,
                     g_accum.empty() ? 0.f : 100.f * g_hits / g_accum.size(),
                     ymin > 1e29f ? 0.f : ymin, ymax < -1e29f ? 0.f : ymax);
        g_queue.clear();
        g_accum.clear();
    }
}
} // namespace

void tick_game_thread()
{
    // One-shot validation probe (D2.1).
    if (g_probe_pending.exchange(false))
    {
        float px = 0.f, py = 0.f, pz = 0.f;
        if (goblin::get_player_world_pos(px, py, pz))
        {
            RayHit h{};
            if (cast_down(px, py + 2000.f, pz, 4000.f, FILTER_GROUND, h))
                spdlog::info("[HEIGHTFIELD] probe @player({:.1f},{:.1f},{:.1f}): GROUND y={:.2f} "
                             "(Δfoot={:.2f}) nrm=({:.3f},{:.3f},{:.3f}) frac={:.3f}",
                             px, py, pz, h.pt[1], h.pt[1] - py, h.nrm[0], h.nrm[1], h.nrm[2], h.dist);
            else
                spdlog::warn("[HEIGHTFIELD] probe @player({:.1f},{:.1f},{:.1f}): MISS", px, py, pz);
        }
        else spdlog::warn("[HEIGHTFIELD] probe: no player pos");
    }

    // Grid sampler (D2.2): start on request, then step rate-limited until the queue drains.
    if (g_sample_pending.exchange(false)) start_sample();
    if (g_sampling) step_sample();
}

void request_present_probe() { g_present_probe_pending.store(true); }

void tick_present()
{
    if (!g_present_probe_pending.exchange(false)) return;
    // Only cast when the world is ACTIVE: in-world AND the map is CLOSED (map-open unloads collision).
    if (goblin::world_map_open())
    {
        spdlog::warn("[HEIGHTFIELD] PRESENT probe skipped — map is open (close it; world collision "
                     "unloads while the map is up)");
        return;
    }
    float px = 0.f, py = 0.f, pz = 0.f;
    if (!goblin::get_player_world_pos(px, py, pz))
    {
        spdlog::warn("[HEIGHTFIELD] PRESENT probe: no player pos");
        return;
    }
    diag_ctx(px, py, pz);   // D2.2 diagnostic: try every ctx interpretation, log which hits
    RayHit h{};
    bool ok = cast_down(px, py + 2000.f, pz, 4000.f, FILTER_GROUND, h);
    if (ok)
        spdlog::info("[HEIGHTFIELD] PRESENT probe @player({:.1f},{:.1f},{:.1f}): GROUND y={:.2f} "
                     "(Δfoot={:.2f}) nrm=({:.3f},{:.3f},{:.3f}) frac={:.3f} — PRESENT-THREAD GAMEPLAY CAST WORKS",
                     px, py, pz, h.pt[1], h.pt[1] - py, h.nrm[0], h.nrm[1], h.nrm[2], h.dist);
    else
        spdlog::warn("[HEIGHTFIELD] PRESENT probe @player({:.1f},{:.1f},{:.1f}): MISS (collision not "
                     "streamed here, or cast unresolved)", px, py, pz);
}
} // namespace goblin::heightfield
