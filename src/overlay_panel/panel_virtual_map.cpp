// F1 panel — MapForGoblins VIRTUAL WORLD MAP (mod-owned page, slice A: the pannable/zoomable canvas).
//
// The strategic call (docs/re/worldmap_new_page_spike_findings.md): a NATIVE new worldmap page is an
// unsolved engine WRITE frontier, but the overlay already owns its own backbuffer drawing — so a custom
// "dev world" map is a MOD-OWNED virtual page we draw ourselves, no engine registration. This is the
// container for it: an ImGui window with a world-space canvas (drag = pan, wheel = zoom about the cursor),
// a reference grid + origin cross, and a mod-defined world→canvas projection. Markers on it come next
// (slice B); a real bundle-backed custom world is World Virtualization vision #1.
//
// Independent of the game's Scaleform map — this draws whenever the overlay is active and the window is
// open (toggled from the Dev tab). Gamepad/keyboard nav works via the already-enabled ImGui nav.

#include "panel_internal.hpp"
#include "goblin_i18n.hpp"
#include "worldmap/marker_layer.hpp"   // Marker / MarkerLayer (overlay_layers → markers to project)
#include "worldmap/maptile.hpp"        // maptile::extract_named — ER map-tile ART (endgame phase-1a slice 2)
#include "goblin_virtual_world.hpp"    // vworld registry — the active custom world's markers (slice C)
#include "goblin_inject.hpp"           // goblin::world_map_open() — the game map-key trigger (slice D)

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace goblin::overlay::panel
{
using goblin::i18n::tr;

namespace
{
    // View state (world-space). cam = the world XZ at the canvas centre; zoom = pixels per world unit.
    // Persisted across frames; the whole map lives in ER world units (same space as marker world pos).
    bool s_open = false;
    float s_cam_x = 0.0f, s_cam_z = 0.0f;
    float s_zoom = 0.05f;  // 0.05 px/unit → ~10k-unit ER map spans ~525px; a sane default overview
    constexpr float kZoomMin = 0.002f, kZoomMax = 4.0f;
    // Which marker group to lay out on the canvas (slice B uses the live mod markers as test data;
    // slice C ties markers to a custom world). group = isDLC*2|isUG → 0/1/2/3.
    int s_group = 0;
    bool s_fit_requested = false;  // one-shot: on next draw, frame the selected group's markers
    int s_drawn = 0;               // marker count drawn last frame (toolbar readout)
    const char *const kGroupNames[4] = {"Base overworld", "Base underground", "DLC overworld",
                                        "DLC underground"};

    // ── ER map ART tiles (endgame phase-1a slice 2) ──────────────────────────────────────────────────
    // A loaded real-map tile: its GPU SRV (from create_tex_from_dds_mem) + the world-space quad it covers.
    // Drawn under the markers. slice 2 = prove ONE tile renders; slice 3 = stream all tiles + true grid
    // alignment (the SRV heap has a hard 256 cap with no recycle, so this is not "all tiles" yet).
    struct LoadedTile { unsigned long long srv = 0; int w = 0, h = 0; float wx0, wz0, wx1, wz1; std::string name; };
    std::vector<LoadedTile> s_tiles;
    bool s_tile_req = false;                 // a load is pending (serviced next draw, off ImGui geometry)
    std::string s_tile_needle;               // e.g. "M00_L0_00_00_00000000"
    float s_tile_rect[4] = {0, 0, 0, 0};     // explicit world quad; all-zero → auto-derive from col/row
    std::string s_tile_status;               // last load result (toolbar readout)

    // World quad for a tile from its name (M{MM}_L{L}_{col}_{row}_...): col/row are 2-hex-digit grid
    // indices; each cell is 256 units at L0. This is an APPROXIMATE placement (no origin/Z-flip/LOD-scale
    // yet — that is slice 3's projection work); it tiles neighbours correctly so the art is legible.
    void auto_rect_from_name(const std::string &needle, float out[4])
    {
        long col = 0, row = 0;
        size_t p = needle.find("_L");
        if (p != std::string::npos)
        {
            size_t c0 = needle.find('_', p + 2);              // after L#
            size_t r0 = (c0 == std::string::npos) ? std::string::npos : needle.find('_', c0 + 1);
            if (c0 != std::string::npos && r0 != std::string::npos)
            {
                col = std::strtol(needle.substr(c0 + 1, r0 - c0 - 1).c_str(), nullptr, 16);
                size_t r1 = needle.find('_', r0 + 1);
                std::string rs = (r1 == std::string::npos) ? needle.substr(r0 + 1) : needle.substr(r0 + 1, r1 - r0 - 1);
                row = std::strtol(rs.c_str(), nullptr, 16);
            }
        }
        out[0] = col * 256.0f;         out[1] = row * 256.0f;
        out[2] = out[0] + 256.0f;      out[3] = out[1] + 256.0f;
    }
}

bool &virtual_map_open() { return s_open; }
void virtual_map_request_fit() { s_fit_requested = true; }
void virtual_map_set_group(int g) { if (g >= 0 && g < 4) s_group = g; }
int virtual_map_group() { return s_group; }

// Request an ER map ART tile load (slice 2). `needle` selects the tile by name substring
// (e.g. "M00_L0_00_00_00000000"); if wx1<=wx0 the world quad is auto-derived from the tile's col/row.
// Serviced on the next draw (create_tex must run on the render thread, not the RPC thread) + opens the map.
void virtual_map_request_tile(const char *needle, float wx0, float wz0, float wx1, float wz1)
{
    s_tile_needle = needle ? needle : "";
    s_tile_rect[0] = wx0; s_tile_rect[1] = wz0; s_tile_rect[2] = wx1; s_tile_rect[3] = wz1;
    s_tile_req = true;
    s_open = true;
}
// Drop cached tiles. NB: the SRV handles are NOT recycled (create_tex_from_dds_mem has no free list yet),
// so this frees the CPU records but not the GPU descriptors — slice 3 adds SRV recycling.
void virtual_map_clear_tiles() { s_tiles.clear(); s_tile_status = "cleared"; }

void draw_virtual_map(const OverlayFrameCtx & /*ctx*/)
{
    // Slice D: open the virtual map with the game MAP KEY when a CUSTOM world is active (the production
    // "M, not F1" UX). On map-open edge with active world ≠ Base ER → open; on map-close, close it if WE
    // opened it (so a Dev-toggle-opened vmap isn't closed by the game map). Runs every frame (this entry is
    // called unconditionally, independent of the F1 panel).
    {
        static bool s_prev_map = false, s_from_map = false;
        const bool map_now = goblin::world_map_open();
        if (map_now && !s_prev_map && goblin::vworld::active() != 0) { s_open = true; s_from_map = true; }
        else if (!map_now && s_prev_map && s_from_map) { s_open = false; s_from_map = false; }
        s_prev_map = map_now;
    }
    if (!s_open) return;

    ImGui::SetNextWindowSize(ImVec2(720.0f, 560.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(470.0f, 40.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(tr("MapForGoblins \xE2\x80\x94 Virtual World Map (WIP)"), &s_open))
    {
        ImGui::End();
        return;
    }

    // Service a pending ER map-tile load (slice 2). Runs here on the render thread (create_tex_from_dds_mem
    // does its own submit_and_wait — must not run on the RPC thread). It's heavy but one-shot per request
    // (mirrors panel_dev_icons' on-click load); slice 3 replaces it with streamed, byte-range tile loads.
    if (s_tile_req)
    {
        s_tile_req = false;
        std::string tex; uint32_t tw = 0, th = 0;
        std::vector<uint8_t> dds = goblin::worldmap::maptile::extract_named("menu/71_MapTile", s_tile_needle, tex, &tw, &th);
        if (dds.empty())
            s_tile_status = "load FAIL: " + s_tile_needle;
        else
        {
            int gw = 0, gh = 0; DXGI_FORMAT gf;
            unsigned long long srv = create_tex_from_dds_mem(dds.data(), dds.size(), gw, gh, gf);
            if (!srv)
                s_tile_status = "SRV FAIL (cap? fmt?): " + s_tile_needle;
            else
            {
                float r[4] = {s_tile_rect[0], s_tile_rect[1], s_tile_rect[2], s_tile_rect[3]};
                if (r[2] <= r[0] || r[3] <= r[1]) auto_rect_from_name(s_tile_needle, r);
                s_tiles.push_back({srv, gw, gh, r[0], r[1], r[2], r[3], tex});
                s_tile_status = tex + " " + std::to_string(gw) + "x" + std::to_string(gh);
            }
        }
    }

    // Toolbar: reset view + group selector + fit-to-markers + readout.
    if (ImGui::SmallButton(tr("Reset view")))
    {
        s_cam_x = s_cam_z = 0.0f;
        s_zoom = 0.05f;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Fit"))) s_fit_requested = true;
    // World selector: "Base ER" (id 0 → live ER markers by group) or a custom virtual world (its own
    // markers). The active world is framework state (goblin::vworld), shared with the RPC.
    ImGui::SameLine();
    const int active_world = goblin::vworld::active();
    auto worlds = goblin::vworld::list();
    const char *active_name = "Base ER";
    for (auto &p : worlds)
        if (p.first == active_world) active_name = p.second.c_str();
    ImGui::SetNextItemWidth(150.0f);
    if (ImGui::BeginCombo(tr("world"), active_name))
    {
        for (auto &p : worlds)
            if (ImGui::Selectable(p.second.c_str(), p.first == active_world))
                goblin::vworld::set_active(p.first);
        ImGui::EndCombo();
    }
    // ER group selector — only meaningful for Base ER (a custom world has its own single marker set).
    if (active_world == 0)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(170.0f);
        if (ImGui::BeginCombo(tr("group"), tr(kGroupNames[s_group])))
        {
            for (int i = 0; i < 4; i++)
                if (ImGui::Selectable(tr(kGroupNames[i]), s_group == i)) s_group = i;
            ImGui::EndCombo();
        }
    }
    ImGui::TextDisabled(tr("drag = pan   wheel = zoom   |   centre (%.0f, %.0f)  zoom %.3f px/u   markers %d"),
                        s_cam_x, s_cam_z, s_zoom, s_drawn);
    if (!s_tiles.empty() || !s_tile_status.empty())
        ImGui::TextDisabled(tr("map tiles: %d loaded   |   last: %s"), (int)s_tiles.size(),
                            s_tile_status.c_str());

    // Canvas = the remaining content region. An InvisibleButton captures drag/scroll over exactly it.
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 32.0f) size.x = 32.0f;
    if (size.y < 32.0f) size.y = 32.0f;
    const ImVec2 canvas_end(origin.x + size.x, origin.y + size.y);
    const ImVec2 center(origin.x + size.x * 0.5f, origin.y + size.y * 0.5f);

    ImGui::InvisibleButton("##vmap_canvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    ImGuiIO &io = ImGui::GetIO();

    // world → screen and screen → world (mod-defined projection; Z maps to screen-Y, no flip needed for
    // a mod page — we own the convention). Keep them inverse so zoom-about-cursor is exact.
    auto w2s = [&](float wx, float wz) {
        return ImVec2(center.x + (wx - s_cam_x) * s_zoom, center.y + (wz - s_cam_z) * s_zoom);
    };
    auto s2w = [&](ImVec2 s, float &wx, float &wz) {
        wx = s_cam_x + (s.x - center.x) / s_zoom;
        wz = s_cam_z + (s.y - center.y) / s_zoom;
    };

    // Pan: dragging moves the camera opposite the mouse delta (in world units).
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
    {
        s_cam_x -= io.MouseDelta.x / s_zoom;
        s_cam_z -= io.MouseDelta.y / s_zoom;
    }
    // Zoom about the cursor: keep the world point under the mouse fixed across the zoom step.
    if (hovered && io.MouseWheel != 0.0f)
    {
        float wx_before, wz_before;
        s2w(io.MousePos, wx_before, wz_before);
        s_zoom *= std::pow(1.2f, io.MouseWheel);
        if (s_zoom < kZoomMin) s_zoom = kZoomMin;
        if (s_zoom > kZoomMax) s_zoom = kZoomMax;
        float wx_after, wz_after;
        s2w(io.MousePos, wx_after, wz_after);
        s_cam_x += wx_before - wx_after;  // re-centre so the cursor stays over the same world point
        s_cam_z += wz_before - wz_after;
    }

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, canvas_end, true);
    dl->AddRectFilled(origin, canvas_end, IM_COL32(18, 20, 26, 255));  // dark canvas backdrop

    // ER map ART tiles (slice 2): draw each loaded real-map tile at its world quad, under the grid+markers.
    for (const LoadedTile &t : s_tiles)
    {
        if (!t.srv) continue;
        ImVec2 a = w2s(t.wx0, t.wz0), b = w2s(t.wx1, t.wz1);
        dl->AddImage((ImTextureID)t.srv, a, b);
        dl->AddRect(a, b, IM_COL32(90, 160, 220, 120));  // faint frame so an empty/black tile is still visible
    }

    // Reference grid: pick a world spacing that renders ~80px on screen, snapped to a 1/2/5×10^n step,
    // so grid density stays readable across the whole zoom range.
    const float target_px = 80.0f;
    float raw = target_px / s_zoom;                 // world units per ~80px
    float mag = std::pow(10.0f, std::floor(std::log10(raw)));
    float norm = raw / mag;
    float step = (norm < 1.5f ? 1.0f : norm < 3.5f ? 2.0f : norm < 7.5f ? 5.0f : 10.0f) * mag;

    float wx0, wz0, wx1, wz1;
    s2w(origin, wx0, wz0);
    s2w(canvas_end, wx1, wz1);
    const ImU32 grid_col = IM_COL32(60, 66, 78, 255);
    const ImU32 axis_col = IM_COL32(120, 130, 150, 255);
    for (float gx = std::ceil(wx0 / step) * step; gx <= wx1; gx += step)
    {
        ImVec2 a = w2s(gx, wz0), b = w2s(gx, wz1);
        dl->AddLine(ImVec2(a.x, origin.y), ImVec2(a.x, canvas_end.y),
                    std::fabs(gx) < step * 0.5f ? axis_col : grid_col);
    }
    for (float gz = std::ceil(wz0 / step) * step; gz <= wz1; gz += step)
    {
        ImVec2 a = w2s(wx0, gz), b = w2s(wx1, gz);
        dl->AddLine(ImVec2(origin.x, a.y), ImVec2(canvas_end.x, a.y),
                    std::fabs(gz) < step * 0.5f ? axis_col : grid_col);
    }

    // Markers: project the ACTIVE world's markers onto the canvas + accumulate a bbox for Fit. A colored
    // dot per marker (on-canvas icons are a follow-up). Base ER (world 0) → the live ER markers of the
    // selected group (slice B); a custom world → its OWN markers in its own coordinate namespace (slice C).
    int drawn = 0;
    float minx = 1e30f, minz = 1e30f, maxx = -1e30f, maxz = -1e30f;
    auto plot = [&](float wx, float wz, uint32_t col) {
        if (wx < minx) minx = wx;
        if (wx > maxx) maxx = wx;
        if (wz < minz) minz = wz;
        if (wz > maxz) maxz = wz;
        ImVec2 ps = w2s(wx, wz);
        if (ps.x < origin.x || ps.x > canvas_end.x || ps.y < origin.y || ps.y > canvas_end.y) return;
        dl->AddCircleFilled(ps, 3.0f, col ? col : IM_COL32(235, 130, 90, 255));
        drawn++;
    };
    if (active_world == 0)
    {
        for (auto *L : overlay_layers())
        {
            if (!L) continue;
            for (const goblin::worldmap::Marker &m : L->markers())
                if (m.group == s_group) plot(m.worldX, m.worldZ, m.color);
        }
    }
    else
    {
        goblin::vworld::World w;
        if (goblin::vworld::get_world(active_world, w))
            for (const goblin::vworld::Marker &m : w.markers)
                plot(m.x + w.originX, m.z + w.originZ, m.color);  // C1: originX/Z default 0 (identity)
    }
    s_drawn = drawn;
    // Fit: frame the selected group's bbox (cam/zoom feed w2s next frame → 1-frame settle).
    if (s_fit_requested && maxx > minx && maxz > minz)
    {
        s_cam_x = (minx + maxx) * 0.5f;
        s_cam_z = (minz + maxz) * 0.5f;
        float zx = size.x * 0.9f / (maxx - minx), zz = size.y * 0.9f / (maxz - minz);
        s_zoom = zx < zz ? zx : zz;
        if (s_zoom < kZoomMin) s_zoom = kZoomMin;
        if (s_zoom > kZoomMax) s_zoom = kZoomMax;
    }
    s_fit_requested = false;

    // World origin cross (0,0) + a small label, so the empty canvas is legible.
    ImVec2 o = w2s(0.0f, 0.0f);
    if (o.x >= origin.x && o.x <= canvas_end.x && o.y >= origin.y && o.y <= canvas_end.y)
    {
        dl->AddCircleFilled(o, 4.0f, IM_COL32(220, 180, 90, 255));
        dl->AddText(ImVec2(o.x + 6, o.y + 4), IM_COL32(220, 180, 90, 255), "0,0");
    }
    // Grid-step legend (bottom-left) so the scale is readable.
    char legend[64];
    std::snprintf(legend, sizeof(legend), "%s: %.0f u", tr("grid"), step);
    dl->AddText(ImVec2(origin.x + 6, canvas_end.y - 18), IM_COL32(150, 158, 172, 255), legend);

    // TODO(slice B): project the mod's markers for the virtual world's group here (w2s per marker + the
    // marker icon draw), then (slice C) tag markers to a synthetic group / bundle-backed custom world.
    dl->PopClipRect();

    ImGui::End();
}
} // namespace goblin::overlay::panel
