#include "grace_layer.hpp"

#include "category_meta.hpp" // category_color
#include "goblin_inject.hpp" // goblin::live_graces / marker_world_pos / ui::category_visible
#include "goblin_map_data.hpp" // Category::WorldGraces
#include "goblin_logic.hpp"    // map_fragment_flag
#include "goblin_bench.hpp"    // GOBLIN_BENCH scoped timers
#include "goblin_config.hpp"   // config::dumpIconTextures (verification log gate)

#include <spdlog/spdlog.h>
#include <map>
#include <string>

namespace goblin::worldmap
{
bool GraceLayer::visible() const
{
    const int g = static_cast<int>(goblin::generated::Category::WorldGraces);
    return goblin::ui::category_visible(g) &&
           goblin::ui::section_visible(goblin::ui::category_section(g));
}

const std::vector<Marker> &GraceLayer::markers() const
{
    if (built_)
        return cache_;
    built_ = true;
    GOBLIN_BENCH("build.graces");

    const auto &graces = goblin::live_graces(); // LIVE BonfireWarpParam, no bake
    cache_.reserve(graces.size());
    for (const goblin::LiveGrace &e : graces)
    {
        // Project the row to UNIFIED overworld coords (legacy dungeons like
        // Stormveil/area-10 are page-local until projected → else they pile up).
        int ga;
        float wx, wz;
        goblin::marker_world_pos(e.areaNo, e.gridXNo, e.gridZNo, e.posX, e.posZ, ga, wx, wz,
                                 /*conv_underground=*/true);
        int grp = goblin::marker_group_from(e.areaNo, ga);
        const int gc = static_cast<int>(goblin::generated::Category::WorldGraces);
        int pname = -1;
        int ckey = goblin::marker_cluster_key(e.areaNo, e.gridXNo, e.gridZNo, e.posX, e.posZ,
                                              &pname);
        int frag = goblin::marker_fragment_flag(e.areaNo, e.gridXNo, e.gridZNo, e.posX, e.posZ);
        // discover_flag = the grace's textDisableFlagId1: when set (discovered), the
        // renderer drops this marker (the game draws that grace natively) — keeps only
        // UNdiscovered graces as overlay helpers. row_id/cleared/collected stay 0
        // (graces aren't collectible; discovery is handled by discover_flag).
        Marker m{wx, wz, grp, (int)e.areaNo, gc, ckey, pname, e.textId,
                 category_color(gc), "show_graces", frag,
                 0ull, 0, 0, e.discoverFlag};
        m.raw_area = e.areaNo; m.raw_gx = e.gridXNo; m.raw_gz = e.gridZNo;
        m.raw_px = e.posX; m.raw_pz = e.posZ;
        m.worldY = e.posY;  // block-local grace altitude → reference for off-page markers' altitude badge
        // Underground/cave grace = ERR's own per-grace gate, read LIVE from BonfireWarpParam.iconId
        // (1 = normal bonfire, 44 = ERR cave/underground grace → MENU_MAP_ERR_GraceUnderground),
        // captured into LiveGrace.underground. This is ERR's authored set (93 graces: all catacombs
        // m30 + caves m31 + most tunnels + assorted dungeon graces + 16 below-surface overworld
        // graces; m12/legacy stay normal = own map layer). Replaces the old areaNo==31 proxy (which
        // missed catacombs, tunnels, the overworld set, etc).
        m.dungeon = e.underground;
        m.source = Source::Live;   // graces come LIVE from BonfireWarpParam (no bake)
        cache_.push_back(m);
    }
    // Verification (dev, gated by dump_icon_textures): one-shot histogram of grace raw areaNos +
    // how many we flag underground (ERR icon), to sanity-check the live iconId==44 gate in-game.
    if (goblin::config::dumpIconTextures)
    {
        std::map<int, int> by_area, by_grp;
        int ug = 0;
        for (const Marker &mm : cache_) { by_area[mm.raw_area]++; by_grp[mm.group]++; if (mm.dungeon) ++ug; }
        std::string s, gs;
        for (const auto &kv : by_area)
            s += " m" + std::to_string(kv.first) + "=" + std::to_string(kv.second);
        for (const auto &kv : by_grp)
            gs += " g" + std::to_string(kv.first) + "=" + std::to_string(kv.second);
        spdlog::info("[GRACE-AREA] {} graces, {} underground (ERR icon, iconId==44); group hist:{}; areaNo hist:{}",
                     cache_.size(), ug, gs, s);
    }
    return cache_;
}
} // namespace goblin::worldmap
