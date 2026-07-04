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
}  // namespace goblin::warp
