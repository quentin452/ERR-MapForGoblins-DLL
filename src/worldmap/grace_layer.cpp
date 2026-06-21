#include "grace_layer.hpp"

#include "category_meta.hpp" // category_color
#include "goblin_inject.hpp" // goblin::live_graces / marker_world_pos / ui::category_visible
#include "goblin_map_data.hpp" // Category::WorldGraces
#include "goblin_logic.hpp"    // map_fragment_flag

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
        int pg = ga & 63;
        // DLC vs base by FINAL page (61/40-43 = DLC). UNDERGROUND by the ORIGINAL areaNo
        // (12 / 40-43): area 12 projects to overworld map-space (pg=60) so it must be gated
        // by its source layer, not the final page.
        bool isug = (e.areaNo == 12) || (e.areaNo >= 40 && e.areaNo <= 43);
        bool isdlc = (pg == 61) || (e.areaNo >= 40 && e.areaNo <= 43);
        int grp = (isdlc ? 2 : 0) | (isug ? 1 : 0);
        const int gc = static_cast<int>(goblin::generated::Category::WorldGraces);
        int pname = -1;
        int ckey = goblin::marker_cluster_key(e.areaNo, e.gridXNo, e.gridZNo, e.posX, e.posZ,
                                              &pname);
        int frag = goblin::marker_fragment_flag(e.areaNo, e.gridXNo, e.gridZNo, e.posX, e.posZ);
        cache_.push_back(Marker{wx, wz, grp, (int)e.areaNo, gc, ckey, pname, e.textId,
                                category_color(gc), "show_graces", frag});
    }
    return cache_;
}
} // namespace goblin::worldmap
