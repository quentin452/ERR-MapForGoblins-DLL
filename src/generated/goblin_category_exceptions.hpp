#pragma once

#include <cstddef>
#include <cstdint>

#include "goblin_map_data.hpp"  // Category enum

namespace goblin::generated
{
    // Curated item -> Category exceptions: the small set of goods the mod buckets
    // by a deliberate id-list / name rule that ER's own taxonomy
    // (EquipParamGoods.goodsType + sortGroupId) CANNOT express — the Low/normal/Rare
    // splits (runes, smithing), Gloveworts vs Great Gloveworts, the gType1-sg50
    // grab-bag (Celestial Dew / Imbued Sword / Stonesword / Deathroot / Seedbed),
    // the full Quest-Progression + Prayerbook id-lists (so cross-cell members
    // classify right), and the ERR-added Reforged families.
    //
    // This is the ONLY per-item category surface left after Phase-3: the bulk is
    // classified live from (goodsType, sortGroupId) (see item_marker_category /
    // category_from_taxonomy in goblin_inject.cpp), drift-free for any mod/DLC.
    // Validated to reproduce the old per-item ITEM_ICONS category column exactly
    // (tools/_validate_taxonomy_map.py — 0 mismatches). Built offline by
    // generate_category_exceptions_cpp from item_icon_table.json (the curated
    // LOOT_CATEGORIES classifier). Sorted ascending by id (binary search).
    struct CategoryException
    {
        int32_t  id;        // raw goods id (NOT offset-encoded)
        Category category;
    };

    extern const size_t CATEGORY_EXCEPTION_COUNT;
    extern const CategoryException CATEGORY_EXCEPTIONS[];
}
