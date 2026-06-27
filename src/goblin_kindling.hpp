#pragma once

#include <cstdint>
#include <unordered_map>

namespace goblin::kindling
{
    /// Build the slot table from the fixed ERR constant set (5 spirits in
    /// m60_45_37_00, entity 1045373501..505). No MAP_ENTRIES dependency (Phase 2
    /// no-bake): row_id == entity_id, the same key the disk-emitted marker uses
    /// (see region_row_id). Safe to call before remap_row_ids.
    void initialize();

    /// Graying row-id for a disk SFX region named "KindlingSpirit_000N" (N=1..5).
    /// Returns 0 if the name isn't a kindling spirit region. The marker emitted at
    /// this region MUST use this as its row_id so is_row_collected() (keyed by the
    /// constant slot table) grays it. Single source of truth shared with the slot table.
    uint64_t region_row_id(const char *region_name);

    /// Remap row IDs after dynamic ID assignment in inject.
    void remap_row_ids(const std::unordered_map<uint64_t, uint64_t> &old_to_new);

    /// Re-read SFX-region state from RAM. Returns delta vs previous tick
    /// (positive = newly hidden, negative = newly revealed).
    /// Idempotent if state hasn't changed.
    int refresh();

    /// True if the row is currently considered collected (hidden).
    bool is_row_collected(uint64_t row_id);

    /// Register the param row pointer so refresh() can flip areaNo in-place.
    /// Same trick as goblin::collected — write 0x20 = 99 to hide, restore original.
    void register_param_ptr(uint64_t row_id, void *param_data);

    /// True if this row's areaNo is owned by the kindling system.
    bool is_registered(uint64_t row_id);

    int collected_count();
};
