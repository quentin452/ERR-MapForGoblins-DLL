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

    // Coordinate teleport — move the player to (x,y,z) in the LocalPlayer+0x6C0 tile-local frame
    // (the same frame get_player_world_pos reads and warp_local/warp_xyz use). Writes the player's
    // HAVOK PHYSICS BODY Vec3 directly (`*(*(LocalPlayer+0x190)+0x68)+0x70/74/78`), the way
    // er_console_mod's `tp` does — this MOVES the body and HOLDS, unlike a raw +0x6C0 store (an
    // OUTPUT MIRROR the physics thread reclaims each frame → snap-back, the old warp bug). The body
    // frame is offset from the tile-local frame by a per-block origin, so we convert by delta
    // (havok_target = target - (tile_now - havok_now)). Intra-region only — a far cross-map target
    // may land in unstreamed void (use to_grace for a full area-load). SEH-guarded; safe on the
    // current thread (RPC/present — live-verified). Returns false if not in-world / the chain is
    // null / the write faulted. RE: docs/re/linux_player_pos_write_setpos_re_findings.md.
    bool teleport_coords(float x, float y, float z);

    // FAR teleport — reach a target BEYOND the streamed bubble by driving the game's OWN
    // streaming instead of refusing: LuaWarp to the nearest DISCOVERED grace (full area-load,
    // proper placement), then one short ground-checked body hop to the exact target. Async —
    // request from any thread while in-world; tick_far_teleport() (present thread, goblin_overlay)
    // advances the state machine across the load. v1 scope: overworld/DLC-OW targets in the
    // unified world/marker frame (their grace grid*256+pos IS that frame); refuses when no
    // discovered grace lies within ~1200 m of the target (nothing to bridge from).
    // RPC `warp_far`; also the fallback when a direct vmap click-to-warp is refused.
    bool request_far_teleport_world(float wx, float wz);
    void tick_far_teleport();       // call once per present frame
    bool far_teleport_active();
}
