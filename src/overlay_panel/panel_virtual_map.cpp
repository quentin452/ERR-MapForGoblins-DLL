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
#include "worldmap/maptile.hpp"        // maptile ART reader (endgame phase-1a slice 2/3)
#include "goblin_worldmap_probe.hpp"   // live converter affine + map-space extent (slice 3 tile placement)
#include "goblin_virtual_world.hpp"    // vworld registry — the active custom world's markers (slice C)
#include "goblin_inject.hpp"           // goblin::world_map_open() — the game map-key trigger (slice D)

#include <spdlog/spdlog.h>

#include <algorithm>
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

// Dev/test: set the canvas view directly (world-space cam centre + zoom px/unit). Lets a driver frame a
// region for a screenshot without the marker-bbox Fit. zoom<=0 leaves zoom unchanged.
void virtual_map_set_view(float camX, float camZ, float zoom)
{
    s_cam_x = camX; s_cam_z = camZ;
    if (zoom > 0.0f) s_zoom = (zoom < kZoomMin ? kZoomMin : (zoom > kZoomMax ? kZoomMax : zoom));
    s_open = true;
}

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

// Load a whole ER map dimension+LOD onto the canvas (slice 3). The tile grid is decoded per the SOLVED
// WorldMapTile findings (docs/re/windows_worldmap_tile_placement_re_findings.md): a UNIFORM 256-cell grid,
// name M{dim}_L{lod}_{col}_{row}_{suffix} with col/row DECIMAL, suffix = 8*morton(subX,subY) in a 64-cell
// block → gridX = col*64+subX, gridZ = row*64+subY. Each tile is exactly one 256-unit world cell. Placement
// into the vmap WORLD frame is derived LIVE + robustly from the markers (no hardcoded origin — see
// procedural_map_derivation_design.md): worldX = 256*gridX + medianOf(worldX - px - 256*raw_gx) over
// overworld markers (slope = the findings' CELLSIZE 256; median is immune to folded dungeon/DLC outliers).
// Reads the archive ONCE; loads up to `cap` tiles nearest the grid centre (SRV heap has a 256 cap, no
// recycle yet). dim 0=overworld/1=underground/10=DLC/11=DLC-ug; area 60 = overworld grid frame.
std::string virtual_map_load_lod(int dim, int lod, int cap)
{
    // 1) LIVE placement transform (no hardcoding — procedural_map_derivation_design.md):
    //   (a) map-space→vmap-world from the engine projection: worldX = projU + bx, worldZ = -projV + bz,
    //       bx/bz = robust MEDIAN over overworld markers (proven exact: bx=7040, bz=16512).
    //   (b) converter grid bases gridXbase/gridZbase — the findings tile map-space is (gridX-gridXbase)*256.
    //   Tile world = (gridX-gridXbase)*256 + bx (X) ; -(gridZ-gridZbase)*256 + bz (Z, converter Z-flip).
    // Both LIVE; needs the ER overworld map OPEN (project + converter VM).
    std::vector<float> dX, dZ, mwx, mwz;
    for (auto *L : overlay_layers())
    {
        if (!L) continue;
        for (const goblin::worldmap::Marker &m : L->markers())
        {
            if (m.group != 0 || m.raw_area < 0) continue;
            float u = 0, v = 0; int pg = -1;
            if (!goblin::worldmap_probe::project(m.raw_area, m.raw_gx, m.raw_gz, m.raw_px, m.raw_pz, u, v, pg) ||
                pg != 0)
                continue;
            dX.push_back(m.worldX - u); dZ.push_back(m.worldZ + v);
            mwx.push_back(m.worldX); mwz.push_back(m.worldZ);
        }
    }
    if (dX.size() < 8)
        return (s_tile_status = "not enough live-projected overworld markers (" + std::to_string(dX.size()) +
                                ") — open the ER map AND MOVE it (pan/zoom), then retry");
    auto median = [](std::vector<float> &v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
    const float bx = median(dX), bz = median(dZ);
    const float mcx = median(mwx), mcz = median(mwz);   // marker world median = the landmass (load-centre)
    goblin::worldmap_probe::ConvAffine aff;
    if (!goblin::worldmap_probe::get_converter_affine(60, aff))
        return (s_tile_status = "converter grid-bases unavailable — open the ER overworld map + move it");
    const int gridXbase = aff.gridXbase, gridZbase = aff.gridZbase;
    // marker-median mapped to tile grid = the load centre (the LANDMASS, not the grid's ocean centre).
    const float cgx = (mcx - bx) / 256.0f + gridXbase, cgz = (bz - mcz) / 256.0f + gridZbase;

    // 2) archive + enumerate this dim/LOD's tiles with the SOLVED decode.
    char prefix[16];
    std::snprintf(prefix, sizeof(prefix), "M%02d_L%d_", dim, lod);
    std::vector<goblin::worldmap::maptile::Entry> entries;
    std::vector<uint8_t> bdt;
    if (!goblin::worldmap::maptile::load_archive("menu/71_MapTile", entries, bdt))
        return (s_tile_status = "maptile archive unavailable");

    auto decode_suffix = [](uint32_t suffix, int &subX, int &subY) {
        uint32_t m = suffix >> 3;   // low 3 bits reserved (suffix = 8*morton)
        subX = subY = 0;
        for (int i = 0; i < 8; ++i) { subX |= ((m >> (2 * i)) & 1u) << i; subY |= ((m >> (2 * i + 1)) & 1u) << i; }
    };
    struct Tile { const goblin::worldmap::maptile::Entry *e; int gx, gz; float d2; };
    std::vector<Tile> tiles;
    for (const auto &e : entries)
    {
        size_t p = e.name.find(prefix);
        if (p == std::string::npos) continue;
        size_t c0 = p + std::strlen(prefix);
        size_t c1 = e.name.find('_', c0);          // after col
        size_t r1 = (c1 == std::string::npos) ? std::string::npos : e.name.find('_', c1 + 1);  // after row
        size_t s1 = (r1 == std::string::npos) ? std::string::npos : e.name.find('.', r1 + 1);  // ".tpf"
        if (c1 == std::string::npos || r1 == std::string::npos || s1 == std::string::npos) continue;
        long col = std::strtol(e.name.substr(c0, c1 - c0).c_str(), nullptr, 10);   // col/row DECIMAL (%02d)
        long row = std::strtol(e.name.substr(c1 + 1, r1 - c1 - 1).c_str(), nullptr, 10);
        uint32_t suf = (uint32_t)std::strtoul(e.name.substr(r1 + 1, s1 - r1 - 1).c_str(), nullptr, 16);
        int sx, sz; decode_suffix(suf, sx, sz);
        tiles.push_back({&e, (int)col * 64 + sx, (int)row * 64 + sz, 0.0f});   // 64-cell blocks
    }
    if (tiles.empty())
        return (s_tile_status = std::string("no tiles for ") + prefix);

    int minGX = tiles[0].gx, maxGX = tiles[0].gx, minGZ = tiles[0].gz, maxGZ = tiles[0].gz;
    for (const auto &t : tiles)
    {
        minGX = (std::min)(minGX, t.gx); maxGX = (std::max)(maxGX, t.gx);
        minGZ = (std::min)(minGZ, t.gz); maxGZ = (std::max)(maxGZ, t.gz);
    }
    // Center the capped load on the landmass (marker-median mapped to grid), not the grid's ocean centre.
    for (auto &t : tiles) { float dx = t.gx - cgx, dz = t.gz - cgz; t.d2 = dx * dx + dz * dz; }
    std::sort(tiles.begin(), tiles.end(), [](const Tile &a, const Tile &b) { return a.d2 < b.d2; });

    // ⚠ KNOWN GAP (2026-07-04): the tile NAME-grid does NOT map to the converter grid by gridXbase — live
    // data shows land tiles at name-gridX~64 while the converter grid puts that land at ~43 (offset ~21 in X,
    // ~14 in Z — NON-uniform), so this places the map with a per-axis offset. The tile→world ORIGIN is
    // computed by the WorldMapTile REGION-WALK (FUN_1409d8c30 / FUN_1409da9f0, "origin passed down"), which
    // the placement findings identified but did NOT decode. That region-walk origin is the missing anchor —
    // see docs/re/windows_worldmap_tile_placement_re_findings.md §4 follow-up. Until then the art loads but
    // is offset from the markers.
    auto tile_world = [&](int gx, int gz, float &wx0, float &wz0) {
        wx0 = float((gx - gridXbase) * 256) + bx;
        wz0 = -float((gz - gridZbase) * 256) + bz;   // Z flipped (converter)
    };
    virtual_map_clear_tiles();
    int loaded = 0, failed = 0;
    for (const Tile &t : tiles)
    {
        if (loaded >= cap) break;
        std::string tex; uint32_t tw = 0, th = 0;
        std::vector<uint8_t> dds = goblin::worldmap::maptile::extract_dds(bdt, *t.e, tex, &tw, &th);
        if (dds.empty()) { ++failed; continue; }
        int gw = 0, gh = 0; DXGI_FORMAT gf;
        unsigned long long srv = create_tex_from_dds_mem(dds.data(), dds.size(), gw, gh, gf);
        if (!srv) { s_tile_status = "SRV cap hit at " + std::to_string(loaded) + " tiles"; break; }
        float wxA, wzA, wxB, wzB;
        tile_world(t.gx, t.gz, wxA, wzA);
        tile_world(t.gx + 1, t.gz + 1, wxB, wzB);
        LoadedTile lt;
        lt.srv = srv; lt.w = gw; lt.h = gh; lt.name = tex;
        lt.wx0 = (std::min)(wxA, wxB); lt.wx1 = (std::max)(wxA, wxB);
        lt.wz0 = (std::min)(wzA, wzB); lt.wz1 = (std::max)(wzA, wzB);
        s_tiles.push_back(lt);
        ++loaded;
    }
    s_open = true;
    spdlog::info("[MAPTILE3] bx={:.0f} bz={:.0f} base=({},{}) markerMed=({:.0f},{:.0f}) loadCtrGrid=({:.0f},{:.0f}) "
                 "| grid gx[{},{}] gz[{},{}] | first tile world x[{:.0f},{:.0f}] z[{:.0f},{:.0f}]",
                 bx, bz, gridXbase, gridZbase, mcx, mcz, cgx, cgz, minGX, maxGX, minGZ, maxGZ,
                 s_tiles.empty() ? 0.f : s_tiles[0].wx0, s_tiles.empty() ? 0.f : s_tiles[0].wx1,
                 s_tiles.empty() ? 0.f : s_tiles[0].wz0, s_tiles.empty() ? 0.f : s_tiles[0].wz1);
    char st[176];
    std::snprintf(st, sizeof(st), "%s %d/%d tiles (grid %dx%d, cap %d, failed %d)", prefix, loaded,
                  (int)tiles.size(), maxGX - minGX + 1, maxGZ - minGZ + 1, cap, failed);
    return (s_tile_status = st);
}

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
    // Load ER's real map ART tiles (WIP — see below). Needs the game map OPEN + you moving on it a
    // moment (so the projection resolves). Loads overworld coarse tiles around the marker area.
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Load ER map tiles"))) virtual_map_load_lod(0, 3, 240);
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Clear tiles"))) virtual_map_clear_tiles();
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
