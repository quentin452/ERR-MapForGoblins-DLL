#pragma once

#include <string>

namespace goblin
{
    /// Apply map fragment visibility logic to WorldMapPointParam entries.
    void apply_map_logic();

    /// Region name for a cluster at the given map tile (area + gridX/gridZ), via
    /// the map-fragment grouping (goblin_map_tiles). "" if the tile maps to no
    /// known region. Used to label cluster icons "<Region> (<count>)".
    std::string cluster_region_label(int area, int gx, int gz);
};
