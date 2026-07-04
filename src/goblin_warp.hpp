#pragma once
#include <cstdint>

// Grace warp — fast-travel the player to a site of grace (dev-world navigation / testing the
// overlay across map areas without walking). Calls the game's OWN Lua-event warp (LuaWarp_01),
// so it does a proper area-load — no falling through the world. AOBs from the Hexinton all-in-one
// CT ("Coinsworth"): LuaWarp_01(rcx=[CSLuaEventManager+0x18], rdx=[CSLuaEventManager+0x08], r8d =
// grace id). NOTE: the CT documented r8d = graceId-1000, but in-game that lands one bonfire off —
// the correct value is the bonfireEntityId DIRECTLY (offset 0, ground-truthed 2026-07-04).
//
// graceId = the bonfire entity id (BonfireWarpParam.bonfireEntityId @0x08), e.g. 1042362951 = The
// First Step, 10002951 = Margit. The player must have the grace UNLOCKED (the game validates). Warping
// changes player position, which the game persists on its next save — expected for a dev tool.
namespace goblin::warp
{
    // Resolve LuaWarp_01 + CSLuaEventManager (idempotent; logs [WARP]). Called at init; safe
    // no-op on any AOB miss (to_grace then returns false).
    void initialize();

    // Fast-travel to the grace `grace_id` (full bonfire entity id). Calls LuaWarp_01 on the
    // CURRENT thread (present thread for the RPC path) — must be a safe point, in-world.
    // SEH-guarded. Returns false if unresolved or the call faulted.
    // `offset` is added to grace_id before the call (LuaWarp_01 r8d = grace_id + offset). GROUND TRUTH
    // (in-game, 2026-07-04): the correct offset is **0** — LuaWarp_01 wants the bonfireEntityId DIRECTLY.
    // The Hexinton CT's -1000 was WRONG (it landed one bonfire off, in the same map cell). Kept as a
    // param only for any future per-grace exception; default 0.
    bool to_grace(int32_t grace_id, int32_t offset = 0);
}
