#include "map_renderer.hpp"

#include "goblin_projection.hpp"     // baked map-space → backbuffer projection
#include "goblin_worldmap_probe.hpp" // get_live_view()

#include <imgui.h>

#include <cmath>

namespace goblin::worldmap
{
namespace
{
// Baked: the native GFx map layer is composited exactly 1 frame behind our Present
// sample, so map-bound markers need a 1-frame view delay to ride the map.
constexpr float kViewDelayFrames = 1.0f;

goblin::projection::ViewDelay<> g_view_delay;
float g_centroidX = 0.f, g_centroidZ = 0.f; // open-group world centroid (DLC-UG pivot)

// Probe LiveView → the pure projection View (keeps goblin_projection.hpp probe-free).
goblin::projection::View to_proj_view(const goblin::worldmap_probe::LiveView &v)
{
    goblin::projection::View pv;
    pv.panX = v.panX; pv.panZ = v.panZ; pv.zoom = v.zoom;
    pv.snapMidX = v.snapMidX; pv.snapMidZ = v.snapMidZ;
    return pv;
}

// DLC UNDERGROUND (group 3, areas 40-43) world→map-space. Its page-10 converter was
// never dumped, so this stays the user's hand-tuned eyeball fit, baked: a swap@1.0
// frame (renderX≈worldZ, renderZ≈worldX) rotated −90° about the group centroid, placed
// at the render centre + a fixed pan. Approximate / unverified (the other three pages
// use the EXACT converter); precise per-page bake is in the backlog.
void dlc_ug_eyeball(float wx, float wz, float &gU, float &gV)
{
    constexpr float RC = 5248.f; // render-space centre (10496/2)
    constexpr float rot_deg = -90.f, panX = -2170.f, panZ = 850.f;
    const float dx = wx - g_centroidX, dz = wz - g_centroidZ;
    float u0 = dz, v0 = dx; // M = swap@1.0 (a=0,b=1,c=1,d=0)
    const float rr = rot_deg * 3.14159265f / 180.f, cs = cosf(rr), sn = sinf(rr);
    gU = (u0 * cs - v0 * sn) + RC + panX;
    gV = (u0 * sn + v0 * cs) + RC + panZ;
}

// Unified world coords → map-space (the frame project_screen expects).
void world_to_mapspace(const Marker &m, bool dlc_ug, float &gU, float &gV)
{
    if (!dlc_ug)
    {
        // EXACT world→map-space (agent RE 0a30738): origin (7168,16384), bias 128,
        // scale 1.0, Z-flipped: mapX = worldX − 7040 ; mapZ = −worldZ + 16512.
        gU = m.worldX - 7040.0f;
        gV = -m.worldZ + 16512.0f;
    }
    else
    {
        dlc_ug_eyeball(m.worldX, m.worldZ, gU, gV);
    }
}
} // namespace

void render_markers(const std::vector<MarkerLayer *> &layers)
{
    namespace proj = goblin::projection;
    goblin::worldmap_probe::LiveView lv;
    if (!goblin::worldmap_probe::get_live_view(lv))
    {
        g_view_delay.reset(); // map closed → re-seed the delay fresh on reopen
        return;
    }

    ImGuiIO &io = ImGui::GetIO();
    ImDrawList *fg = ImGui::GetForegroundDrawList();
    const float realW = io.DisplaySize.x, realH = io.DisplaySize.y;

    // Motion sync: delay the projected view by the baked frame so markers ride the
    // native map layer instead of leading it during a pan.
    proj::View view = to_proj_view(lv);
    g_view_delay.apply(view, kViewDelayFrames);

    // Which group's map is OPEN — from the SOLVED region getter (probe reads the
    // WorldMapDialog page+layer): openDlc = DLC map, underground = layer byte.
    const int open_grp = (lv.openDlc ? 2 : 0) | ((lv.underground != 0) ? 1 : 0);
    const bool dlc_ug = (open_grp == 3);

    // The DLC-UG eyeball rotates about the open group's world centroid. Compute it over
    // every visible layer's markers (cheap; only needed for group 3).
    if (dlc_ug)
    {
        double sx = 0, sz = 0;
        int n = 0;
        for (auto *L : layers)
        {
            if (!L || !L->visible())
                continue;
            for (const Marker &m : L->markers())
                if (m.group == open_grp) { sx += m.worldX; sz += m.worldZ; ++n; }
        }
        if (n) { g_centroidX = (float)(sx / n); g_centroidZ = (float)(sz / n); }
    }

    for (auto *L : layers)
    {
        if (!L || !L->visible())
            continue;
        for (const Marker &m : L->markers())
        {
            if (m.group != open_grp)
                continue; // draw only the open map group
            float gU, gV;
            world_to_mapspace(m, dlc_ug, gU, gV);
            proj::Px p = proj::project_screen(gU, gV, view, realW, realH);
            if (p.x < -16 || p.y < -16 || p.x > realW + 16 || p.y > realH + 16)
                continue; // ImGui doesn't CPU-cull; skip off-screen primitives ourselves
            fg->AddCircleFilled(ImVec2(p.x, p.y), 5.0f, m.color);
            fg->AddCircle(ImVec2(p.x, p.y), 5.0f, IM_COL32(0, 0, 0, 220), 0, 1.5f);
        }
    }
}
} // namespace goblin::worldmap
