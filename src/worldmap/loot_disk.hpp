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
// _00 MSB in it, returning all POSITIONED treasures (partIndex >= 0) on a real
// Asset part. DummyAsset (cut/inert) placements are dropped; their lotIds are
// appended to droppedDummyLots (when non-null) so the caller can flag the
// "reachable_dummy" subset (those the bake still backs → recover when the bake
// is removed). Logs [LOOTDISK] per-map (debug) + totals. Empty when no dir.
std::vector<DiskTreasure> load_disk_treasures(std::vector<uint32_t> *droppedDummyLots = nullptr);

// ── Map-dir discovery state (F1 error + CreateFileW fallback) ──────────────────
// With loot_from_disk_msb on, the map dir is resolved by ancestor-walk at init
// (Found). If that finds nothing we fall back to OBSERVING the game's own map
// opens via the CreateFileW hook (Searching): the first *.msb.dcx open reveals
// the real dir, loader-agnostic (the [MAPOPEN] essai proved ME3 opens each map
// by CreateFileW with the resolved mod path). If no map opens within an in-game
// timeout the state goes Failed → the overlay shows a red error and builds no
// markers (the disk source is REQUIRED when the feature is on).
enum class DiskLootState { Disabled, Found, Searching, Failed };

// Resolve the map dir once (ancestor-walk). Found → cache + state Found; empty →
// state Searching (the CreateFileW observer completes it). Idempotent / cheap.
void ensure_map_dir_resolved();

// Current state. Lazily flips Searching→Failed once the in-game timeout elapses
// (no per-frame tick needed — evaluated on access).
DiskLootState disk_loot_state();

// The resolved (or last-searched) MapStudio dir, for the build + the error text.
std::filesystem::path disk_loot_dir();

// Called by the CreateFileW observer for every *.msb.dcx the game opens. While
// the dir is not yet Found, captures its map\MapStudio parent → flips to Found.
void on_map_opened_path(const wchar_t *full_path);

// Registered by the marker layer at init: invoked once, the instant the map dir
// flips to Found via CreateFileW discovery, so the worker build kicks immediately
// instead of waiting for the next overlay tick (~7s). Set before any map opens.
void set_build_trigger(void (*fn)());
} // namespace goblin::worldmap
