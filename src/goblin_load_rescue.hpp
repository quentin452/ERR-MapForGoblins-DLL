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
// This hooks the universal map-change SETTER FUN_140627fc0 (SetCurrentMap) — the funnel EVERY map
// change flows through (tile crossings + the load-spawn; proven live). It ALWAYS logs each call
// (mapId + area) for diagnosis; when ARMED it rewrites an INVALID (area==0) incoming map to a valid
// one (default First Step) so streaming can't chase a nonexistent map = no infinite load. MAP-ONLY
// (the funnel has no position; a fixed map un-wedges, then the player falls→dies→respawns at grace).
namespace goblin::load_rescue
{
    // Resolve MAP_SETTER + install the hook (idempotent; logs [LOADRESCUE]). Called at init.
    void install();

    // RPC `load_rescue [status|on|off|verbose 0|1|set <mapHex>]`:
    //   status     — installed?, armed?, verbose?, safe map, and the last few setter calls seen.
    //   on|off     — arm/disarm the area==0 → safe-map substitution (default OFF: log-only).
    //   verbose    — toggle per-call [LOADRESCUE] logging. Default OFF: the setter fires on every
    //                streaming transition (hundreds of lines a session). A real RESCUE always logs,
    //                and `status` replays the last calls either way.
    //   set <hex>  — set the safe rescue MapId (default First Step 0x3C2A2300).
    std::string command(const std::string &rest);
}
