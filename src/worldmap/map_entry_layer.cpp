#include "map_entry_layer.hpp"

#include "category_meta.hpp"
#include "goblin_map_data.hpp" // MAP_ENTRIES / MAP_ENTRY_COUNT / MapEntry / Category
#include "goblin_inject.hpp"   // marker_world_pos / goblin::ui::category_visible

#include <array>

namespace goblin::worldmap
{
namespace
{
constexpr int NUM_CAT = static_cast<int>(goblin::generated::Category::WorldInteractables) + 1;

std::array<std::vector<Marker>, NUM_CAT> g_buckets;
bool g_built = false;

// Build every category's marker cache in ONE pass over MAP_ENTRIES (9k rows). Same
// world-projection + group classification as the grace layer.
void build_buckets()
{
    if (g_built)
        return;
    g_built = true;
    namespace gen = goblin::generated;
    for (size_t i = 0; i < gen::MAP_ENTRY_COUNT; ++i)
    {
        const gen::MapEntry &e = gen::MAP_ENTRIES[i];
        int c = static_cast<int>(e.category);
        if (c < 0 || c >= NUM_CAT)
            continue;
        const auto &d = e.data;
        int ga;
        float wx, wz;
        goblin::marker_world_pos(d.areaNo, d.gridXNo, d.gridZNo, d.posX, d.posZ, ga, wx, wz,
                                 /*conv_underground=*/true);
        int pg = ga & 63;
        bool isug = (d.areaNo == 12) || (d.areaNo >= 40 && d.areaNo <= 43);
        bool isdlc = (pg == 61) || (d.areaNo >= 40 && d.areaNo <= 43);
        int grp = (isdlc ? 2 : 0) | (isug ? 1 : 0);
        int pname = -1;
        int ckey = goblin::marker_cluster_key(d.areaNo, d.gridXNo, d.gridZNo, d.posX, d.posZ,
                                              &pname);
        g_buckets[c].push_back(Marker{wx, wz, grp, (int)d.areaNo, c, ckey, pname,
                                      category_color(c), category_icon_key(c)});
    }
}
} // namespace

MapEntryLayer::MapEntryLayer(int category) : cat_(category)
{
    const char *k = category_icon_key(category);
    name_ = k ? k : "marker";
}

bool MapEntryLayer::visible() const
{
    return goblin::ui::category_visible(cat_) &&
           goblin::ui::section_visible(goblin::ui::category_section(cat_));
}

const std::vector<Marker> &MapEntryLayer::markers() const
{
    build_buckets();
    return g_buckets[cat_];
}
} // namespace goblin::worldmap
