#pragma once

// World Editor enumeration backend (in-game editor slice 6 — the asset/item PICKER). Builds cached,
// browsable lists of (a) pickup ASSETS (AssetEnvironmentGeometryParam rows whose pickUpItemLotParamId
// points at a real loot lot) and (b) GOODS (EquipParamGoods rows), each with its resolved live name,
// so the F1 panel can offer a searchable list instead of making the user type raw ids. Live param
// chain, any mod, no bake — the same resolves the map build and loot_at use. Populated on demand
// (scan()), then read lock-free from the render/panel thread via copy_*(). See docs/HANDOFF.md.

#include <cstddef>
#include <cstdint>

namespace goblin::world_editor
{
    // POD row records copied across the (eventual) render/host boundary — fixed-size, no heap.
    struct WEAsset
    {
        int32_t aegRow;      // AssetEnvironmentGeometryParam row id (AEG{A}_{B} = A*1000+B)
        int32_t lot;         // its pickUpItemLotParamId (an ItemLotParam_map lot)
        int32_t textid;      // resolved slot-1 item textid (encoded), -1 if none
        char    name[60];    // resolved item name (UTF-8, truncated), may be empty
    };
    struct WEGoods
    {
        int32_t goodsId;     // EquipParamGoods row id
        int32_t textid;      // encoded goods textid (goodsId + 500000000)
        char    name[60];    // resolved goods name (UTF-8, truncated), never empty (unnamed skipped)
    };

    // (Re)build both caches from the live params. Present-thread / synchronous — a brief hitch on a
    // one-shot button press. Returns the total number of rows cached (assets + goods). Safe to call
    // before params load (returns 0, leaves caches empty, `scanned()` stays true so the UI shows 0).
    int scan();
    bool scanned();

    size_t asset_count();
    size_t goods_count();
    // Copy up to `max` cached records into the caller's buffer; returns the number copied.
    size_t copy_assets(WEAsset *out, size_t max);
    size_t copy_goods(WEGoods *out, size_t max);
}
