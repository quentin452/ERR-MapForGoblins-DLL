#pragma once

#include <cstddef>
#include <cstdint>

#include "goblin_map_data.hpp"  // Category enum

namespace goblin::generated
{

// How the runtime derives a World feature's graying flag (marker textDisableFlagId1), so an
// activated/looted instance hides like the bake did — resolved LIVE from the mod's files, no bake:
//   None          no flag (respawn points, never "complete").
//   ImpSeal       flag = tile_base(area,gx,gz) + (entityId % 1000); the pass also rejects
//                 placements whose entity suffix isn't a real seal {570,575,565,611} and picks
//                 the key label by suffix (565 = Imbued Sword Key, else Stonesword Key).
//   HeroTombEmevd flag = the EMEVD template-90005683 activated flag, joined to the asset by EntityID.
//   SealEmevd     flag = the EMEVD template-90006051 activation flag, joined by EntityID; the pass
//                 SELF-GATES (skips placements with no 90006051 binding = decoration / non-seal use).
enum class FlagRule : uint8_t { None, ImpSeal, HeroTombEmevd, SealEmevd };

// One asset-model World feature: an AEG ModelName that maps to a marker category.
// The runtime's generic disk pass (build_disk_world_feature_markers) emits a marker
// for every placed MSB asset whose aegRow matches a row here. Generated from
// tools/world_feature_assets.py — add a feature there, not in C++.
struct WorldFeatureModel
{
    uint32_t aeg_row;          // AssetEnvironmentGeometryParam row id (AEG{A}_{B} -> A*1000+B)
    Category category;         // marker bucket
    int32_t  text_id;          // WorldMapPointParam.textId1 for the tooltip label
    bool     entity_required;  // true = emit only placements carrying an MSB EntityID (interactive)
    bool     category_wipe;    // true = drop ALL baked of this category (dedicated); false = cell-dedup (shared)
    FlagRule flag_rule;        // how the pass resolves the graying flag (+ Imp suffix filter/label)
};

extern const WorldFeatureModel WORLD_FEATURE_MODELS[];  // sorted by aeg_row
extern const size_t WORLD_FEATURE_MODEL_COUNT;

} // namespace goblin::generated
