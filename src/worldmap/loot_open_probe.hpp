#pragma once
// DIAGNOSTIC (config diag_map_opens, default off): hook kernel32!CreateFileW and
// log every map file the GAME opens — full resolved OS path (post ME3/UXM
// redirect), open latency, and time since arming. Lets us compare WHERE the game
// actually reads maps from (loader-agnostic ground truth) vs the [LOOTDISK]
// ancestor-walk dir, and WHEN the first map open happens vs the init-time build.
//
// BOOT I/O PROFILE (config diag_boot_io, default off): the same observer widened
// to EVERY file open, hooked live immediately (not queued for enable_hooks) so it
// covers the whole boot — including the ~8s regulation/param wait. [BOOTIO] lines
// carry +ms-since-arming + per-open latency; correlate with the init-phase log
// timestamps to see what the startup actually waits on. Read-only.
//
// CAPTURE ROLE (always armed — loot_from_disk_msb is on by default): every map
// file the game OPENS successfully (.msb.dcx / .msb / .mapbnd[.dcx]) is recorded
// with its exact resolved path. The resident-MSB path source (resident_msb.cpp)
// reads + decompresses those exact files — no memory scan, no dir-resolution
// walk. ME3/UXM redirect BELOW CreateFileW, so the captured path IS the active
// mod's real file (loader-agnostic ground truth, whatever mounts the data).
#include <cstdint>
#include <string>
#include <vector>

namespace goblin::worldmap
{
// Idempotent — safe to call from both the early boot-io arm point and the normal
// post-from_params site; the first effective call installs the hook.
void install_map_open_probe();

// One map file the game opened (CreateFileW observer), with its exact resolved path.
struct CapturedMapFile
{
    std::string path;    // exact resolved OS path, e.g. "...\GA\map\MapStudio\m60_41_33_00.msb.dcx"
    std::string name;    // tile stem ("m60_41_33_00") from the filename; empty if not tile-named
    bool        isMsb = false;  // .msb.dcx / .msb (slice-1 parseable) vs .mapbnd[.dcx] (recorded for the Oodle-join slice)
    uint8_t     area = 0, gx = 0, gz = 0;  // from name; 0 when name didn't parse
};

// Snapshot of the captured map-file paths (deduped, successful opens only). Thread-safe;
// grows as the game streams maps. Callers read the whole snapshot under one lock.
std::vector<CapturedMapFile> captured_map_files();

// Exact captured path for a VIRTUAL game path ("msg/engus/item_dlc02.msgbnd.dcx" → the real
// file the game opened, tail-matched, case-insensitive). Empty if the game hasn't opened that
// virtual file yet. The mod-agnostic ground truth for the file readers: consult BEFORE the
// ancestor-walk (which misses exotic mounts like GA's <root>/GA/ and falls back to vanilla).
std::string captured_path_for(const std::string &rel_path);
} // namespace goblin::worldmap
