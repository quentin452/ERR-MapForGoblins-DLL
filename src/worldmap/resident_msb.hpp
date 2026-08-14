#pragma once
// Resident-MSB loot source — derives treasure/collectible/enemy/region/ObjAct placements from
// the RESIDENT DECOMPRESSED MSBs the game keeps in memory for the currently-streamed maps.
// RE (2026-06-24, live-proven): docs/re/windows_runtime_msb_resident_re_findings.md +
// docs/re/windows_resident_msbe_layout_re_findings.md + docs/memory/tooling/runtime-msb-resident-plan.md.
//
// WHY THIS EXISTS (user 2026-08-14, Golden Age via ME3): the disk route resolves the map dir by
// walking the DLL's ancestors for <p>/mod/map/MapStudio — but GA mounts its data at <root>/GA/
// (ME3 package path), so the walk missed it and fell back to the VANILLA install's maps. The
// RESIDENT route can't miss: the game keeps the decompressed MSBs of whatever it ACTUALLY loaded
// (loader-agnostic by construction — ME3/ME2/UXM all end at the same in-memory blobs).
//
// Enumeration: bounded committed-private scan for "MSB " magic (proven: 25 blobs live), paired
// with the CSMapbndResCap instances (vtable er+0x2ba5658) whose raw-bundle pointer matches the
// blob — the ResCap name key ("m60_xx_yy_zz") gives the tile. Parser: msbe::parse_msb with
// resident=true (entry-internal offsets are ABSOLUTE VAs in the resident copy; blobBase = the
// blob's VA). Output shapes are the SAME Disk* structs as load_disk_treasures, so the builders
// are shared.
//
// Coverage is INCREMENTAL: only the ~25 streamed maps are resident at once; the caller merges
// (resident wins per tile) and the set grows as the player explores.

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "loot_disk.hpp"  // DiskTreasure / DiskCollectible / DiskEnemy / DiskRegion / DiskObjAct

namespace goblin::worldmap
{
// One resident decompressed MSB blob + its tile identity (name + area/grid, from the
// CSMapbndResCap name key). blob = VA of the "MSB " header; len = file size derived from
// the PARAM section chain (the last section's nextParamOffset = end of file).
struct ResidentMsb
{
    std::string name;    // e.g. "m60_41_33_00" (validated m-pattern)
    uint8_t area = 0, gx = 0, gz = 0;  // parsed from name
    uintptr_t blob = 0;  // VA of the resident "MSB " header
    size_t len = 0;      // blob byte length (section-chain EOF)
};

// Scan the process for resident decompressed MSBs and resolve each one's tile name by pairing
// the blob with the CSMapbndResCap instance whose raw bundle (+0xC0) points at it. Best-effort:
// a blob with no matching/valid name is DROPPED (logged) — a name is required for the world
// transform, and a wrong tile would misplace every marker on it, so validation is strict.
// Thread-safe against the game streaming concurrently: every read is SEH-guarded.
std::vector<ResidentMsb> scan_resident_msbs();

// Parse the resident MSBs into the SAME Disk* shapes load_disk_treasures produces (treasures
// returned, others appended into the non-null vectors — mirrors the disk loader's contract).
// `coveredTiles` (out, optional): packed tile keys (area<<16|gx<<8|gz) for every blob that
// parsed ok — the caller drops the DISK entries on those tiles so the RESIDENT (active-mod)
// data wins (the disk route may have read the wrong install's maps). Empty on any failure.
std::vector<DiskTreasure> load_resident_msbs(std::unordered_set<uint32_t> *coveredTiles = nullptr,
                                             std::vector<DiskCollectible> *collectibles = nullptr,
                                             std::vector<DiskEnemy> *enemies = nullptr,
                                             std::vector<DiskRegion> *regions = nullptr,
                                             std::vector<DiskObjAct> *objacts = nullptr);

// Raw diagnostic dump of the CSMapbndResCap instances + their name/bundle fields (hex + UTF-16
// attempts) — pins the live layout when the name/bundle offsets don't match the RE'd build.
// RPC `resident_msb dbg`. Empty string on no instances.
std::string resident_msb_dbg();
} // namespace goblin::worldmap
