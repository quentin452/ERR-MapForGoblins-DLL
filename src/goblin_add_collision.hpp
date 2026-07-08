#pragma once
// Route D — walkable-greybox collision box: spawn a Havok hknpBoxShape body into the live world so the
// player can stand on / be blocked by it, ZERO authored art. The minimal "our own asset" brick for custom
// 3D worlds. RE: docs/re/hknpworld_addbody_slot_re_findings.md + add_collision_linux_impl_brief.md.
#include <cstdint>

#include "goblin_dll_export.hpp"  // GOBLIN_RENDER_API — host exports these to the render DLL (hot-reload split)

namespace goblin::add_collision
{
struct Result
{
    bool ok = false;
    uint64_t world = 0, bodyMgr = 0, bodies = 0, shape = 0;
    uint32_t count = 0, bodyId = 0;
    float half[3] = {}, pos[3] = {};
    char err[160] = {};
};

// Read-only: resolve hknpWorld/bodyMgr from the PhysWorld singleton (no allocation, no call). Sanity gate.
GOBLIN_RENDER_API Result resolve_world();

// Phase-1 RECON: resolve + call hknpBodyCinfo init on a scratch buffer + read body[0], logging both byte
// dumps ([ADDCOL]) so the cinfo field layout (shape/pos/orient/motionType/flags) can be pinned. No world
// mutation → safe from the present/RPC thread. Returns the resolve result (dumps go to the log).
GOBLIN_RENDER_API Result recon();

// Phase-2 ADD (findings §6): build an hknpBodyCinfo — init defaults, then the minimal STATIC fill
// (+0x00 shape / +0x30 position; orientation stays identity, motionType stays 0 = STATIC) — and
// allocateBody + addBody it into the live broadphase. The shape is BORROWED from a live body (the
// findings' first-probe shortcut: proves alloc→add→broadphase→hf_probe before the box builder + its
// BuildCfg map are invested in), so `half` is recorded/logged but not yet honored. `pos` is the Havok
// BLOCK-LOCAL frame — the same frame hf_probe casts in (brief §7).
//   force=false → resolve + build + DUMP the filled cinfo only; NO alloc, no world mutation.
//   force=true  → allocateBody (FUN_1418aabf0) + addBody (FUN_1418a9ff0). addMode/actMode gate that
//                 engine addBody — the "solid vs ray-only" broadphase behaviour; -1 = the engine-flush
//                 default (0,0, like the CS flush). Present-thread first attempt (brief §4) — the freeze
//                 watchdog covers a stall; fall back to a FUN_140c72c20-mirror game-thread hook if it does.
// Verify with hf_probe_present at pos: a down-ray hit at the body's top = it is live in the broadphase.
// filterInfo is stamped at body+0x6c BEFORE addBody so the CHARACTER controller collides (not just
// raycasts): layer = filterInfo & 0x7f, gated by the 128-layer CSCollisionFilter matrix
// (er+0xc61940/c61d70/c61be0). -1 => 0x38 (layer 56 = the walkable static-map layer). A post-add write
// is ignored — addBody caches the filter into the broadphase leaf. RPC-tunable via
// `add_collision ... go <addMode> <actMode> <filterInfo>`.
GOBLIN_RENDER_API Result add_box(const float half[3], const float pos[3], bool force,
                                 int addMode = -1, int actMode = -1, int filterInfo = -1);
} // namespace goblin::add_collision
