#pragma once
#include <cstdint>
#include <string>

// Load-wedge rescue — the DLL-side fix for the "warp inconnu" infinite-load.
//
// A coordinate teleport/fall OUTSIDE any mapped volume can leave the engine's current-map tracker
// INVALID (area byte 0); if an autosave lands then, the .err records an unloadable spawn and every
// subsequent load hangs on the loading screen forever. RE:
// docs/re/windows_load_wedge_mapid_writer_re_findings.md.
//
// This hooks the spawn/warp APPLIER FUN_1406260e0 (SetPlayerMapAndPos) — the funnel that commits BOTH
// the current MapId (via the SetCurrentMap notifier FUN_140627fc0) AND the player position at load/warp.
// It ALWAYS logs each call (mapId + area + pos) for diagnosis; when ARMED it also rewrites an INVALID
// (area==0) incoming spawn to a known-good target (captured from a healthy load, default First Step)
// so the load can't wedge.
namespace goblin::load_rescue
{
    // Resolve SPAWN_APPLIER + install the hook (idempotent; logs [LOADRESCUE]). Called at init.
    void install();

    // RPC `load_rescue [status|on|off|verbose 0|1|capture|set <mapHex>]`:
    //   status     — armed?, verbose?, safe-target set?, and the last few applier calls seen.
    //   on|off     — arm/disarm the area==0 → safe-target substitution (default OFF: log-only).
    //   verbose    — toggle per-call [LOADRESCUE] logging (default ON during diagnosis).
    //   capture    — snapshot the NEXT valid applier call (map+pos) as the safe rescue target.
    //   set <hex>  — set the safe target MapId directly (pos still needs a capture).
    std::string command(const std::string &rest);
}
