#pragma once

// Live geom-instance MOVE primitive — the first live test of the transform SETTER found statically in
// docs/re/windows_msb_placement_write_re_findings.md (vtable[0xd0] SetWorldMatrix on CSWorldGeomIns).
// Picks a live geom instance from the FieldIns registry ([er+0x3d7b0c0]→+0x10→+0x720→map, the same
// chain goblin_collected's [FIELDINS-B] walk uses), reads its world matrix via the engine's own getter
// (FUN_1406c46e0(inst+0x18,&mat)), offsets the translation, and calls the virtual setter. Present-thread
// only (the setter drives physics/render). Dev RE tool — behind the `move_asset` RPC.

#include <cstdint>

namespace goblin::geom_move
{
    struct MoveResult
    {
        bool ok = false;
        uint64_t inst = 0;      // chosen instance address
        uint64_t vtable = 0;    // its vtable (absolute)
        float before[3] = {};   // translation before the move
        float moved[3] = {};    // translation after the +delta setter call
        float restored[3] = {}; // translation after the restore setter call
        char err[96] = {};      // failure reason (ok=false)
    };

    // Move the first live geom instance by (dx,dy,dz) via the virtual setter, read back, then restore
    // it to its original transform (so the world is left unchanged). Proves the setter round-trips.
    MoveResult move_first(float dx, float dy, float dz);

    // Persistence probe: move the first live geom instance by (dx,dy,dz) and DO NOT restore — remember
    // the instance + its pre-move translation. `moved` = the +0x220 translation right after the setter.
    MoveResult move_hold(float dx, float dy, float dz);
    // Re-read the held instance's current +0x220 translation (into `moved`; `before` = the remembered
    // pre-move value). Lets a poll loop watch whether the engine reverts a cache-only write over frames.
    // ok=false if nothing is held or the instance is no longer readable.
    MoveResult read_held();
}
