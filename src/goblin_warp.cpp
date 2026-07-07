#include "goblin_warp.hpp"

#include "goblin_inject.hpp"          // get_player_world_pos — loading-state guard
#include "goblin_load_watchdog.hpp"
#include "modutils.hpp"
#include "re_signatures.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
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
// Max tile-local delta a coordinate teleport may apply, metres. Open-world streaming follows
// ~1500 m of hop (goblin_inject.hpp, write_player_local_pos note); past that the target is
// unstreamed VOID — the player free-falls, hits the kill plane, and the save gets poisoned at
// the void position (2026-07-07 incident: a bad frame resolve asked for an ~11 km jump; the
// resulting save wedged every subsequent load). Validate the warp here, at the LAST writer,
// so no caller (RPC warp_local/warp_xyz, vmap click-to-warp) can void-jump.
constexpr float kMaxTeleportDelta = 1500.0f;

__declspec(noinline) static void teleport_body_body(uint8_t *lp, float tx, float ty, float tz,
                                                    bool *ok, bool *refused)
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
    float dx = tx - lx, dy = ty - ly, dz = tz - lz;
    // NaN targets (x != x) and beyond-streaming-gate jumps are invalid — refuse, don't move.
    if (!(dx == dx && dy == dy && dz == dz) ||
        dx * dx + dz * dz > kMaxTeleportDelta * kMaxTeleportDelta ||
        dy < -kMaxTeleportDelta || dy > kMaxTeleportDelta)
    {
        *refused = true;
        return;
    }
    reinterpret_cast<float *>(vec)[0] = tx - (lx - hx);
    reinterpret_cast<float *>(vec)[1] = ty - (ly - hy);
    reinterpret_cast<float *>(vec)[2] = tz - (lz - hz);
    *ok = true;
}
static bool teleport_body_seh(uint8_t *lp, float tx, float ty, float tz, bool *refused)
{
    bool ok = false;
    __try { teleport_body_body(lp, tx, ty, tz, &ok, refused); }
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
    bool refused = false;
    bool ok = teleport_body_seh(lp, x, y, z, &refused);
    if (refused)
    {
        spdlog::warn("[WARP] teleport_coords REFUSED — target ({:.1f},{:.1f},{:.1f}) is more than "
                     "{:.0f} m from the player (streaming gate; a farther jump lands in unstreamed "
                     "void). Use the grace warp for cross-map travel.",
                     x, y, z, kMaxTeleportDelta);
        return false;
    }
    spdlog::info("[WARP] teleport_coords ({:.2f},{:.2f},{:.2f}) lp={} -> {}",
                 x, y, z, (void *)lp, ok ? "moved" : "FAULTED");
    return ok;
}
}  // namespace goblin::warp
