#include "map_renderer.hpp"

#include "goblin_projection.hpp"     // baked map-space → backbuffer projection
#include "goblin_worldmap_probe.hpp" // get_live_view()
#include "goblin_inject.hpp"         // ui::clustering_enabled / global_threshold / category_clustered
#include "goblin_config.hpp"         // overlay marker scale config
#include "generated_shared/goblin_overlay_icons.hpp" // ICON_CELLS / ATLAS dims

#include <imgui.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

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

// Marker/glyph base sizes at 1920×1080; scaled by realH/1080 at draw time so they
// track the native GFx icons (which scale with the canvas) instead of looking
// oversized at low res / undersized at high res.
constexpr float kIconHalfBase = 10.f; // ~20px sprite at 1080 (native-ish)
constexpr float kGlyphRBase = 12.f;   // cluster pile disc radius at 1080

// Resolve a marker's atlas cell to UVs. Returns false if no atlas / key missing.
bool icon_uv(const char *key, ImVec2 &uv0, ImVec2 &uv1)
{
    using namespace goblin::overlay_icons;
    if (!key)
        return false;
    for (int i = 0; i < ICON_CELL_COUNT; ++i)
        if (std::strcmp(ICON_CELLS[i].key, key) == 0)
        {
            const IconCell &c = ICON_CELLS[i];
            uv0 = ImVec2((c.col * CELL) / (float)ATLAS_W, (c.row * CELL) / (float)ATLAS_H);
            uv1 = ImVec2(((c.col + 1) * CELL) / (float)ATLAS_W,
                         ((c.row + 1) * CELL) / (float)ATLAS_H);
            return true;
        }
    return false;
}

// Draw one marker at backbuffer px p: the atlas icon if available, else a circle.
// half = icon half-size in px (resolution-scaled by the caller).
void draw_marker(ImDrawList *fg, const Marker &m, ImVec2 p, ImTextureID atlas, float half)
{
    ImVec2 uv0, uv1;
    if (atlas && icon_uv(m.icon_key, uv0, uv1))
    {
        fg->AddImage(atlas, ImVec2(p.x - half, p.y - half), ImVec2(p.x + half, p.y + half),
                     uv0, uv1);
    }
    else
    {
        float cr = half * 0.45f;
        fg->AddCircleFilled(p, cr, m.color);
        fg->AddCircle(p, cr, IM_COL32(0, 0, 0, 220), 0, 1.5f);
    }
}

// Unified world coords → map-space (the frame project_screen expects).
void world_to_mapspace_xy(float worldX, float worldZ, bool dlc_ug, float &gU, float &gV)
{
    if (!dlc_ug)
    {
        // EXACT world→map-space (agent RE 0a30738): origin (7168,16384), bias 128,
        // scale 1.0, Z-flipped: mapX = worldX − 7040 ; mapZ = −worldZ + 16512.
        gU = worldX - 7040.0f;
        gV = -worldZ + 16512.0f;
    }
    else
    {
        dlc_ug_eyeball(worldX, worldZ, gU, gV);
    }
}
inline void world_to_mapspace(const Marker &m, bool dlc_ug, float &gU, float &gV)
{
    world_to_mapspace_xy(m.worldX, m.worldZ, dlc_ug, gU, gV);
}

// A projected marker awaiting the clustering decision.
struct ScreenMarker
{
    ImVec2 p;
    const Marker *m;
};

// Draw a cluster pile glyph (filled disc + member count) at screen point c.
void draw_cluster_glyph(ImDrawList *fg, ImVec2 c, int n, float r)
{
    fg->AddCircleFilled(c, r, IM_COL32(40, 42, 52, 235));
    fg->AddCircle(c, r, IM_COL32(255, 255, 255, 230), 0, 2.0f);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", n);
    ImVec2 ts = ImGui::CalcTextSize(buf);
    fg->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(255, 255, 255, 255), buf);
}

// Group clustered markers by their nearest-grace key (matches the native map's
// by-location clustering, NOT screen proximity — markers sharing a grace pile even
// when spread out). A group with MORE than `threshold` members draws ONE glyph at the
// member screen-centroid; smaller groups draw their members normally. Off-screen
// piles/markers are culled. realW/realH = backbuffer size for the cull.
void draw_clusters(ImDrawList *fg, const std::vector<ScreenMarker> &items, int threshold,
                   ImTextureID atlas, float realW, float realH,
                   const goblin::projection::View &view, bool dlc_ug, float iconHalf,
                   float glyphR)
{
    namespace proj = goblin::projection;
    auto on_screen = [&](const ImVec2 &p) {
        return !(p.x < -32 || p.y < -32 || p.x > realW + 32 || p.y > realH + 32);
    };
    std::unordered_map<int, std::vector<int>> groups; // cluster_key → member indices
    groups.reserve(items.size());
    for (int i = 0; i < (int)items.size(); ++i)
        groups[items[i].m->cluster_key].push_back(i);

    // Pass 1: resolve each pile's GRACE anchor position (sub-threshold groups draw
    // their members normally now). The anchor pos doubles as the neighbour set used to
    // pick a non-overlapping offset below.
    struct Pile { ImVec2 g; int count; };
    std::vector<Pile> piles;
    for (auto &kv : groups)
    {
        const auto &idxs = kv.second;
        if ((int)idxs.size() <= threshold)
        {
            for (int i : idxs)
                if (on_screen(items[i].p))
                    draw_marker(fg, *items[i].m, items[i].p, atlas, iconHalf);
            continue;
        }
        // Pile AT its grace (correctly placed), not the member centroid (which drifts
        // into the sea). Fall back to the centroid only if the anchor is bad.
        ImVec2 c;
        int garea;
        float gwx, gwz;
        if (goblin::grace_anchor_world(kv.first, garea, gwx, gwz))
        {
            float gU, gV;
            world_to_mapspace_xy(gwx, gwz, dlc_ug, gU, gV);
            proj::Px gp = proj::project_screen(gU, gV, view, realW, realH);
            c = ImVec2(gp.x, gp.y);
        }
        else
        {
            float sx = 0, sy = 0;
            for (int i : idxs) { sx += items[i].p.x; sy += items[i].p.y; }
            c = ImVec2(sx / idxs.size(), sy / idxs.size());
        }
        piles.push_back({c, (int)idxs.size()});
    }

    // Pass 2: nudge each pile off its grace icon (so both stay visible), in the cardinal
    // direction pointing AWAY from its nearest neighbouring grace — so a pile next to
    // other graces doesn't land on one. The direction is derived from the screen vector
    // between two graces, whose ANGLE is zoom-invariant (zoom scales both graces'
    // positions by the same factor), so the chosen direction is STABLE across zoom —
    // no flipping when you zoom in/out. Isolated piles default to "below".
    // Offset depends ONLY on the cluster glyph size (not iconHalf) so the category
    // icon scale never moves the piles — clusters are governed by the cluster scale.
    const float OFF = glyphR * 2.0f;
    for (size_t i = 0; i < piles.size(); ++i)
    {
        int nn = -1;
        float nnd = 1e30f;
        for (size_t j = 0; j < piles.size(); ++j)
        {
            if (j == i) continue;
            float dx = piles[i].g.x - piles[j].g.x, dy = piles[i].g.y - piles[j].g.y;
            float d = dx * dx + dy * dy;
            if (d < nnd) { nnd = d; nn = (int)j; }
        }
        ImVec2 off(0.f, OFF); // default: below
        if (nn >= 0)
        {
            float vx = piles[i].g.x - piles[nn].g.x, vy = piles[i].g.y - piles[nn].g.y;
            if (std::fabs(vx) > std::fabs(vy))
                off = ImVec2(vx > 0 ? OFF : -OFF, 0.f);
            else
                off = ImVec2(0.f, vy > 0 ? OFF : -OFF);
        }
        ImVec2 best(piles[i].g.x + off.x, piles[i].g.y + off.y);
        if (on_screen(best))
            draw_cluster_glyph(fg, best, piles[i].count, glyphR);
    }
}
} // namespace

void render_markers(const std::vector<MarkerLayer *> &layers, void *atlas_texture)
{
    namespace proj = goblin::projection;
    ImTextureID atlas = reinterpret_cast<ImTextureID>(atlas_texture);
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

    // Clustering = a live render pass: categories opted into clustering bin together
    // into ONE mixed pile per dense screen cell; everything else draws normally. Live
    // by construction (re-binned every frame), so toggles/zoom update with no rebuild.
    const bool clustering = goblin::ui::clustering_enabled();
    const int threshold = clustering ? goblin::ui::global_threshold() : 0;
    // Resolution-relative icon/glyph sizes (match the native canvas-scaled icons),
    // × the user's master scale × the per-type scale (saved in the ini).
    const float uiScale = realH / 1080.f;
    const float master = goblin::config::overlayMasterScale;
    const float iconHalf = kIconHalfBase * uiScale * master * goblin::config::overlayIconScale;
    const float glyphR = kGlyphRBase * uiScale * master * goblin::config::overlayClusterScale;
    std::vector<ScreenMarker> clustered; // markers whose category opted into clustering

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
            ImVec2 sp(p.x, p.y);
            // Clustered-eligible markers are deferred WITHOUT culling — an off-screen
            // member still counts toward its grace pile + centroid (a pile can sit on
            // screen while some members are off it). Everything else culls + draws now.
            if (clustering && m.category >= 0 && m.cluster_key >= 0 &&
                goblin::ui::category_clustered(m.category))
                clustered.push_back({sp, &m});
            else if (!(sp.x < -32 || sp.y < -32 || sp.x > realW + 32 || sp.y > realH + 32))
                draw_marker(fg, m, sp, atlas, iconHalf);
        }
    }

    if (clustering && !clustered.empty())
        draw_clusters(fg, clustered, threshold, atlas, realW, realH, view, dlc_ug, iconHalf,
                      glyphR);
}
} // namespace goblin::worldmap
