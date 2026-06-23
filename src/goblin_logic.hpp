#pragma once

#include <string>

namespace goblin
{
    /// Region name for a cluster at the given map tile (area + gridX/gridZ), via
    /// the map-fragment grouping (goblin_map_tiles). "" if the tile maps to no
    /// known region. Used to label cluster icons "<Region> (<count>)".
    std::string cluster_region_label(int area, int gx, int gz);

    /// Map-fragment discovery event-flag for a tile (area + gridX/gridZ), via the same
    /// tile→fragment table the native injection rides (goblin_map_tiles). 0 = the tile
    /// needs no fragment. Used by the overlay to gate markers behind require_map_fragments.
    int map_fragment_flag(int area, int gx, int gz);

    /// Fallback region name from a marker's ORIGINAL areaNo (pre-overworld
    /// projection) — for clusters whose projected tile maps to no fragment region
    /// (Haligtree, the underground, Leyndell-legacy…). "" if the area is unknown.
    std::string area_region_label(int area);
};
