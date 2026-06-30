// Spatial grid over map-space — buckets markers into ER's 256-unit map tiles so the render hot loop
// scales with what's ON SCREEN (a few cells) instead of the full marker set, and so clustering becomes
// "one pile per occupied tile" (deterministic, zoom-aware, grace-independent) instead of the old
// nearest-grace heuristic.
//
// Cells are keyed in MAP-SPACE (the frame world_to_mapspace produces), bucketed by TILE = 256 units,
// with the marker GROUP (base/DLC × over/under) folded into the key so different map layers never share
// a cell. The grid stores marker INDICES into the layer's marker cache (rebuilt whenever that cache is).
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace goblin::worldmap
{
    // One ER map tile in map-space units. Markers within the same tile + group cluster together.
    inline constexpr float kTileSize = 256.0f;

    // Cell key: group (4 bits) | tileU (14 bits) | tileV (14 bits). Tile indices are biased by +8192 so
    // negative map-space coords stay non-negative; 14 bits covers ±8192 tiles = ±2.1M units, far beyond
    // ER's map-space extent.
    inline uint32_t grid_cell_key(int group, float mU, float mV)
    {
        const int tu = (int)(mU / kTileSize) + 8192;
        const int tv = (int)(mV / kTileSize) + 8192;
        return ((uint32_t)(group & 0xF) << 28) | ((uint32_t)(tu & 0x3FFF) << 14) |
               (uint32_t)(tv & 0x3FFF);
    }

    struct SpatialGrid
    {
        // cellKey -> indices of markers whose map-space tile is this cell.
        std::unordered_map<uint32_t, std::vector<int>> cells;

        void clear() { cells.clear(); }
        bool empty() const { return cells.empty(); }
        size_t cell_count() const { return cells.size(); }

        void insert(int group, float mU, float mV, int markerIndex)
        {
            cells[grid_cell_key(group, mU, mV)].push_back(markerIndex);
        }

        // Visit every cell whose tile overlaps the map-space rect [u0,u1]×[v0,v1] for the given group.
        // The rect is the on-screen viewport unprojected to map-space (+ a tile of margin), so the hot
        // loop only touches markers that can actually be visible. fn receives (cellKey, indices).
        template <typename Fn>
        void for_cells_in_rect(int group, float u0, float v0, float u1, float v1, Fn &&fn) const
        {
            if (u1 < u0) { float t = u0; u0 = u1; u1 = t; }
            if (v1 < v0) { float t = v0; v0 = v1; v1 = t; }
            const int tu0 = (int)(u0 / kTileSize) + 8192, tu1 = (int)(u1 / kTileSize) + 8192;
            const int tv0 = (int)(v0 / kTileSize) + 8192, tv1 = (int)(v1 / kTileSize) + 8192;
            for (int tu = tu0; tu <= tu1; ++tu)
                for (int tv = tv0; tv <= tv1; ++tv)
                {
                    const uint32_t key = ((uint32_t)(group & 0xF) << 28) |
                                         ((uint32_t)(tu & 0x3FFF) << 14) | (uint32_t)(tv & 0x3FFF);
                    auto it = cells.find(key);
                    if (it != cells.end())
                        fn(key, it->second);
                }
        }
    };
}
