#pragma once
// Disk-MSB loot source — derives treasure loot placements from the ACTIVE mod's
// real map/MapStudio/*.msb.dcx files (no committed bake). Gated by config
// loot_from_disk_msb. The parse chain (msbe_parser) + the position transform are
// RE'd + disk-validated to 99.3% exact-match vs the bake:
//   docs/re/windows_resident_msbe_layout_re_findings.md (the chain)
//   docs/re/windows_msbe_position_transform_validation.md (world = grid*256 + pos)
// The block-local Part+0x20 position here is exactly the bake's x/z, so the
// runtime reuses the SAME marker_world_pos transform downstream (no new RE).
#include <cstdint>
#include <filesystem>
#include <vector>

namespace goblin::worldmap
{
// One positioned treasure read from a disk MSB: an itemLotId at a block-local
// position on a named tile. A single lotId may appear MORE THAN ONCE (one per
// MSB Treasure event / part) — each is its own entry (emit each).
struct DiskTreasure
{
    uint32_t lotId = 0;
    uint8_t  area = 0, gx = 0, gz = 0;  // from the m{AA}_{BB}_{CC}_00 filename
    float    posX = 0.0f, posZ = 0.0f;  // Part+0x20 X/Z (block-local; = bake x/z)
};

// Record the DLL's own mod folder (parent of MapForGoblins.dll) for map-dir
// auto-detect. Call once at init, before load_disk_treasures().
void set_mod_folder(const std::filesystem::path &p);

// Resolve the map dir (config loot_msb_dir, else auto-detect) and parse every
// _00 MSB in it, returning all POSITIONED treasures (partIndex >= 0). Logs
// [LOOTDISK] per-map (debug) + totals. Empty when no dir resolves or none parse.
std::vector<DiskTreasure> load_disk_treasures();
} // namespace goblin::worldmap
