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

    const auto &graces = goblin::live_graces(); // LIVE WorldMapPointParam, no bake
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
        // Dungeon grace = a CAVE (raw areaNo 31 = m31 "Grottes"). ERR uses its dungeon-style grace
        // icon (MENU_MAP_ERR_GraceUnderground) ONLY for cave graces — NOT catacombs (m30), tunnels
        // (m32), legacy dungeons (m10–12) or DLC. The OLD gate ((area != 60/61) && grp != 1/3) flagged
        // ALL of those, so far too many graces drew the ERR icon. areaNo here is the RAW WorldMapPoint
        // area (pre-projection), so it still reads 31 for cave graces. If ERR's real set turns out to
        // include other dungeon types, widen this — verify in-game via the [GRACE-AREA] log below.
        m.dungeon = (e.areaNo == 31);
        cache_.push_back(m);
    }
    // Verification (dev, gated by dump_icon_textures): one-shot histogram of the raw areaNos that
    // carry graces + which we flag as CAVE → ERR underground icon. Compare against what ERR actually
    // draws to confirm the m31-only gate (or learn the correct set).
    if (goblin::config::dumpIconTextures)
    {
        std::map<int, int> by_area;
        for (const Marker &mm : cache_) by_area[mm.raw_area]++;
        std::string s;
        for (const auto &kv : by_area)
            s += " m" + std::to_string(kv.first) + "=" + std::to_string(kv.second) +
                 (kv.first == 31 ? "(CAVE->ERR)" : "");
        spdlog::info("[GRACE-AREA] grace areaNo histogram (CAVE m31 = ERR underground icon):{}", s);
    }
    return cache_;
}
} // namespace goblin::worldmap
