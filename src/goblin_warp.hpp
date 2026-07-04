#pragma once
#include <cstdint>

// Grace warp — fast-travel the player to a site of grace (dev-world navigation / testing the
// overlay across map areas without walking). Calls the game's OWN Lua-event warp (LuaWarp_01),
// so it does a proper area-load — no falling through the world. AOBs + convention from the
// Hexinton all-in-one CT ("Coinsworth"): LuaWarp_01(rcx=[CSLuaEventManager+0x18],
// rdx=[CSLuaEventManager+0x08], r8d = graceId - 1000).
//
// graceId = the bonfire entity id (BonfireWarpParam), e.g. 1042362951 = The First Step,
// 10002951 = Margit. The player must have the grace UNLOCKED (the game validates). Warping
// changes player position, which the game persists on its next save — expected for a dev tool.
namespace goblin::warp
{
    // Resolve LuaWarp_01 + CSLuaEventManager (idempotent; logs [WARP]). Called at init; safe
    // no-op on any AOB miss (to_grace then returns false).
    void initialize();

    // Fast-travel to the grace `grace_id` (full bonfire entity id). Calls LuaWarp_01 on the
    // CURRENT thread (present thread for the RPC path) — must be a safe point, in-world.
    // SEH-guarded. Returns false if unresolved or the call faulted.
    // `offset` is added to grace_id before the call (LuaWarp_01 r8d = grace_id + offset). The
    // Hexinton CT convention was -1000 (bonfire entity → warp/spawn-point id), but that offset is
    // NOT uniform across graces (some land one bonfire off), so it's exposed for empirical tuning.
    bool to_grace(int32_t grace_id, int32_t offset = -1000);
}
