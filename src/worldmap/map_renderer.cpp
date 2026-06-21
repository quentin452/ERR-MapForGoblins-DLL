#include "map_renderer.hpp"

#include "goblin_projection.hpp"     // baked map-space → backbuffer projection
#include "goblin_worldmap_probe.hpp" // get_live_view()
#include "goblin_inject.hpp"         // ui::clustering_enabled / global_threshold / category_clustered
#include "goblin_config.hpp"         // overlay marker scale config
#include "goblin_messages.hpp"       // lookup_text_utf8 (tooltip names)
#include "goblin_collected.hpp"      // is_original_row_collected (rune/ember graying)
#include "goblin_kindling.hpp"       // is_row_collected (kindling graying)
#include "goblin_major_regions.hpp"  // MAJOR_REGION_ANCHORS (region labels)
#include "goblin_quest_gates.hpp"    // QUEST_GATES (quest-NPC gating)
#include "goblin_map_data.hpp"       // Category enum (WorldQuestNPC)
#include "goblin_grace_anchors.hpp"  // GRACE_ANCHOR_COUNT (player-pos debug viz)

#include <string>
#include "generated_shared/goblin_overlay_icons.hpp" // ICON_CELLS / ATLAS dims

#include <imgui.h>

#include <cfloat>
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

// Desaturate toward luminance + halve alpha → the "collected" dim tint (packed ABGR).
unsigned int dim_color(unsigned int abgr)
{
    int a = (abgr >> 24) & 0xff, b = (abgr >> 16) & 0xff, g = (abgr >> 8) & 0xff, r = abgr & 0xff;
    int lum = (r * 54 + g * 183 + b * 19) >> 8; // ITU-R-ish luma
    r = (r + lum * 2) / 3; g = (g + lum * 2) / 3; b = (b + lum * 2) / 3;
    a /= 2;
    return ((unsigned)a << 24) | ((unsigned)b << 16) | ((unsigned)g << 8) | (unsigned)r;
}

// Is this marker's item collected / boss cleared? cleared_only reports the boss-clear
// sub-case (gets a checkmark). Event flags (cleared/loot) read the live game state;
// rune/ember/kindling use the collected-tracking sets (refreshed by the mod thread).
bool marker_done(const Marker &m, bool &cleared_only)
{
    cleared_only = m.cleared_flag && goblin::ui::read_event_flag((uint32_t)m.cleared_flag);
    if (cleared_only)
        return true;
    if (m.collected_flag && goblin::ui::read_event_flag((uint32_t)m.collected_flag))
        return true;
    if (m.row_id && (goblin::collected::is_original_row_collected(m.row_id) ||
                     goblin::kindling::is_row_collected(m.row_id)))
        return true;
    return false;
}

// A small green checkmark at the marker's top-right (cleared bosses, native-style).
void draw_check(ImDrawList *fg, ImVec2 p, float half)
{
    const float s = half * 0.8f;
    const ImVec2 c(p.x + half * 0.55f, p.y - half * 0.55f);
    fg->AddCircleFilled(c, s * 0.7f, IM_COL32(18, 40, 18, 200));
    const ImU32 col = IM_COL32(70, 225, 90, 255);
    fg->AddLine(ImVec2(c.x - s * 0.45f, c.y + s * 0.02f),
                ImVec2(c.x - s * 0.08f, c.y + s * 0.38f), col, 2.2f);
    fg->AddLine(ImVec2(c.x - s * 0.08f, c.y + s * 0.38f),
                ImVec2(c.x + s * 0.50f, c.y - s * 0.42f), col, 2.2f);
}

// Boss markers tinted red when redify_boss_icons is on (overlay port of the legacy
// red-skull iconId 374). The overlay has one WorldBosses category covering overworld +
// dungeon bosses; collected/cleared graying takes precedence (a dead boss grays). The
// legacy redify_dungeon (dungeon-ENTRANCE markers) has no overlay equivalent — the
// overlay projects dungeon CONTENTS, not a separate entrance marker — so it's native-only.
inline bool redify_boss(const Marker &m)
{
    return goblin::config::redifyBossIcons &&
           m.category == static_cast<int>(goblin::generated::Category::WorldBosses);
}

// Draw one marker at backbuffer px p: the atlas icon if available, else a circle.
// half = icon half-size in px (resolution-scaled by the caller). When collected_graying
// is on, collected/cleared markers dim+desaturate (or hide if hide_collected), and
// cleared bosses get a green checkmark. Uncollected bosses redden when redify_boss_icons.
void draw_marker(ImDrawList *fg, const Marker &m, ImVec2 p, ImTextureID atlas, float half)
{
    bool cleared = false, done = false;
    if (goblin::config::collectedGraying)
    {
        done = marker_done(m, cleared);
        if (done && goblin::config::hideCollected)
            return; // legacy-style: hide collected/cleared entirely
    }
    // Spoiler-free (anonymous_loot): lot-backed loot draws as a neutral gray "?" disc,
    // hiding the item's icon/colour. Collected ones still gray; category gate unchanged.
    if (goblin::config::anonymousLoot && m.lot_backed)
    {
        float cr = half * 0.5f;
        const ImU32 fill = done ? IM_COL32(120, 120, 120, 120) : IM_COL32(155, 155, 160, 215);
        fg->AddCircleFilled(p, cr, fill);
        fg->AddCircle(p, cr, IM_COL32(0, 0, 0, done ? 120 : 220), 0, 1.5f);
        const char *q = "?";
        ImVec2 ts = ImGui::CalcTextSize(q);
        fg->AddText(ImVec2(p.x - ts.x * 0.5f, p.y - ts.y * 0.5f),
                    IM_COL32(25, 25, 30, done ? 150 : 255), q);
        return;
    }
    const bool red = !done && redify_boss(m);
    const ImU32 tint = done ? IM_COL32(150, 150, 150, 130)
                            : (red ? IM_COL32(255, 70, 70, 255) : IM_COL32(255, 255, 255, 255));

    ImVec2 uv0, uv1;
    if (atlas && icon_uv(m.icon_key, uv0, uv1))
    {
        fg->AddImage(atlas, ImVec2(p.x - half, p.y - half), ImVec2(p.x + half, p.y + half),
                     uv0, uv1, tint);
    }
    else
    {
        float cr = half * 0.45f;
        const ImU32 fill = done ? dim_color(m.color) : (red ? IM_COL32(235, 70, 70, 255) : m.color);
        fg->AddCircleFilled(p, cr, fill);
        fg->AddCircle(p, cr, IM_COL32(0, 0, 0, done ? 120 : 220), 0, 1.5f);
    }
    if (cleared)
        draw_check(fg, p, half);
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

// Best hovered item this frame (for the tooltip). bestd = squared px distance.
struct Hover
{
    float bestd = 1e30f;
    ImVec2 pos;
    std::string text;
};
// If `mouse` is within radius r of p and closer than the current best, take it.
void hover_test(Hover &h, ImVec2 mouse, ImVec2 p, float r, const std::string &text)
{
    if (mouse.x < 0 || text.empty()) return;
    float dx = mouse.x - p.x, dy = mouse.y - p.y, d = dx * dx + dy * dy;
    if (d <= r * r && d < h.bestd) { h.bestd = d; h.pos = p; h.text = text; }
}
// One marker's tooltip = its item/marker name + its location (nearest-grace region),
// like the native map. Either may be empty; empty if both are.
std::string marker_label(const Marker &m)
{
    std::string loc = goblin::lookup_text_utf8(m.loc_pname);
    // Spoiler-free: don't leak the item name — just "?" (+ its location, like native).
    if (goblin::config::anonymousLoot && m.lot_backed)
        return loc.empty() ? std::string("?") : ("?\n" + loc);
    std::string name = goblin::lookup_text_utf8(m.name_id);
    if (name.empty()) return loc;
    if (loc.empty()) return name;
    return name + "\n" + loc; // item name, then its location on the next line
}
// A cluster pile's tooltip = its location name (if any) + the member count. Shows
// "<remaining>/<total> left" while some are uncollected, "<total> markers" otherwise.
std::string pile_label(int loc_pname, int remaining, int total)
{
    std::string loc = goblin::lookup_text_utf8(loc_pname);
    std::string n = (remaining == total)
                        ? (std::to_string(total) + " markers")
                        : (std::to_string(remaining) + "/" + std::to_string(total) + " left");
    return loc.empty() ? n : (loc + "\n" + n);
}
// Draw a small dark tooltip box near the cursor on the foreground draw list.
void draw_tooltip(ImDrawList *fg, ImVec2 at, const std::string &text)
{
    if (text.empty()) return;
    ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    ImVec2 p0(at.x + 16, at.y + 6), p1(p0.x + ts.x + 12, p0.y + ts.y + 8);
    fg->AddRectFilled(p0, p1, IM_COL32(20, 20, 26, 235), 4.f);
    fg->AddRect(p0, p1, IM_COL32(255, 255, 255, 70), 4.f);
    fg->AddText(ImVec2(p0.x + 6, p0.y + 4), IM_COL32(245, 245, 245, 255), text.c_str());
}

// Draw a cluster pile glyph (filled disc + remaining-member count) at screen point c.
// depleted = every member collected → green-tinted disc (mirrors the legacy native
// CLUSTER_DONE icon) so a finished area reads as done at a glance.
void draw_cluster_glyph(ImDrawList *fg, ImVec2 c, int n, float r, bool depleted)
{
    const ImU32 fill = depleted ? IM_COL32(28, 44, 30, 205) : IM_COL32(40, 42, 52, 235);
    const ImU32 ring = depleted ? IM_COL32(70, 205, 95, 225) : IM_COL32(255, 255, 255, 230);
    const ImU32 txt = depleted ? IM_COL32(150, 225, 155, 255) : IM_COL32(255, 255, 255, 255);
    fg->AddCircleFilled(c, r, fill);
    fg->AddCircle(c, r, ring, 0, 2.0f);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", n);
    ImVec2 ts = ImGui::CalcTextSize(buf);
    fg->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), txt, buf);
}

// Group clustered markers by their nearest-grace key (matches the native map's
// by-location clustering, NOT screen proximity — markers sharing a grace pile even
// when spread out). A group with MORE than `threshold` members draws ONE glyph at the
// member screen-centroid; smaller groups draw their members normally. Off-screen
// piles/markers are culled. realW/realH = backbuffer size for the cull.
void draw_clusters(ImDrawList *fg, const std::vector<ScreenMarker> &items, int threshold,
                   ImTextureID atlas, float realW, float realH,
                   const goblin::projection::View &view, bool dlc_ug, bool overworld_page,
                   float iconHalf, float glyphR, ImVec2 mouse, Hover &hover)
{
    namespace proj = goblin::projection;
    auto on_screen = [&](const ImVec2 &p) {
        return !(p.x < -32 || p.y < -32 || p.x > realW + 32 || p.y > realH + 32);
    };
    std::unordered_map<int, std::vector<int>> groups; // cluster_key → member indices
    groups.reserve(items.size());
    for (int i = 0; i < (int)items.size(); ++i)
        groups[items[i].m->cluster_key].push_back(i);

    // Distance-adaptive: when on, the per-location threshold ramps from near_thr (full
    // detail around the player) to base_thr (clustered far away) over near→far radius,
    // SAME overworld page only (world coords aren't comparable across pages, and the
    // underground player float is unreliable). This is its own feature: enabling it
    // OVERRIDES per-category opt-in (every category clusters by distance — handled at
    // the eligibility check in render_markers).
    namespace cfg = goblin::config;
    const bool dist_adaptive = cfg::clusterDistanceAdaptive && overworld_page; // overworld only
    const int base_thr = threshold;
    int near_thr = (int)cfg::clusterNearThreshold;
    if (near_thr < 1) near_thr = 1;
    const float near_u = cfg::clusterNearRadius * 256.0f;
    const float far_u = cfg::clusterFarRadius * 256.0f;
    int player_area = -1;
    float pwx = 0, pwz = 0;
    const bool have_player =
        dist_adaptive && goblin::get_player_map_pos(player_area, pwx, pwz);
    const bool euclid_frame = (player_area == 60 || player_area == 61); // overworld pages

    // DEBUG viz (config cluster_debug_radius): player marker + near/far rings so you can
    // SEE where the distance ramp engages. Overworld only (= where dist_adaptive runs).
    const bool dbg_radius = goblin::config::clusterDebugRadius && have_player;
    if (dbg_radius)
    {
        float gU, gV;
        world_to_mapspace_xy(pwx, pwz, dlc_ug, gU, gV);
        proj::Px pp = proj::project_screen(gU, gV, view, realW, realH);
        const ImVec2 ppx(pp.x, pp.y);
        auto ring_px = [&](float r) {
            float u, v;
            world_to_mapspace_xy(pwx + r, pwz, dlc_ug, u, v);
            proj::Px e = proj::project_screen(u, v, view, realW, realH);
            return std::fabs(e.x - pp.x);
        };
        fg->AddCircle(ppx, ring_px(near_u), IM_COL32(60, 230, 90, 210), 64, 2.f);  // near=detail
        fg->AddCircle(ppx, ring_px(far_u), IM_COL32(255, 90, 30, 210), 64, 2.f);   // far=clustered
        fg->AddCircleFilled(ppx, 6.f, IM_COL32(255, 0, 255, 255));
        fg->AddCircle(ppx, 6.f, IM_COL32(0, 0, 0, 200), 0, 1.5f);
        char b[48];
        std::snprintf(b, sizeof(b), "player A%d", player_area);
        fg->AddText(ImVec2(ppx.x + 8, ppx.y - 6), IM_COL32(255, 120, 255, 255), b);
    }

    // Pass 1: resolve each pile's GRACE anchor + its (distance-adaptive) threshold.
    // Sub-threshold groups draw their members normally; denser ones become a pile.
    struct Pile { ImVec2 g; int count; int total; int loc_pname; int thr; };
    std::vector<Pile> piles;
    for (auto &kv : groups)
    {
        const auto &idxs = kv.second;
        // Grace anchor (world + page) up front — needed for the distance ramp AND the
        // pile placement below.
        int garea = -1;
        float gwx = 0, gwz = 0;
        const bool has_anchor = goblin::grace_anchor_world(kv.first, garea, gwx, gwz);
        // Per-location threshold. Distance-adaptive (OVERWORLD only — dist_adaptive is
        // gated off underground because the underground player position is unavailable):
        // Euclidean ramp near_thr→base_thr over the near→far radius. Otherwise flat
        // base_thr (normal threshold clustering).
        int thr = base_thr;
        if (have_player && overworld_page && has_anchor && euclid_frame &&
            garea == player_area && far_u > near_u)
        {
            const float dx = gwx - pwx, dz = gwz - pwz;
            const float d = std::sqrt(dx * dx + dz * dz);
            float t = (d - near_u) / (far_u - near_u);
            if (t < 0.f) t = 0.f; else if (t > 1.f) t = 1.f;
            thr = (int)std::lround(near_thr + t * (base_thr - near_thr));
            if (thr < 1) thr = 1;
        }
        if ((int)idxs.size() <= thr)
        {
            for (int i : idxs)
                if (on_screen(items[i].p))
                {
                    draw_marker(fg, *items[i].m, items[i].p, atlas, iconHalf);
                    hover_test(hover, mouse, items[i].p, iconHalf, marker_label(*items[i].m));
                }
            continue;
        }
        // Pile AT its grace (correctly placed), not the member centroid (which drifts
        // into the sea). Fall back to the centroid only if the anchor is bad.
        ImVec2 c;
        if (has_anchor)
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
        // Pile count = UNCOLLECTED members (depletion), so the glyph reflects progress
        // instead of the static total. When collected_graying is off, show the full
        // total. marker_done = the same collected/cleared predicate used for graying.
        int total = (int)idxs.size();
        int remaining = total;
        if (goblin::config::collectedGraying)
        {
            remaining = 0;
            for (int i : idxs)
            {
                bool co;
                if (!marker_done(*items[i].m, co))
                    ++remaining;
            }
        }
        piles.push_back({c, remaining, total, items[idxs[0]].m->loc_pname, thr});
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
        {
            const bool depleted = (piles[i].count == 0 && piles[i].total > 0);
            draw_cluster_glyph(fg, best, piles[i].count, glyphR, depleted);
            if (dbg_radius)
            {
                // The per-location threshold this pile used (near_thr vs base_thr =
                // distance/tab decision) — drawn above the glyph in cyan.
                char tb[24];
                std::snprintf(tb, sizeof(tb), "thr%d", piles[i].thr);
                fg->AddText(ImVec2(best.x - glyphR, best.y - glyphR - 14.f),
                            IM_COL32(80, 230, 255, 255), tb);
            }
            hover_test(hover, mouse, best, glyphR,
                       pile_label(piles[i].loc_pname, piles[i].count, piles[i].total));
            // Location name centred under the glyph (shadowed for readability on the busy map).
            std::string loc = goblin::lookup_text_utf8(piles[i].loc_pname);
            if (!loc.empty())
            {
                ImVec2 ts = ImGui::CalcTextSize(loc.c_str());
                ImVec2 tp(best.x - ts.x * 0.5f, best.y + glyphR + 2.f);
                fg->AddText(ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 205), loc.c_str());
                fg->AddText(tp, IM_COL32(255, 255, 255, 235), loc.c_str());
            }
        }
    }
}

// Does a major-region anchor (its `area` page id) belong to the currently OPEN map
// group? Anchor pages: 10/60 = base overworld, 12 = base underground, 61 = DLC
// overworld. (No major-region anchor exists for the DLC underground, group 3.)
bool region_in_group(uint8_t area, int open_grp)
{
    switch (open_grp)
    {
    case 0: return area == 10 || area == 60; // base overworld
    case 1: return area == 12;               // base underground
    case 2: return area == 61;               // DLC overworld
    default: return false;                   // group 3 (DLC UG): none
    }
}

// Draw the coarse major-region names (Limgrave, Caelid, ...) on the open page, beneath
// the markers. Each anchor's world centre projects exactly like a marker; the label is
// drawn large + shadowed, centred on the anchor. Off-screen labels are culled.
void draw_region_labels(ImDrawList *fg, int open_grp, bool dlc_ug,
                        const goblin::projection::View &view, float realW, float realH,
                        float uiScale)
{
    namespace proj = goblin::projection;
    using namespace goblin::generated;
    const float fontSize = ImGui::GetFontSize() * 1.6f * uiScale;
    const ImU32 col = IM_COL32(238, 226, 188, 205);   // muted gold, semi-transparent
    const ImU32 shadow = IM_COL32(0, 0, 0, 190);
    ImFont *font = ImGui::GetFont();
    for (size_t i = 0; i < MAJOR_REGION_ANCHOR_COUNT; ++i)
    {
        const MajorRegionAnchor &a = MAJOR_REGION_ANCHORS[i];
        if (!region_in_group(a.area, open_grp))
            continue;
        float gU, gV;
        world_to_mapspace_xy(a.wx, a.wz, dlc_ug, gU, gV);
        proj::Px p = proj::project_screen(gU, gV, view, realW, realH);
        if (p.x < -64 || p.y < -32 || p.x > realW + 64 || p.y > realH + 32)
            continue;
        ImVec2 ts = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, a.name);
        ImVec2 tp(p.x - ts.x * 0.5f, p.y - ts.y * 0.5f);
        fg->AddText(font, fontSize, ImVec2(tp.x + 1.5f, tp.y + 1.5f), shadow, a.name);
        fg->AddText(font, fontSize, tp, col, a.name);
    }
}

// Quest-aware gating: a WorldQuestNPC marker is hidden while its questline is
// inactive (NONE of its quest-active flags is set). Mirrors the legacy native gate
// (refresh_quest_npc_eviction, goblin_inject.cpp). An NPC with no QuestGate entry is
// always shown. Gate join is by name_id (== QuestGate.nameId).
bool quest_npc_gated_out(const Marker &m)
{
    using namespace goblin::generated;
    if (!goblin::config::questNpcQuestAware) return false;
    if (m.category != (int)Category::WorldQuestNPC) return false;
    // Cold-API safety: until AlwaysOn (6001) reads true the flag manager isn't warm —
    // never blank every quest NPC on map open. Show as-is.
    if (!goblin::ui::read_event_flag(6001)) return false;
    for (size_t i = 0; i < QUEST_GATE_COUNT; ++i)
        if (QUEST_GATES[i].nameId == (uint32_t)m.name_id)
        {
            for (uint32_t f : QUEST_GATES[i].flags)
                if (f && goblin::ui::read_event_flag(f)) return false; // quest active → show
            return true; // gate found, none of its flags active → hide
        }
    return false; // no gate for this NPC → always show
}
} // namespace

void render_markers(const std::vector<MarkerLayer *> &layers, void *atlas_texture, float mouseX,
                    float mouseY)
{
    namespace proj = goblin::projection;
    ImTextureID atlas = reinterpret_cast<ImTextureID>(atlas_texture);
    const ImVec2 mouse(mouseX, mouseY);
    Hover hover;
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

    // Player-pos debug (cluster_debug_radius): the PLAYER dot (magenta) via
    // get_player_map_pos + a HUD of its full pipeline + EVERY grace anchor projected as a
    // small cyan dot. Graces use the marker pipeline (correct), so the grace nearest the
    // native yellow dot shows where the player SHOULD land — the magenta-vs-cyan delta +
    // the HUD tile/world numbers reveal whether the player's MapId tile/local is in a
    // different frame than WorldMapPointParam (the underground mis-projection hypothesis).
    if (goblin::config::clusterDebugRadius)
    {
        const float realWd = io.DisplaySize.x, realHd = io.DisplaySize.y;
        // Cyan grace anchors (the correct pipeline) for reference — gated to the OPEN page
        // via the SAME group helper the layers use, so underground shows only underground
        // graces (not the whole overworld set).
        for (size_t i = 0; i < goblin::generated::GRACE_ANCHOR_COUNT; ++i)
        {
            int ga; float gwx, gwz;
            if (!goblin::grace_anchor_world((int)i, ga, gwx, gwz)) continue;
            if (goblin::marker_group_from(goblin::generated::GRACE_ANCHORS[i].area, ga) != open_grp)
                continue;
            float gU, gV; world_to_mapspace_xy(gwx, gwz, dlc_ug, gU, gV);
            proj::Px gp = proj::project_screen(gU, gV, view, realWd, realHd);
            if (gp.x < -8 || gp.y < -8 || gp.x > realWd + 8 || gp.y > realHd + 8) continue;
            fg->AddCircleFilled(ImVec2(gp.x, gp.y), 4.f, IM_COL32(40, 220, 255, 200));
        }
        int pa = -1, pgx = -1, pgz = -1; float pwx = 0, pwz = 0;
        const bool okp = goblin::get_player_map_pos(pa, pwx, pwz, &pgx, &pgz);
        float gU = 0, gV = 0; proj::Px p{};
        if (okp)
        {
            world_to_mapspace_xy(pwx, pwz, dlc_ug, gU, gV);
            p = proj::project_screen(gU, gV, view, realWd, realHd);
            ImVec2 q(p.x, p.y);
            fg->AddCircleFilled(q, 11.f, IM_COL32(255, 0, 255, 255));
            fg->AddCircle(q, 11.f, IM_COL32(0, 0, 0, 220), 0, 2.f);
            fg->AddText(ImVec2(q.x + 14, q.y - 7), IM_COL32(255, 0, 255, 255), "PLAYER");
        }
        char hud[320];
        std::snprintf(hud, sizeof(hud),
                      "[PLR] open_grp=%d ug=%d ok=%d | proj_area=%d projTile=(%d,%d)\n"
                      "world=(%.0f,%.0f) map=(%.0f,%.0f) px=(%.0f,%.0f)  (cyan=grace anchors)",
                      open_grp, (int)dlc_ug, okp, pa, pgx, pgz, pwx, pwz, gU, gV, p.x, p.y);
        fg->AddRectFilled(ImVec2(12, 88), ImVec2(700, 132), IM_COL32(10, 10, 16, 230), 4.f);
        fg->AddRect(ImVec2(12, 88), ImVec2(700, 132), IM_COL32(255, 0, 255, 255), 4.f);
        fg->AddText(ImVec2(20, 96), IM_COL32(255, 255, 255, 255), hud);
    }

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
    // Distance-adaptive is its own clustering mode: when on it OVERRIDES the per-category
    // opt-in (every category clusters by distance from the player; see draw_clusters).
    // OVERWORLD ONLY: the underground player position is unavailable (WorldChrMan physics
    // returns NaN, MapId map-pos reads garbage origin) → can't gauge distance there, so
    // underground falls back to normal threshold + per-category clustering.
    const bool dist_adaptive =
        clustering && goblin::config::clusterDistanceAdaptive && !(open_grp & 1);
    // Resolution-relative icon/glyph sizes (match the native canvas-scaled icons),
    // × the user's master scale × the per-type scale (saved in the ini).
    const float uiScale = realH / 1080.f;
    const float master = goblin::config::overlayMasterScale;
    const float iconHalf = kIconHalfBase * uiScale * master * goblin::config::overlayIconScale;
    const float glyphR = kGlyphRBase * uiScale * master * goblin::config::overlayClusterScale;
    std::vector<ScreenMarker> clustered; // markers whose category opted into clustering

    // Region names beneath the markers (major-region anchors for the open page).
    if (goblin::config::showRegionLabels)
        draw_region_labels(fg, open_grp, dlc_ug, view, realW, realH, uiScale);

    for (auto *L : layers)
    {
        if (!L || !L->visible())
            continue;
        for (const Marker &m : L->markers())
        {
            if (m.group != open_grp)
                continue; // draw only the open map group
            // Discovered graces are drawn by the game natively (generated from
            // BonfireWarpParam), so drop our overlay marker to avoid a double icon.
            // discover_flag is set only on grace markers; read live so it updates the
            // moment the player rests at a grace.
            if (m.discover_flag && goblin::ui::read_event_flag((uint32_t)m.discover_flag))
                continue;
            // Quest-aware gating: hide a quest-NPC whose questline is currently inactive.
            if (quest_npc_gated_out(m))
                continue;
            // Post-event story gate: a marker tagged with a secondary story flag (post-burn
            // Leyndell / Chapel, Ashen Capital, Charm-broken, Sealing-tree-burnt) is a
            // post-event variant and appears only once that flag is set. Read live so it
            // reveals the instant the event fires. Legacy parity: SetSecondaryFlags
            // (textEnableFlag2) + the Ashen Capital eventFlag gate.
            if (m.secondary_flag && !goblin::ui::read_event_flag((uint32_t)m.secondary_flag))
                continue;
            float gU, gV;
            world_to_mapspace(m, dlc_ug, gU, gV);
            // Discovery gate: when require_map_fragments is on, hide a marker whose map
            // piece is still fogged — the engine's REAL fog-of-war reveal state
            // (WorldMapPieceParam, in-game-confirmed exact, no calibration needed). group
            // bits = isDLC*2 | isUG → fog layer areaIdx {0 OW, 1 UG, 10 DLC}.
            if (goblin::config::requireMapFragments)
            {
                const int areaIdx = (m.group & 2) ? 10 : (m.group & 1);
                if (goblin::marker_fogged(areaIdx, gU, gV))
                    continue;
            }
            proj::Px p = proj::project_screen(gU, gV, view, realW, realH);
            ImVec2 sp(p.x, p.y);
            // Clustered-eligible markers are deferred WITHOUT culling — an off-screen
            // member still counts toward its grace pile + centroid (a pile can sit on
            // screen while some members are off it). Everything else culls + draws now.
            if (clustering && m.category >= 0 && m.cluster_key >= 0 &&
                (dist_adaptive || goblin::ui::category_clustered(m.category)))
                clustered.push_back({sp, &m});
            else if (!(sp.x < -32 || sp.y < -32 || sp.x > realW + 32 || sp.y > realH + 32))
            {
                draw_marker(fg, m, sp, atlas, iconHalf);
                hover_test(hover, mouse, sp, iconHalf, marker_label(m));
            }
        }
    }

    if (clustering && !clustered.empty())
        draw_clusters(fg, clustered, threshold, atlas, realW, realH, view, dlc_ug,
                      /*overworld_page=*/!(open_grp & 1), iconHalf, glyphR, mouse, hover);

    // Tooltip for the hovered marker / pile (drawn last so it's on top).
    if (hover.bestd < 1e30f)
        draw_tooltip(fg, mouse, hover.text);
}

void draw_minimap(const std::vector<MarkerLayer *> &layers, void *atlas_texture, float screenW,
                  float screenH)
{
    namespace cfg = goblin::config;
    if (!cfg::showMinimap || !goblin::ui::icons_enabled())
        return;
    // Live player position (read during gameplay, map closed). OVERWORLD only — the
    // underground player position isn't reliable yet (see overlay-pending-re #1).
    int parea = 0;
    float pwx = 0.f, pwz = 0.f;
    if (!goblin::get_player_map_pos(parea, pwx, pwz))
        return;
    int pgroup;
    if (parea == 60)
        pgroup = 0; // base overworld
    else if (parea == 61)
        pgroup = 2; // DLC overworld
    else
        return; // underground / unknown page → no minimap (player pos unreliable)

    const float R = cfg::minimapSize > 24.f ? cfg::minimapSize : 24.f;
    const float scale = cfg::minimapZoom > 0.0001f ? cfg::minimapZoom : 0.08f;
    const float margin = 24.f;
    const ImVec2 ctr(screenW - R - margin, R + margin); // top-right corner
    const float cullR = R - 5.f;

    int bgA = (int)(cfg::minimapOpacity * 255.f);
    bgA = bgA < 0 ? 0 : (bgA > 255 ? 255 : bgA);

    ImDrawList *fg = ImGui::GetForegroundDrawList();
    fg->AddCircleFilled(ctr, R, IM_COL32(12, 14, 20, bgA), 64);
    fg->AddCircle(ctr, R, IM_COL32(230, 220, 180, 200), 64, 2.0f);

    fg->PushClipRect(ImVec2(ctr.x - R, ctr.y - R), ImVec2(ctr.x + R, ctr.y + R), true);
    ImTextureID atlas = reinterpret_cast<ImTextureID>(atlas_texture);
    const float half = 6.0f; // minimap markers are small + fixed-size
    for (auto *L : layers)
    {
        if (!L || !L->visible())
            continue;
        for (const Marker &m : L->markers())
        {
            if (m.group != pgroup)
                continue; // only the player's overworld page
            // Same hide-gates as the worldmap (discovered grace, quest-NPC, post-event story).
            if (m.discover_flag && goblin::ui::read_event_flag((uint32_t)m.discover_flag))
                continue;
            if (quest_npc_gated_out(m))
                continue;
            if (m.secondary_flag && !goblin::ui::read_event_flag((uint32_t)m.secondary_flag))
                continue;
            // North-up, player-centred: same orientation as the worldmap (mapV = -worldZ).
            float dx = (m.worldX - pwx) * scale;
            float dy = -(m.worldZ - pwz) * scale;
            if (dx * dx + dy * dy > cullR * cullR)
                continue; // outside the HUD radius
            // Fog gate: hide markers on a still-fogged map piece (require_map_fragments).
            if (cfg::requireMapFragments)
            {
                float gU, gV;
                world_to_mapspace(m, /*dlc_ug=*/false, gU, gV);
                const int areaIdx = (m.group & 2) ? 10 : 0;
                if (goblin::marker_fogged(areaIdx, gU, gV))
                    continue;
            }
            draw_marker(fg, m, ImVec2(ctr.x + dx, ctr.y + dy), atlas, half);
        }
    }
    fg->PopClipRect();

    // Player marker at centre + a north tick (no heading yet → north-up only).
    fg->AddCircleFilled(ctr, 4.0f, IM_COL32(255, 225, 70, 255));
    fg->AddCircle(ctr, 4.0f, IM_COL32(0, 0, 0, 200), 0, 1.5f);
    fg->AddText(ImVec2(ctr.x - 4.f, ctr.y - R - 16.f), IM_COL32(230, 220, 180, 220), "N");
}
} // namespace goblin::worldmap
