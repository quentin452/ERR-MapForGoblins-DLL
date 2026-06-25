#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace goblin::collected
{
    /// Build tile-to-row lookup tables from map data. No memory reads yet.
    void initialize();

    /// A geom-tracked marker that did NOT come from the static bake (MAP_ENTRIES) but from a
    /// runtime disk pass (e.g. the AEG collectible pass placing Rune/Ember Pieces). Registering
    /// these lets the GEOF/WGM refresh gray them exactly like baked pieces. `row_id` MUST be a
    /// unique synthetic id (see worldmap::kRuntimeGeomRowBase) and equal the displayed
    /// Marker.row_id so is_*_row_collected(row_id) resolves.
    struct RuntimeEntry
    {
        uint64_t row_id = 0;
        uint8_t area = 0, gridX = 0, gridZ = 0;
        int geom_slot = -1;        // MSB InstanceID - 9000, or -1 if N/A
        float px = 0, py = 0, pz = 0;  // MSB-local position
        std::string object_name;   // MSB part name, e.g. "AEG099_821_9003"
    };

    /// Stage runtime geom entries to be merged into the tracking tables on the next refresh().
    /// Thread-safe: the disk build runs off-thread, refresh() drains on its own thread, so the
    /// live maps are only ever mutated from the refresh thread. Entries are copied.
    void register_runtime_entries(std::vector<RuntimeEntry> entries);

    /// Remap row IDs after dynamic ID assignment in inject.
    /// old_to_new maps original MASSEDIT row_id -> dynamically assigned row_id.
    void remap_row_ids(const std::unordered_map<uint64_t, uint64_t> &old_to_new);

    /// Re-read GEOF/WGM from memory. Returns delta (newly hidden count).
    int refresh();

    bool is_row_collected(uint64_t row_id);

    /// Same as is_row_collected, but takes the ORIGINAL MAP_ENTRIES row_id and
    /// resolves the post-remap dynamic id internally. Use this from code paths
    /// that still see original IDs (e.g. the marker dump reads MAP_ENTRIES directly).
    bool is_original_row_collected(uint64_t original_row_id);

    void register_param_ptr(uint64_t row_id, void *param_data);

    /// True if this row's areaNo is owned by the collected system (so other
    /// areaNo managers, e.g. fragment-eviction, leave it alone).
    bool is_registered(uint64_t row_id);

    int collected_count();
    int skipped_count();
};
