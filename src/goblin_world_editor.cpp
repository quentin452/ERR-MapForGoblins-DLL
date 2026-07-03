#include "goblin_world_editor.hpp"

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "from/params.hpp"     // get_param — live param iteration
#include "goblin_inject.hpp"   // aeg_pickup_lot, resolve_loot_item_textid
#include "goblin_messages.hpp" // lookup_text_utf8

namespace
{
    // Iteration only needs each row's id (ParamRowInfo carries row_id; the T& in the iterator pair is
    // never dereferenced here), so a 1-byte row type is enough and cheapest.
    struct RowId { uint8_t _b; };

    // Sanity caps so a pathological param can't blow the cache (AEG has ~100k rows; pickups are a small
    // subset, goods a few thousand — these are ceilings, not expected sizes).
    constexpr size_t kMaxAssets = 8000;
    constexpr size_t kMaxGoods  = 8000;

    // Goods name/textid encoding: markers key goods at goodsId + 500000000 (encode_live_item cat 1),
    // and lookup_text_utf8 resolves that key to the GoodsName FMG string. Same as loot_at's chain.
    constexpr int32_t kGoodsTextBand = 500000000;

    std::mutex g_mtx;
    std::vector<goblin::world_editor::WEAsset> g_assets;
    std::vector<goblin::world_editor::WEGoods> g_goods;
    bool g_scanned = false;

    void set_name(char *dst, size_t cap, const std::string &s)
    {
        size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
        std::memcpy(dst, s.data(), n);
        dst[n] = '\0';
    }
}

namespace goblin::world_editor
{
    int scan()
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_assets.clear();
        g_goods.clear();

        // Goods: every EquipParamGoods row that resolves to a non-empty name.
        try
        {
            auto goods = from::params::get_param<RowId>(L"EquipParamGoods");
            for (auto entry : goods)
            {
                if (g_goods.size() >= kMaxGoods) break;
                const int32_t id = static_cast<int32_t>(entry.first);
                if (id <= 0) continue;
                const int32_t tid = id + kGoodsTextBand;
                std::string nm = goblin::lookup_text_utf8(tid);
                if (nm.empty()) continue;  // unnamed / placeholder goods — not useful in a picker
                WEGoods e{};
                e.goodsId = id;
                e.textid = tid;
                set_name(e.name, sizeof(e.name), nm);
                g_goods.push_back(e);
            }
        }
        catch (...) {}

        // Assets: every AssetEnvironmentGeometryParam row whose pickUpItemLotParamId is a real lot.
        try
        {
            auto aeg = from::params::get_param<RowId>(L"AssetEnvironmentGeometryParam");
            for (auto entry : aeg)
            {
                if (g_assets.size() >= kMaxAssets) break;
                const uint32_t id = static_cast<uint32_t>(entry.first);
                const int32_t lot = static_cast<int32_t>(goblin::aeg_pickup_lot(id));
                if (lot <= 0) continue;  // non-pickup assets read -1 (0xFFFFFFFF) or 0
                const int32_t tid = goblin::resolve_loot_item_textid(static_cast<uint32_t>(lot), 1, -1);
                std::string nm = (tid >= 0) ? goblin::lookup_text_utf8(tid) : std::string{};
                WEAsset e{};
                e.aegRow = static_cast<int32_t>(id);
                e.lot = lot;
                e.textid = tid;
                set_name(e.name, sizeof(e.name), nm);  // may be empty (some lots have no FMG name)
                g_assets.push_back(e);
            }
        }
        catch (...) {}

        g_scanned = true;
        spdlog::info("[WORLDEDIT] scan: {} pickup assets, {} named goods", g_assets.size(),
                     g_goods.size());
        return static_cast<int>(g_assets.size() + g_goods.size());
    }

    bool scanned()
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        return g_scanned;
    }

    size_t asset_count()
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        return g_assets.size();
    }

    size_t goods_count()
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        return g_goods.size();
    }

    size_t copy_assets(WEAsset *out, size_t max)
    {
        if (!out || max == 0) return 0;
        std::lock_guard<std::mutex> lk(g_mtx);
        size_t n = g_assets.size() < max ? g_assets.size() : max;
        std::memcpy(out, g_assets.data(), n * sizeof(WEAsset));
        return n;
    }

    size_t copy_goods(WEGoods *out, size_t max)
    {
        if (!out || max == 0) return 0;
        std::lock_guard<std::mutex> lk(g_mtx);
        size_t n = g_goods.size() < max ? g_goods.size() : max;
        std::memcpy(out, g_goods.data(), n * sizeof(WEGoods));
        return n;
    }
}
