#include "grace_layer.hpp"

#include "goblin_inject.hpp" // goblin::live_graces / marker_world_pos / LiveGrace

namespace goblin::worldmap
{
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
        cache_.push_back(Marker{wx, wz, grp, (int)e.areaNo, 0xEB82E65Au, "show_graces"});
    }
    return cache_;
}
} // namespace goblin::worldmap
