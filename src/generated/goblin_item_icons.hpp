#pragma once

#include <cstddef>
#include <cstdint>

#include "goblin_map_data.hpp"  // Category enum

namespace goblin::generated
{
    // Live-loot ICON lookup. Maps an offset-encoded item key (the same encoding
    // used for marker textIds: goods+500M, weapon+100M / ammo as-is, protector+200M,
    // accessory+300M, gem+400M) to the iconId that item would get as a normal marker.
    // Built offline by generate_item_icons_cpp from the LOOT_CATEGORIES classifier.
    // The DLL reads the live ItemLotParam item, looks it up here, and re-icons the
    // marker so randomized loot shows the right icon.
    //
    // NOTE (Phase-3): this table no longer carries the Category. Categorisation is
    // now LIVE from ER's own taxonomy (EquipParamGoods.goodsType + sortGroupId, see
    // item_marker_category in goblin_inject.cpp) + the small curated
    // goblin_category_exceptions table — drift-free for any mod/DLC. The icon stays
    // per-item (genuinely irreducible) so it lives on here.
    struct ItemIcon
    {
        int32_t  key;       // offset-encoded item id
        uint16_t iconId;
    };

    extern const size_t ITEM_ICON_COUNT;
    extern const ItemIcon ITEM_ICONS[];  // sorted ascending by key (binary search)

    // Spoiler-free "?" map-icon frame id for this profile (anonymous_loot mode).
    // 440 on a vanilla-base gfx, shifted by the icon-frame offset on bases that
    // add their own frames (Convergence). Generated per profile.
    extern const uint16_t ANON_ICON_ID;

    // Cluster map-icon frame id (the "stack of dots" glyph for collapsed marker
    // piles). Always ANON_ICON_ID + 1 — build_vanilla_gfx appends it one frame
    // past the "?" — so it inherits the same per-profile offset. Generated per
    // profile beside ANON_ICON_ID.
    extern const uint16_t CLUSTER_ICON_ID;

    // Cluster-depleted map-icon frame id (the GREEN "stack of dots"). The DLL swaps
    // a cluster's iconId to this once all its members are collected. ANON_ICON_ID + 3
    // (one past the quest-NPC frame) — same per-profile offset. Generated per profile.
    extern const uint16_t CLUSTER_DONE_ICON_ID;
}
