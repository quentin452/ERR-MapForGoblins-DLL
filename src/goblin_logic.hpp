#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace goblin
{
    /// Region name for a cluster at the given map tile (area + gridX/gridZ), via
    /// the map-fragment grouping (goblin_map_tiles). "" if the tile maps to no
    /// known region. Used to label cluster icons "<Region> (<count>)".
    std::string cluster_region_label(int area, int gx, int gz);

    /// Map-fragment discovery event-flag for a tile (area + gridX/gridZ), via the same
    /// tile→fragment table the native injection rides (goblin_map_tiles). 0 = the tile
    /// needs no fragment. Used by the overlay to gate markers behind require_map_fragments.
    /// 2026-08-14 HARDENING: the table is vanilla-derived; a tile's flag is only honoured
    /// when the ACTIVE install's EMEVD actually sets it (see active_event_flag) — a mod
    /// renumbering its fragment flags degrades to "ungated" (leak) instead of hiding
    /// every region forever (or leaking everything).
    int map_fragment_flag(int area, int gx, int gz);

    /// Fallback region name from a marker's ORIGINAL areaNo (pre-overworld
    /// projection) — for clusters whose projected tile maps to no fragment region
    /// (Haligtree, the underground, Leyndell-legacy…). "" if the area is unknown.
    std::string area_region_label(int area);

    /// Feed the ACTIVE-install event-flag set: every flag the mod's EMEVDs
    /// SetEventFlag(., state=1) (collected by load_emevd_awards' setter scan).
    void note_active_event_flags(const std::vector<uint32_t> &flags);

    /// Publish the set (one-shot, after the first EMEVD scan pass completes).
    void publish_active_event_flags();

    /// True iff the ACTIVE install sets `flag` in its EMEVDs. Before the first scan
    /// completes (EMEVD pass off / build not run yet) returns true — assume the
    /// hardcoded tables are right rather than changing gate behavior.
    bool active_event_flag(uint32_t flag);
};
