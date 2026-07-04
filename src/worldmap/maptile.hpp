#pragma once
// ELDEN RING world-map tile loader (endgame phase-1a).
//
// The game's map ART lives in a BHF4 split archive `menu/71_MapTile.tpfbhd` (header) + `.tpfbdt` (data).
// Each entry is a per-tile `DCX`-compressed `TPF` holding one DDS. This module reads that archive off the
// ACTIVE install (loose mod overlay OR packed dvdbnd, via read_game_file_decompressed — mod-agnostic, no
// bake) and extracts tiles on demand. The BHF4 format was cracked + validated offline (tools/tpfbhd_recon,
// docs/plans/map_tile_loading_plan.md). GPU upload + canvas draw (slice 2) build on this.
//
// BHF4 header: "BHF4" | fileCount u32@0x0C | entriesStart u64@0x10 (=0x40) | version@0x18 |
//              entryStride u64@0x20 (=0x24)
// Entry (36B): rawFlags u32 | -1 u32 | compressedSize u64@0x08 | uncompressedSize u64@0x10 |
//              dataOffset u32@0x18 | fileId u32@0x1c | nameOffset u32@0x20 (UTF-16LE null-term in .tpfbhd)

#include <cstdint>
#include <string>
#include <vector>

namespace goblin::worldmap::maptile
{
struct Entry
{
    std::string name;            // e.g. "71_MapTile\\MENU_MapTile_<...>.tpf.dcx"
    uint64_t    compressedSize;  // bytes of the DCX blob in the .tpfbdt
    uint32_t    dataOffset;      // offset of the DCX blob within the .tpfbdt
    uint32_t    fileId;          // sequential id
};

// Parse a BHF4 header blob into entries. false on bad magic / out-of-range table.
bool parse_bhf4(const std::vector<uint8_t> &bhd, std::vector<Entry> &out);

// The col/row extent of one dimension+LOD, parsed from the tile names ("...M{MM}_L{L}_{col}_{row}_..."),
// so the tile grid can be laid over the map-space art extent (slice 3 placement). `prefix` e.g. "M00_L0".
// col/row are parsed as HEX (names go 00,01,…,09,0a,…). Returns count matched; false if none.
struct GridRange { int minCol = 0, maxCol = 0, minRow = 0, maxRow = 0, count = 0; };
bool grid_range(const std::vector<Entry> &entries, const std::string &prefix, GridRange &out);

// Read + parse the tile archive off the active install. rel_base = "menu/71_MapTile" (no extension);
// reads rel_base+".tpfbhd" and rel_base+".tpfbdt". Keeps the (small, ~MB) .tpfbdt in `bdt` for on-demand
// tile extraction. false if either file is missing or the header is not BHF4.
bool load_archive(const std::string &rel_base, std::vector<Entry> &entries, std::vector<uint8_t> &bdt);

// Extract one entry's first DDS: seek .tpfbdt -> DCX decompress -> TPF -> first named texture -> DDS bytes.
// Returns the DDS bytes (empty on failure); fills texName and (if non-null) width/height from the DDS header.
std::vector<uint8_t> extract_dds(const std::vector<uint8_t> &bdt, const Entry &e, std::string &texName,
                                 uint32_t *w = nullptr, uint32_t *h = nullptr);

// Convenience: load the archive, find the FIRST entry whose name contains `needle`
// (e.g. "M00_L0_00_00_00000000"), and extract its DDS. Fills texName + (if non-null) w/h.
// Frees the (large) .tpfbdt before returning. Heavy — reads the whole archive per call — so this is for a
// single-tile slice-2 probe; slice 3 will cache the parse / read only the wanted byte range.
std::vector<uint8_t> extract_named(const std::string &rel_base, const std::string &needle,
                                   std::string &texName, uint32_t *w = nullptr, uint32_t *h = nullptr);

// Dev recon: load the archive in-game, log the entry count + first entries' names/dims, and return a
// one-line RPC summary. Learns the tile naming/count/dims for the packed 71_MapTile (sub-slice 1b).
std::string probe(const std::string &rel_base, int max_probe = 8, const char *name_filter = nullptr);
} // namespace goblin::worldmap::maptile
