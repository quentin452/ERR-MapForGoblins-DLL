#pragma once
// Route D — walkable-greybox collision box: spawn a Havok hknpBoxShape body into the live world so the
// player can stand on / be blocked by it, ZERO authored art. The minimal "our own asset" brick for custom
// 3D worlds. RE: docs/re/hknpworld_addbody_slot_re_findings.md + add_collision_linux_impl_brief.md.
#include <cstdint>

namespace goblin::add_collision
{
struct Result
{
    bool ok = false;
    uint64_t world = 0, bodyMgr = 0, bodies = 0, shape = 0;
    uint32_t count = 0, bodyId = 0;
    char err[160] = {};
};

// Read-only: resolve hknpWorld/bodyMgr from the PhysWorld singleton (no allocation, no call). Sanity gate.
Result resolve_world();

// Phase-1 RECON: resolve + call hknpBodyCinfo init on a scratch buffer + read body[0], logging both byte
// dumps ([ADDCOL]) so the cinfo field layout (shape/pos/orient/motionType/flags) can be pinned. No world
// mutation → safe from the present/RPC thread. Returns the resolve result (dumps go to the log).
Result recon();
} // namespace goblin::add_collision
