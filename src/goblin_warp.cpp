#include "goblin_warp.hpp"

#include "goblin_heightfield.hpp"     // ground_check_sync — live teleport-target validity check
#include "goblin_inject.hpp"          // get_player_world_pos — loading-state guard
#include "goblin_load_watchdog.hpp"
#include "modutils.hpp"
#include "re_signatures.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cmath>
#include <cstdint>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace goblin::warp
{
namespace
{
// LuaWarp_01(rcx, rdx, r8d) — the game's Lua-event warp. rcx/rdx from CSLuaEventManager,
// r8d = graceId - 1000. Return value unused (the CT stub discards it).
using LuaWarpFn = uint64_t(*)(void *a, void *b, uint32_t code);

LuaWarpFn g_warp = nullptr;
void **g_lem_slot = nullptr;   // *g_lem_slot == CSLuaEventManager (re-read each call)
std::atomic<bool> g_ready{false};
}  // namespace

void initialize()
{
    if (g_ready.load()) return;
    // LuaWarp_01 = LUA_WARP match + 2 (the AOB anchors on the prior fn's C3 ret).
    if (auto *m = reinterpret_cast<uint8_t *>(modutils::scan<void>({.aob = goblin::sig::LUA_WARP})))
        g_warp = reinterpret_cast<LuaWarpFn>(m + 2);
    // CSLuaEventManager slot: mov rax,[rip+disp32] — disp @ +3, instruction length 7.
    if (auto *m = reinterpret_cast<uint8_t *>(
            modutils::scan<void>({.aob = goblin::sig::CS_LUA_EVENT_MANAGER})))
    {
        int32_t disp = *reinterpret_cast<int32_t *>(m + 3);
        g_lem_slot = reinterpret_cast<void **>(m + 7 + disp);
    }
    g_ready.store(true);
    spdlog::info("[WARP] LuaWarp_01={} CSLuaEventManager_slot={} ({})", (void *)g_warp,
                 (void *)g_lem_slot, (g_warp && g_lem_slot) ? "OK" : "MISS");
}

// The raw game call, isolated so the caller's __try wraps a lone opaque CALL (clang-cl keeps
// that; a __try around inline loads gets elided — see clang-cl-seh-noinline).
__declspec(noinline) static uint64_t call_warp(void *a, void *b, uint32_t code)
{
    return g_warp(a, b, code);
}

static bool warp_seh(void *a, void *b, uint32_t code)
{
    __try
    {
        call_warp(a, b, code);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool to_grace(int32_t grace_id, int32_t offset)
{
    if (!g_ready.load()) initialize();
    if (!g_warp || !g_lem_slot) { spdlog::warn("[WARP] unresolved — to_grace skipped"); return false; }

    // Reject a warp while the world is NOT playable (a loading screen is up = LocalPlayer null). This
    // guards re-entrancy: double-clicking a 2nd grace DURING the first warp's load would fire LuaWarp
    // mid-transition → the player lands in a weird/wrong position. get_player_world_pos returns false
    // exactly while LocalPlayer is null, so this blocks both the re-entrant warp and any mid-load warp.
    float px = 0.f, py = 0.f, pz = 0.f;
    if (!goblin::get_player_world_pos(px, py, pz))
    {
        spdlog::warn("[WARP] world not playable (loading / not in-world) — to_grace skipped (re-entrancy guard)");
        return false;
    }

    // NB: id-validation against the live BonfireWarpParam was ATTEMPTED (2026-07-05) but is NOT feasible
    // with the current data — a valid warp id (e.g. 1042362951 = First Step) matches NEITHER the captured
    // rowId nor bonfireEntityId (which read as 8-digit 10001950-style values). The true warp-id↔param
    // mapping (ERR remaps graces) needs its own RE before a whitelist can gate bad ids. Until then, warping
    // to a non-grace id strands the player at local=(0,0,0)/map-unresolved (recoverable — a valid warp
    // fixes it). See docs/memory/bugs/. The load watchdog below still catches a HUNG warp.

    // Live CSLuaEventManager, guarded (the singleton may not exist before the world loads).
    void *lem = nullptr;
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), g_lem_slot, &lem, sizeof(lem), &got) ||
        got != sizeof(lem) || reinterpret_cast<uintptr_t>(lem) < 0x10000)
    {
        spdlog::warn("[WARP] CSLuaEventManager null (not in-world?) — to_grace skipped");
        return false;
    }
    void *a = nullptr, *b = nullptr;
    auto *L = reinterpret_cast<uint8_t *>(lem);
    if (!ReadProcessMemory(GetCurrentProcess(), L + 0x18, &a, sizeof(a), &got) || got != sizeof(a) ||
        !ReadProcessMemory(GetCurrentProcess(), L + 0x08, &b, sizeof(b), &got) || got != sizeof(b))
    {
        spdlog::warn("[WARP] CSLuaEventManager fields unreadable — to_grace skipped");
        return false;
    }

    uint32_t code = static_cast<uint32_t>(grace_id + offset);  // r8d = grace_id + offset (offset=0 = entity id direct)
    // Arm the load watchdog BEFORE the call: a hung fast-travel keeps the loading screen
    // rendering (freeze watchdog blind), so this catches the "infinite loading" and dumps
    // all-thread stacks + the target grace for diagnosis.
    goblin::load_watchdog::arm_warp(grace_id);
    bool ok = warp_seh(a, b, code);
    spdlog::info("[WARP] to_grace grace_id={} offset={} code={} lem={} -> {}", grace_id, offset, code, lem,
                 ok ? "called" : "FAULTED");
    return ok;
}

// Coordinate teleport = write the player's HAVOK PHYSICS BODY position directly, the way
// er_console_mod's `tp` does (RE'd from its DLL 2026-07-07, live-verified on ERRv2.2.9.6). The
// authoritative body pos is a Vec3 at:
//     posObj = *(*(LocalPlayer + 0x190 /*chr module*/) + 0x68);  Vec3 @ posObj + 0x70/0x74/0x78
// Writing it MOVES the body and HOLDS (unlike LocalPlayer+0x6C0, an output mirror the physics
// thread reclaims each frame — that snap-back was the old warp_local/warp_xyz bug). The body's
// frame is havok/physics-block-local, offset from the LocalPlayer+0x6C0 tile-local frame by a
// per-block origin — but the two differ only by a translation, so a DELTA maps 1:1 (verified live:
// havok X += 20 → tile-local X += 20). So to reach a tile-local target we convert:
//     havok_target = target - (tile_now - havok_now).
// Same noinline-body + SEH shape as the other raw player writes (clang-cl SEH-elision guard).
struct BodyPos { float x, y, z; };
__declspec(noinline) static uint8_t *resolve_body_vec(uint8_t *lp)
{
    auto *mod = *reinterpret_cast<uint8_t **>(lp + 0x190);   // ChrIns physics module (same +0x190 as HP)
    if (!mod) return nullptr;
    auto *pos = *reinterpret_cast<uint8_t **>(mod + 0x68);   // position holder
    return pos ? pos + 0x70 : nullptr;                        // &Vec3 (X @ +0x70, Y +0x74, Z +0x78)
}
// FALLBACK cap only — used when the LIVE ground check below is unavailable (loading, native map
// open, cast unresolved). ~1500 m is the proven open-world streaming-follow distance. When the
// live check runs it is authoritative and distance is irrelevant: loaded collision at the target
// column ⇒ safe; NO collision ⇒ refuse even 40 m away. Both save-poisoning shapes were hit on
// 2026-07-07 — an ~11 km MapId-void jump AND a 40 m hop into Agheel Lake's floorless middle;
// each free-fell, autosaved mid-fall, and wedged every subsequent load of the save.
constexpr float kMaxTeleportDelta = 1500.0f;

__declspec(noinline) static void teleport_body_body(uint8_t *lp, float tx, float ty, float tz, bool *ok)
{
    uint8_t *vec = resolve_body_vec(lp);
    if (!vec) return;
    // Current havok body pos + current tile-local pos → the per-block frame offset, then write the
    // delta-converted target back into the body vec.
    float hx = reinterpret_cast<float *>(vec)[0];
    float hy = reinterpret_cast<float *>(vec)[1];
    float hz = reinterpret_cast<float *>(vec)[2];
    float lx = *reinterpret_cast<float *>(lp + 0x6C0);
    float ly = *reinterpret_cast<float *>(lp + 0x6C4);
    float lz = *reinterpret_cast<float *>(lp + 0x6C8);
    reinterpret_cast<float *>(vec)[0] = tx - (lx - hx);
    reinterpret_cast<float *>(vec)[1] = ty - (ly - hy);
    reinterpret_cast<float *>(vec)[2] = tz - (lz - hz);
    *ok = true;
}
static bool teleport_body_seh(uint8_t *lp, float tx, float ty, float tz)
{
    bool ok = false;
    __try { teleport_body_body(lp, tx, ty, tz, &ok); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    return ok;
}

bool teleport_coords(float x, float y, float z)
{
    // LocalPlayer is null during a load / at the menu — the not-in-world + re-entrancy guard.
    auto *lp = reinterpret_cast<uint8_t *>(goblin::get_local_player_ptr());
    if (!lp)
    {
        spdlog::warn("[WARP] LocalPlayer null (loading / not in-world) — teleport_coords skipped");
        return false;
    }
    float lx = 0.f, ly = 0.f, lz = 0.f;
    if (!goblin::get_player_world_pos(lx, ly, lz))
    {
        spdlog::warn("[WARP] player pos unreadable — teleport_coords skipped");
        return false;
    }
    const float dx = x - lx, dy = y - ly, dz = z - lz;
    if (!(dx == dx && dy == dy && dz == dz))   // NaN target
    {
        spdlog::warn("[WARP] teleport_coords REFUSED — NaN target");
        return false;
    }
    // LIVE validity check (authoritative): does walkable collision exist in the target column?
    float gy = 0.f;
    const int gc = goblin::heightfield::ground_check_sync(x, y, z, &gy);
    if (gc == 0)
    {
        spdlog::warn("[WARP] teleport_coords REFUSED — no loaded collision at target "
                     "({:.1f},{:.1f},{:.1f}): void / deep-water kill column / unstreamed. "
                     "Use the grace warp for cross-map travel.", x, y, z);
        return false;
    }
    if (gc < 0 && (dx * dx + dz * dz > kMaxTeleportDelta * kMaxTeleportDelta ||
                   dy < -kMaxTeleportDelta || dy > kMaxTeleportDelta))
    {
        spdlog::warn("[WARP] teleport_coords REFUSED — live ground check unavailable and target "
                     "({:.1f},{:.1f},{:.1f}) is beyond the {:.0f} m fallback cap.",
                     x, y, z, kMaxTeleportDelta);
        return false;
    }
    bool ok = teleport_body_seh(lp, x, y, z);
    spdlog::info("[WARP] teleport_coords ({:.2f},{:.2f},{:.2f}) ground_check={} gy={:.1f} lp={} -> {}",
                 x, y, z, gc, gy, (void *)lp, ok ? "moved" : "FAULTED");
    return ok;
}

// ── FAR teleport (streamed) ── beyond-bubble targets go through the game's own streaming:
// LuaWarp to the nearest discovered grace (full area-load), then one ground-checked hop.
namespace
{
std::atomic<int> g_far_state{0};        // 0 idle · 1 warping (load in flight) · 2 hop pending
float g_far_wx = 0.f, g_far_wz = 0.f;   // target, unified world/marker frame
float g_far_gwx = 0.f, g_far_gwz = 0.f; // chosen grace, same frame
uint32_t g_far_deadline_ms = 0;
// The hop is worth doing only when the grace actually brings the target into the streamed
// bubble — beyond this, refuse the request outright (nothing to bridge from).
constexpr float kFarGraceMax = 1200.0f;
}  // namespace

bool far_teleport_active() { return g_far_state.load() != 0; }

bool request_far_teleport_world(float wx, float wz)
{
    if (g_far_state.load() != 0)
    {
        spdlog::warn("[WARP] far-teleport already in flight — request ignored");
        return false;
    }
    float px = 0.f, py = 0.f, pz = 0.f;
    if (!goblin::get_player_world_pos(px, py, pz))
    {
        spdlog::warn("[WARP] far-teleport: not in-world");
        return false;
    }
    // Nearest DISCOVERED overworld/DLC-OW grace to the TARGET (their grid*256+pos is already the
    // unified frame; folded areas would need a projection pass — v1 keeps them out of scope).
    const goblin::LiveGrace *best = nullptr;
    float bestd2 = 1e30f, bwx = 0.f, bwz = 0.f;
    for (const auto &g : goblin::live_graces())
    {
        if (g.areaNo != 60 && g.areaNo != 61) continue;
        if (!g.bonfireEntityId || !g.discoverFlag) continue;
        const float gwx = g.gridXNo * 256.0f + g.posX, gwz = g.gridZNo * 256.0f + g.posZ;
        const float dx = gwx - wx, dz = gwz - wz, d2 = dx * dx + dz * dz;
        // distance test first — the flag read runs only for improving candidates.
        if (d2 < bestd2 && goblin::ui::read_event_flag(static_cast<uint32_t>(g.discoverFlag)))
        {
            bestd2 = d2; best = &g; bwx = gwx; bwz = gwz;
        }
    }
    if (!best)
    {
        spdlog::warn("[WARP] far-teleport: no discovered overworld grace at all");
        return false;
    }
    if (bestd2 > kFarGraceMax * kFarGraceMax)
    {
        spdlog::warn("[WARP] far-teleport REFUSED: nearest discovered grace is {:.0f} m from the "
                     "target ({:.0f} m max) — nothing to bridge from",
                     std::sqrt(bestd2), kFarGraceMax);
        return false;
    }
    g_far_wx = wx; g_far_wz = wz; g_far_gwx = bwx; g_far_gwz = bwz;
    g_far_deadline_ms = GetTickCount() + 45000;
    if (!to_grace(static_cast<int32_t>(best->bonfireEntityId), 0))
        return false;
    g_far_state.store(1);
    spdlog::info("[WARP] far-teleport: grace {} ({:.0f} m from target w({:.0f},{:.0f})) — load in "
                 "flight, hop follows", best->bonfireEntityId, std::sqrt(bestd2), wx, wz);
    return true;
}

void tick_far_teleport()
{
    const int st = g_far_state.load();
    if (st == 0) return;
    if (GetTickCount() > g_far_deadline_ms)
    {
        spdlog::warn("[WARP] far-teleport timed out (load never settled) — cancelled");
        g_far_state.store(0);
        return;
    }
    int area = 0;
    float pwx = 0.f, pwz = 0.f;
    if (!goblin::get_player_map_pos(area, pwx, pwz))   // loading screen → wait
        return;
    if (st == 1)
    {
        const float dx = pwx - g_far_gwx, dz = pwz - g_far_gwz;
        if (dx * dx + dz * dz > 60.0f * 60.0f) return;   // not at the grace yet
        g_far_state.store(2);                             // arrived — settle one frame, then hop
        return;
    }
    // st == 2 — the hop. Convert the unified-frame target into tile-local via the raw-pos delta
    // (same math as warp_to_world_xz), then teleport at the CHECKED ground height + 2.
    float lx = 0.f, ly = 0.f, lz = 0.f;
    if (!goblin::get_player_world_pos(lx, ly, lz)) return;
    int rarea = 0;
    float cwx = 0.f, cwz = 0.f;
    if (!goblin::get_player_raw_pos(rarea, cwx, cwz)) return;   // transient MapId → retry next frame
    g_far_state.store(0);
    const float tx = lx + (g_far_wx - cwx), tz = lz + (g_far_wz - cwz);
    float gy = 0.f;
    const int gc = goblin::heightfield::ground_check_sync(tx, ly, tz, &gy);
    if (gc == 0)
    {
        spdlog::warn("[WARP] far-teleport: target column has NO ground — staying at the grace");
        return;
    }
    const bool ok = teleport_coords(tx, gc == 1 ? gy + 2.0f : ly, tz);
    spdlog::info("[WARP] far-teleport hop -> {}", ok ? "done" : "refused/failed (player at the grace)");
}
}  // namespace goblin::warp
