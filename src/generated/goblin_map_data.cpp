// PHASE-2 NO-BAKE STUB — the static marker bake is RETIRED.
//
// This file used to hold ~8653 baked WorldMapPointParam rows (the static map bake). Phase 2
// of the no-bake migration removed it: every map marker now comes from live mod files / game
// memory — the disk loot/collectible/enemy/emevd passes, live bosses, live graces — and the
// three behaviours that used to read this table were each migrated off it first:
//   • geom-graying seed (goblin_collected)  → goblin::collected::register_runtime_entries
//                                              (Rune/Ember pieces + gather nodes, runtime-registered)
//   • kindling slot table (goblin_kindling) → fixed ERR constants (slots 1..5, entity 1045373501..505)
//   • FMG marker labels  (goblin_messages)  → whole-namespace preload (copy_fmg_all_layered)
//
// The Category enum + MapEntry / LootSource types stay in goblin_map_data.hpp — they still drive
// the overlay layers/buckets. An EMPTY table keeps every remaining consumer loop (census, diags,
// the build pass) a zero-iteration no-op. To restore the bake, `git revert` this commit or
// re-run tools/generate_data.py (which must be updated to match before any regen).

#include "goblin_map_data.hpp"

namespace goblin::generated
{

const size_t MAP_ENTRY_COUNT = 0;

// A zero-length array is ill-formed in standard C++; this single zero-initialized dummy is NEVER
// read — every consumer iterates `i < MAP_ENTRY_COUNT` (== 0) so the element is unreachable.
const MapEntry MAP_ENTRIES[1] = {};

} // namespace goblin::generated
