#pragma once
// Resident-MSB loot source — derives treasure/collectible/enemy/region/ObjAct placements from
// the maps the ACTIVE mod actually loads. RE (2026-06-24, live-proven): docs/re/windows_runtime_msb_resident_re_findings.md +
// docs/re/windows_resident_msbe_layout_re_findings.md + docs/memory/tooling/runtime-msb-resident-plan.md.
//
// WHY THIS EXISTS (user 2026-08-14, Golden Age via ME3): the disk route resolves the map dir by
// walking the DLL's ancestors for <p>/mod/map/MapStudio — but GA mounts its data at <root>/GA/
// (ME3 package path), so the walk missed it and fell back to the VANILLA install's maps. This
// source can't miss: it reads the EXACT files the game opened.
//
// ENUMERATION (2026-08-14 REWORK — path capture, no memory scan): the CreateFileW observer
// (loot_open_probe.hpp) records the exact resolved path of every map file the game opens
// (.msb.dcx/.msb/.mapbnd[.dcx]). ME3/UXM redirect BELOW CreateFileW, so the captured path IS
// the active mod's real file (loader-agnostic ground truth, whatever mounts the data) and the
// tile name comes free from the filename. Slice 1: read + DCX-decompress the loose
// .msb.dcx/.msb from those exact paths and parse with msbe::parse_msb (resident=false — the
// file layout, not the VA-relocated resident layout). .mapbnd captures are recorded for the
// later Oodle-join slice.
//
// RETIRED (kept as diagnostics only, `resident_msb` RPC): the bounded committed-private "MSB "
// magic sweep — a full sweep (~8 GB, 17 s) FROZE the game and the ~1 GB bound missed the blobs
// nondeterministically. Do NOT re-enable it on the build path. The name-lookback hunt is a
// dead end too.
//
// Output shapes are the SAME Disk* structs as load_disk_treasures, so the builders
// are shared. Coverage is INCREMENTAL: only the maps the game has streamed are captured; the
// caller merges (active-source wins per tile) and the set grows as the player explores.

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "loot_disk.hpp"  // DiskTreasure / DiskCollectible / DiskEnemy / DiskRegion / DiskObjAct

namespace goblin::worldmap
{
// One resident decompressed MSB blob + its tile identity (name + area/grid, from the
// CSMapbndResCap name key). DIAGNOSTIC-ONLY shape (the old memory-scan enumeration).
// blob = VA of the "MSB " header; len = file size derived from the PARAM section chain.
struct ResidentMsb
{
    std::string name;    // e.g. "m60_41_33_00" (validated m-pattern)
    uint8_t area = 0, gx = 0, gz = 0;  // parsed from name
    uintptr_t blob = 0;  // VA of the resident "MSB " header
    size_t len = 0;      // blob byte length (section-chain EOF)
};

// DIAGNOSTIC ONLY (`resident_msb` RPC) since 2026-08-14: sweep the process for resident
// decompressed MSBs + resolve tile names by the lookback hunt. NOT on the production path —
// the sweep froze the game at full scale and missed nondeterministically at the safe bound.
// Thread-safe against the game streaming concurrently: every read is SEH-guarded.
std::vector<ResidentMsb> scan_resident_msbs();

// Parse the ACTIVE mod's MSBs (the CreateFileW path source) into the SAME Disk* shapes
// load_disk_treasures produces (treasures returned, others appended into the non-null
// vectors — mirrors the disk loader's contract).
// `coveredTiles` (out, optional): packed tile keys (area<<16|gx<<8|gz) for every tile whose
// .msb the game has opened and we parsed — the caller drops the DISK entries on those tiles
// so the ACTIVE-mod data wins (the disk route may have read the wrong install's maps).
// Empty on no captures yet (game hasn't streamed a map — deferred, no scan) or on failure.
std::vector<DiskTreasure> load_resident_msbs(std::unordered_set<uint32_t> *coveredTiles = nullptr,
                                             std::vector<DiskCollectible> *collectibles = nullptr,
                                             std::vector<DiskEnemy> *enemies = nullptr,
                                             std::vector<DiskRegion> *regions = nullptr,
                                             std::vector<DiskObjAct> *objacts = nullptr);

// Raw diagnostic dump of the old magic-scan + the lookback name-hunt (pins the resident
// layout when the RE'd offsets don't match the exe build). RPC `resident_msb dbg`.
// Empty string on no instances.
std::string resident_msb_dbg();
} // namespace goblin::worldmap
