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
#include "goblin_overlay_render_api.hpp"  // lookup_text_utf8 / category_label / warp_to_grace
#include "goblin_map_data.hpp"            // generated::Category::WorldGraces (click-to-warp)
#include "worldmap/marker_layer.hpp"   // Marker / MarkerLayer (overlay_layers → markers to project)
#include "worldmap/map_renderer.hpp"   // draw_marker_glyph — reuse the native state-aware per-marker draw
#include "goblin_config.hpp"           // config::clusterSpiderfy — hover fan-out gate (spiderfy)
#include "worldmap/maptile.hpp"        // maptile ART reader (endgame phase-1a slice 2/3)
#include "goblin_worldmap_probe.hpp"   // live converter affine + map-space extent (slice 3 tile placement)
#include "goblin_virtual_world.hpp"    // vworld registry — the active custom world's markers (slice C)
#include "goblin_inject.hpp"           // goblin::world_map_open() + marker_group_from (slice D / A7)
#include "goblin_major_regions.hpp"    // MAJOR_REGION_ANCHORS — coarse region name labels (A7 parity)
#include "goblin_bench.hpp"            // GOBLIN_BENCH_QUIET — profile the vmap draw at high marker counts
#include "goblin_custom_markers.hpp"   // shared player-placed marker store (vmap + minimap read it)
#include "overlay_panel/marker_quadtree.hpp"  // spatial index: viewport cull + LOD clustering (perf)

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
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
    // Orientation calibration (dev): sign per axis for world→screen. Minimap convention = +X, -Z
    // (north-up), so default sx=+1, sz=-1. `vmap flip` toggles these live to match the native map.
    float s_sx = 1.0f, s_sz = -1.0f;
    constexpr float kZoomMin = 0.002f, kZoomMax = 4.0f;
    // Which marker group to lay out on the canvas (slice B uses the live mod markers as test data;
    // slice C ties markers to a custom world). group = isDLC*2|isUG → 0/1/2/3.
    int s_group = 0;
    bool s_show_icons = true;      // draw real category icons (native/atlas) vs plain colour dots
    bool s_show_labels = true;     // draw coarse region name labels (A7 parity — Limgrave, Caelid, …)
    bool s_show_relief = true;      // draw the heightfield hillshade backdrop (D2.3)
    bool s_show_graces = false;     // Track B: the grace warp-menu sidebar
    bool s_show_markers_panel = false;  // F1→vmap port: the Sections & categories controls in a sidebar
    Filter s_markers_filter;            // default (not filtering) → draw_sections_categories shows all
    float s_markers_w = 320.0f, s_graces_w = 250.0f;  // user-resizable sidebar widths (drag the splitter)
    // Custom player-placed markers live in the shared goblin::custom_markers store (so the minimap draws
    // them too). Right-click the canvas near empty space to drop one; near an existing pin to delete it.
    bool s_show_custom = false;        // custom-marker list sidebar toggle
    float s_custom_w = 250.0f;         // its resizable width
    bool s_show_item_search = false;   // A9: item-search sidebar (locate items ON the vmap, no native map)
    float s_items_w = 260.0f;          // its resizable width
    char s_item_filter[64] = "";       // file-scope so a dev RPC (vmap items <q>) can drive it for tests
    int s_custom_seq = 1;              // auto-name counter
    char s_grace_filter[64] = "";   // grace-list search box
    // Grace list cache (rebuilt on open / Refresh; discovered-state re-read live per visible row).
    struct GraceRow { std::string name; float wx, wz; uint64_t rowId; int discFlag; int group;
                      std::string region; int regionIdx; };
    std::vector<GraceRow> s_graces;
    bool s_graces_built = false;
    // Marker spatial index (perf): viewport-cull + LOD-cluster the ~6837 base markers instead of the
    // O(n)-every-frame loop. Rebuilt when the displayed group changes (markers themselves are static).
    MarkerQuadtree s_qt;
    std::vector<const goblin::worldmap::Marker *> s_grace_pts;  // group's graces (drawn on top, not clustered)
    int s_qt_group = -999;
    uint32_t s_qt_vis_gen = 0xffffffffu;  // visibility_generation() the index was built for → rebuild on toggle
    uint32_t s_qt_region_mask = 0xffffffffu;  // region-enabled bitmask the index was built for → rebuild on a
                                              // region-name toggle (region_set_enabled doesn't bump vis_gen)
    // Live-projected marker COPIES the vmap actually clusters/draws (underground/DLC markers are re-projected
    // through the engine converter → their baked worldX/worldZ is wrong; see the QT build). The QT + graces
    // point into this, so it must outlive them (static, rebuilt with the tree).
    std::vector<goblin::worldmap::Marker> s_vmarkers;
    bool s_qt_proj_incomplete = false;   // a UG/DLC marker couldn't live-project (converter not up) → retry
    int  s_qt_proj_retries = 0;          // bounded rebuild retries while the converter warms up
    bool s_force_spiderfy = false;       // dev/test: force-open the spiderfy fan on the largest pile
    bool s_fan_open = false;             // spiderfy: a cluster's hover-fan is open
    uint64_t s_fan_key = 0;              // world-quantized identity of the open fan (sticky across rebuilds)
    // Quantized world-space identity for a spiderfy cluster (tag 1 = pile, 2 = coincident singles).
    inline uint64_t spiderfy_key(float wx, float wz, uint64_t tag)
    {
        uint64_t qx = (uint64_t)(int64_t)std::floor(wx / 8.0f) & 0xffffffffu;
        uint64_t qz = (uint64_t)(int64_t)std::floor(wz / 8.0f) & 0xffffffffu;
        return (tag << 62) ^ (qx << 30) ^ qz;
    }
    uint64_t s_warp_pending = 0;   // grace rowId to warp to; serviced at the next frame's top (not mid-draw)
    int s_warp_offset = 0;          // added to the grace entity id before LuaWarp; 0 = entity id direct (ground truth; CT's -1000 was wrong)
    bool s_fit_requested = false;  // one-shot: on next draw, frame the selected group's markers
    bool s_focus_player = false;   // one-shot: on next draw, centre the camera on the player + their group
    bool s_follow_player_dim = true;  // auto-switch the vmap PAGE to the player's dimension on a crossing (base ER)
    int  s_player_group_prev = -1;    // last observed player dimension group (edge-detect the crossing)
    // Item-search LOCATE highlight (bugs: search only centred, no visual; and with markers toggled off a
    // locate showed nothing). virtual_map_locate() fills s_locate_pts (world XZ of every match on the page)
    // + arms s_locate_arm; the draw stamps s_locate_until and pulses a ring at each, ON TOP and INDEPENDENT
    // of the marker-visibility gate — so the hit is visible even when all markers are hidden. Parity with
    // the native map's search ring.
    std::vector<ImVec2> s_locate_pts;   // (worldX, worldZ) of the last locate's matches
    bool s_locate_arm = false;          // set by locate; consumed in draw (GetTime() is frame-safe there)
    double s_locate_until = 0.0;        // ImGui::GetTime() expiry of the pulse (0 = inactive)

    // Item-search "mark all results": a persistent highlight on EVERY matching item instance of the current
    // query (distinct orange, not the blue custom pins), capped at s_search_mark_max, regenerated on each new
    // search + wiped by Clear. Stored in the SHARED goblin::search_marks store so the MINIMAP draws them too
    // (like the blue custom markers). Only the UI knobs are vmap-local.
    int s_search_mark_max = 150;
    bool s_search_mark_on = true;
    int s_drawn = 0;               // marker count drawn last frame (toolbar readout)
    int s_relief_hits = 0, s_relief_drawn = 0; // D2.3 debug: hit cells fetched / quads actually drawn
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

// True when the vmap is showing the ported "Sections & categories" sidebar → the F1 window should NOT
// also draw its copy (avoids the two-overlapping-panels + toggle confusion; single source of truth).
bool markers_panel_open() { return s_open && s_show_markers_panel; }

// Service a grace warp queued by a vmap double-click. Called POST-FRAME (after ImGui::Render, next to
// debug_rpc::pump) — running warp mid-ImGui-draw freezes the loading screen; the RPC path works because it
// executes here. Present thread. No-op unless a warp is pending.
void virtual_map_service_pending_warp()
{
    if (!s_warp_pending) return;
    uint64_t gid = s_warp_pending; s_warp_pending = 0;
    bool ok = goblin::overlay_api::warp_to_grace((int32_t)gid, s_warp_offset);
    s_tile_status = ok ? ("warped to grace " + std::to_string(gid))
                       : ("warp failed " + std::to_string(gid));
}
void virtual_map_request_fit() { s_fit_requested = true; }
// Recenter on the player + switch to the player's map page, next draw. Call from EVERY open path (the
// on-open edge only fires if draw runs while closed; the F1 toggle/dev button must request it explicitly).
void virtual_map_request_focus() { s_focus_player = true; }
void virtual_map_set_group(int g) { if (g >= 0 && g < 4) s_group = g; }
int virtual_map_group() { return s_group; }

// Dev/test: force-open the spiderfy hover-fan on the largest visible pile (precise headless hover is
// unreliable). Screenshot-verify the fan geometry. No effect in normal use (off).
void virtual_map_force_spiderfy(bool on) { s_force_spiderfy = on; s_open = true; }

// Dev/test: toggle the terrain-relief hillshade backdrop (near raycast + D-far -1 cloud) so a headless
// driver can A/B screenshot cloud-relief vs the native map ART tile at the same framing.
void virtual_map_set_relief(bool on) { s_show_relief = on; s_open = true; }

// A9 dev/test: open the vmap item-search sidebar and set its query (drives the same list the UI builds).
// Lets a headless driver populate + screenshot the search without ImGui clicks. Empty query just opens it.
void virtual_map_item_search(const char *query)
{
    s_show_item_search = true;
    std::snprintf(s_item_filter, sizeof(s_item_filter), "%s", query ? query : "");
    s_open = true;
}

// A9: locate an item-search hit on the vmap. Centre the canvas on the centroid of every Base-ER marker
// with this name on this page (group&3), switch to that page, zoom in if we were far out, and open the
// vmap. Returns the instance count (0 = no such marker on that page → no-op, doesn't open). Mirrors the
// native map's click-to-locate so item search works with the vmap as the surface, not just the ER map.
int virtual_map_locate(int32_t name_id, int group)
{
    const int pg = group & 3;
    float sx = 0.0f, sz = 0.0f; int n = 0;
    s_locate_pts.clear();
    for (auto *L : overlay_layers())
    {
        if (!L) continue;
        for (const goblin::worldmap::Marker &m : L->markers())
        {
            if (m.name_id != name_id || (m.group & 3) != pg) continue;
            sx += m.worldX; sz += m.worldZ; ++n;
            s_locate_pts.push_back(ImVec2(m.worldX, m.worldZ));  // pulse a ring at each (drawn regardless of toggles)
        }
    }
    if (n == 0) { s_locate_pts.clear(); return 0; }
    s_cam_x = sx / n; s_cam_z = sz / n;
    s_group = pg;
    if (s_zoom < 0.25f) s_zoom = 0.25f;   // frame the hit if the view was zoomed far out
    s_locate_arm = true;                  // draw stamps the pulse-timer this frame
    s_open = true;
    return n;
}

// TRIAGE the markers the vmap drops as "hors map": same predicate as the QT build — origin-zero (0,0) or
// wildly out-of-frame (|coord| > 40000). Reports WHICH objects + WHICH ER map (raw area) + category + reason
// to the [OFFMAP] log, so a bad source/projection can be tracked to its dimension. Reads the marker layers'
// STORED worldX/worldZ (marker_world_pos, the build projection) — so overworld is exact; UG/DLC are the fold
// (the vmap re-projects those via the live converter at draw, so a few UG/DLC edge cases can differ). RPC.
std::string virtual_map_offmap_probe()
{
    auto implausible = [](float x, float z) {
        return (x == 0.f && z == 0.f) || x < -40000.f || x > 40000.f || z < -40000.f || z > 40000.f;
    };
    size_t total = 0, off = 0, zero = 0, oob = 0;
    std::map<int, int> byArea, byCat;   // raw ER area -> count, category -> count
    std::vector<std::string> samples;
    for (auto *L : overlay_layers())
    {
        if (!L) continue;
        for (const goblin::worldmap::Marker &m : L->markers())
        {
            ++total;
            if (!implausible(m.worldX, m.worldZ)) continue;
            ++off;
            const bool z0 = (m.worldX == 0.f && m.worldZ == 0.f);
            if (z0) ++zero; else ++oob;
            byArea[m.raw_area]++; byCat[m.category]++;
            if (samples.size() < 25)
            {
                std::string nm = m.name_id >= 0 ? goblin::overlay_api::lookup_text_utf8(m.name_id) : std::string();
                const char *cl = goblin::overlay_api::category_label(m.category);
                char b[192];
                std::snprintf(b, sizeof(b), "'%s' area%d grid(%d,%d) [%s] w(%.0f,%.0f) %s",
                              nm.c_str(), m.raw_area, m.raw_gx, m.raw_gz, cl ? cl : "?",
                              m.worldX, m.worldZ, z0 ? "ZERO" : "OOB");
                samples.push_back(b);
            }
        }
    }
    spdlog::info("[OFFMAP] {} off-map of {} markers ({} origin-zero, {} out-of-range)", off, total, zero, oob);
    { std::string s; for (auto &kv : byArea) { char b[24]; std::snprintf(b, sizeof(b), " area%d:%d", kv.first, kv.second); s += b; }
      spdlog::info("[OFFMAP] by raw ER area (which map):{}", s.empty() ? " (none)" : s.c_str()); }
    { std::string s; for (auto &kv : byCat) { char b[32]; const char *cl = goblin::overlay_api::category_label(kv.first);
        std::snprintf(b, sizeof(b), " %s:%d", cl ? cl : "?", kv.second); s += b; }
      spdlog::info("[OFFMAP] by category:{}", s.empty() ? " (none)" : s.c_str()); }
    for (auto &s : samples) spdlog::info("[OFFMAP]   {}", s);
    char out[192];
    std::snprintf(out, sizeof(out), "ok offmap: %zu off-map of %zu markers (%zu origin-zero, %zu out-of-range) — see [OFFMAP] log",
                  off, total, zero, oob);
    return out;
}

// Dev orientation calibration: set world→screen axis signs. flipX/flipZ toggle each axis relative to the
// minimap default (+X, -Z). Lets us match the native map live without a rebuild per guess.
void virtual_map_set_flip(bool flipX, bool flipZ)
{
    s_sx = flipX ? -1.0f : 1.0f;
    s_sz = flipZ ? 1.0f : -1.0f;   // default (no flip) = -Z (minimap north-up)
    s_open = true;
}

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

// Dump every live marker to a CSV (for OFFLINE procedural-map style prototyping — no game needed after).
// Columns: group,category,srcArea,color,worldX,worldZ. category = int(Category) (name table lives in the
// generated enum). Returns marker count, -1 on file error.
int dump_markers_csv(const char *path)
{
    std::ofstream f(path);
    if (!f) return -1;
    f << "group,category,srcArea,color,worldX,worldZ,name_id\n";
    int n = 0;
    for (auto *L : overlay_layers())
    {
        if (!L) continue;
        for (const goblin::worldmap::Marker &m : L->markers())
        {
            f << m.group << ',' << m.category << ',' << m.srcArea << ',' << std::hex << m.color << std::dec
              << ',' << m.worldX << ',' << m.worldZ << ',' << m.name_id << '\n';
            ++n;
        }
    }
    return n;
}

// Harvest the LIVE resident WorldMapTile rects (position only — textures deferred) and draw them as
// outlined cells at the engine's OWN positions (region-walk applied). This is the authoritative alignment:
// the engine already positioned these, so they overlay the markers exactly. Confirms the calibration + is
// the base for resident-texture draw later. Needs the ER map OPEN + moved (cursor published). map-space→
// world via the proven marker-projection fit (worldX=u+bx, worldZ=-v+bz).
std::string virtual_map_load_resident()
{
    // map-space→world fit from live markers (proven bx=7040, bz=16512).
    std::vector<float> dX, dZ;
    for (auto *L : overlay_layers())
    {
        if (!L) continue;
        for (const goblin::worldmap::Marker &m : L->markers())
        {
            if (m.group != 0 || m.raw_area < 0) continue;
            float u = 0, v = 0; int pg = -1;
            if (goblin::worldmap_probe::project(m.raw_area, m.raw_gx, m.raw_gz, m.raw_px, m.raw_pz, u, v, pg) && pg == 0)
            { dX.push_back(m.worldX - u); dZ.push_back(m.worldZ + v); }
        }
    }
    if (dX.size() < 8)
        return (s_tile_status = "open the ER map + move it (need markers projected) then retry");
    auto median = [](std::vector<float> &v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
    const float bx = median(dX), bz = median(dZ);

    std::vector<goblin::worldmap_probe::ResidentTileRect> rt;
    int n = goblin::worldmap_probe::harvest_resident_tiles(rt);
    if (n == 0)
        return (s_tile_status = "no resident tiles (open the ER map + move it, then retry)");
    virtual_map_clear_tiles();
    for (const auto &t : rt)
    {
        LoadedTile lt;
        lt.srv = 0;  // no texture yet (position-only harvest); drawn as an outline cell
        lt.name = "cell";
        float wxA = t.u0 + bx, wxB = t.u1 + bx;
        float wzA = -t.v0 + bz, wzB = -t.v1 + bz;
        lt.wx0 = (std::min)(wxA, wxB); lt.wx1 = (std::max)(wxA, wxB);
        lt.wz0 = (std::min)(wzA, wzB); lt.wz1 = (std::max)(wzA, wzB);
        s_tiles.push_back(lt);
    }
    s_open = true;
    char st[96];
    std::snprintf(st, sizeof(st), "resident: %d tile cells (position-only, outlines)", n);
    return (s_tile_status = st);
}

// A3 recon: correlate the LIVE resident tile grid (authoritative rects) with the 71_MapTile archive
// name-grid. Decides whether archive-name↔runtime-cell is a clean formula (⇒ textured placement is a
// quick fix) or needs the deferred texture-fetch RE. Logs [TILERECON]; read-only. Map must be open+moved.
std::string virtual_map_tile_recon()
{
    std::vector<goblin::worldmap_probe::ResidentTileRect> rt;
    int n = goblin::worldmap_probe::harvest_resident_tiles(rt);
    if (n == 0)
        return (s_tile_status = "recon: no resident tiles (open the ER map + MOVE it, then retry)");

    // Per-dim resident grid extent + rect extent (the authoritative runtime grid).
    struct Ext { int minGX = 1 << 30, maxGX = -(1 << 30), minGZ = 1 << 30, maxGZ = -(1 << 30), count = 0;
                 float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f; };
    std::map<int, Ext> byDim;
    for (const auto &t : rt)
    {
        Ext &e = byDim[t.dim];
        e.minGX = (std::min)(e.minGX, t.gridX); e.maxGX = (std::max)(e.maxGX, t.gridX);
        e.minGZ = (std::min)(e.minGZ, t.gridZ); e.maxGZ = (std::max)(e.maxGZ, t.gridZ);
        e.u0 = (std::min)(e.u0, t.u0); e.u1 = (std::max)(e.u1, t.u1);
        e.v0 = (std::min)(e.v0, t.v0); e.v1 = (std::max)(e.v1, t.v1);
        ++e.count;
    }
    spdlog::info("[TILERECON] === {} resident tiles, {} distinct dim(s) ===", n, (int)byDim.size());
    for (const auto &[dim, e] : byDim)
        spdlog::info("[TILERECON] resident dim={} count={} gridX[{}..{}] gridZ[{}..{}] mapRect u[{:.0f}..{:.0f}] v[{:.0f}..{:.0f}]",
                     dim, e.count, e.minGX, e.maxGX, e.minGZ, e.maxGZ, e.u0, e.u1, e.v0, e.v1);
    // A few raw resident tiles (id→rect ground truth) for offline correlation.
    for (int i = 0; i < (int)rt.size() && i < 12; ++i)
        spdlog::info("[TILERECON] resident[{}] dim={} gx={} gz={} rect=({:.0f},{:.0f},{:.0f},{:.0f})",
                     i, rt[i].dim, rt[i].gridX, rt[i].gridZ, rt[i].u0, rt[i].v0, rt[i].u1, rt[i].v1);

    // Archive name-grid per LOD for each resident dim (archive prefix M{dim:%02d}).
    std::vector<goblin::worldmap::maptile::Entry> entries;
    std::vector<uint8_t> bdt;
    if (!goblin::worldmap::maptile::load_archive("menu/71_MapTile", entries, bdt))
        return (s_tile_status = "recon: maptile archive unavailable (resident logged though)");
    auto decode_suffix = [](uint32_t suffix, int &subX, int &subY) {
        uint32_t m = suffix >> 3;   // low 3 bits reserved (suffix = 8*morton)
        subX = subY = 0;
        for (int i = 0; i < 8; ++i) { subX |= ((m >> (2 * i)) & 1u) << i; subY |= ((m >> (2 * i + 1)) & 1u) << i; }
    };
    for (const auto &[dim, e] : byDim)
    {
        (void)e;
        for (int lod = 0; lod <= 4; ++lod)
        {
            char prefix[16]; std::snprintf(prefix, sizeof(prefix), "M%02d_L%d_", dim, lod);
            int cnt = 0, minNX = 1 << 30, maxNX = -(1 << 30), minNZ = 1 << 30, maxNZ = -(1 << 30);
            int minCol = 1 << 30, maxCol = -(1 << 30), minRow = 1 << 30, maxRow = -(1 << 30);
            std::string samples;
            for (const auto &en : entries)
            {
                size_t p = en.name.find(prefix);
                if (p == std::string::npos) continue;
                size_t c0 = p + std::strlen(prefix);
                size_t c1 = en.name.find('_', c0);
                size_t r1 = (c1 == std::string::npos) ? std::string::npos : en.name.find('_', c1 + 1);
                size_t s1 = (r1 == std::string::npos) ? std::string::npos : en.name.find('.', r1 + 1);
                if (c1 == std::string::npos || r1 == std::string::npos || s1 == std::string::npos) continue;
                long col = std::strtol(en.name.substr(c0, c1 - c0).c_str(), nullptr, 10);
                long row = std::strtol(en.name.substr(c1 + 1, r1 - c1 - 1).c_str(), nullptr, 10);
                uint32_t suf = (uint32_t)std::strtoul(en.name.substr(r1 + 1, s1 - r1 - 1).c_str(), nullptr, 16);
                int sx, sz; decode_suffix(suf, sx, sz);
                int nx = (int)col * 64 + sx, nz = (int)row * 64 + sz;
                minNX = (std::min)(minNX, nx); maxNX = (std::max)(maxNX, nx);
                minNZ = (std::min)(minNZ, nz); maxNZ = (std::max)(maxNZ, nz);
                minCol = (std::min)(minCol, (int)col); maxCol = (std::max)(maxCol, (int)col);
                minRow = (std::min)(minRow, (int)row); maxRow = (std::max)(maxRow, (int)row);
                if (cnt < 6) samples += " " + en.name.substr(c0 - 4, s1 - (c0 - 4)) +
                                        "(nx" + std::to_string(nx) + ",nz" + std::to_string(nz) + ")";
                ++cnt;
            }
            if (cnt == 0) continue;
            spdlog::info("[TILERECON] archive {} count={} col[{}..{}] row[{}..{}] nx[{}..{}] nz[{}..{}] samples:{}",
                         prefix, cnt, minCol, maxCol, minRow, maxRow, minNX, maxNX, minNZ, maxNZ, samples);
        }
    }
    char st[128];
    std::snprintf(st, sizeof(st), "recon: %d resident, %d dim(s) — see [TILERECON] log", n, (int)byDim.size());
    return (s_tile_status = st);
}

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

void draw_virtual_map(const OverlayFrameCtx &ctx)
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
    // Focus the player on OPEN (rising edge of s_open, ANY open path). Tracked BEFORE the early return
    // so a close→reopen RE-fires — else s_was_open never resets while closed (the fn returns first) and
    // focus fired only on the very first open.
    {
        static bool s_was_open = false;
        if (s_open && !s_was_open) s_focus_player = true;
        s_was_open = s_open;
    }
    if (!s_open) return;

    ImGui::SetNextWindowSize(ImVec2(720.0f, 560.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(470.0f, 40.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(tr("MapForGoblins \xE2\x80\x94 Virtual World Map (WIP)"), &s_open))
    {
        ImGui::End();
        return;
    }
    GOBLIN_BENCH_QUIET("vmap.draw");   // whole-panel cost vs the vmap.markers sub-cost (bottleneck attribution)

    // Resolution-aware UI scale (parity with the minimap's `uiScale = screenH/1080`): every FIXED-PIXEL
    // DrawList size below — icon/pin/dot radii, the player arrow, pile discs, region-label font, and the
    // px text OFFSETS — is multiplied by this so the vmap reads the same at 720p/1080p/4K instead of
    // tiny-at-4K / huge-at-720p. Pan/zoom (w2s, s_zoom) stays in WORLD units and is left UNSCALED — it's
    // resolution-independent, exactly as the minimap leaves minimapZoom unscaled. Floor 0.5 so a tiny
    // window never collapses the geometry to sub-pixel.
    const float uiScale = (std::max)(0.5f, ImGui::GetIO().DisplaySize.y / 1080.0f);

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
    ImGui::SameLine();
    // Jump to the live player — switch to the vworld the player physically occupies (PlayerDim, base ER
    // today) so it recentres even from another active world, then focus. Not hardcoded 0 → future-proof.
    if (ImGui::SmallButton(tr("Player"))) { goblin::vworld::set_active(goblin::vworld::player_world()); s_focus_player = true; }
    ImGui::SameLine();
    ImGui::Checkbox(tr("Follow"), &s_follow_player_dim);   // auto-switch page to the player's dimension on a crossing
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("auto-switch the map page to the player's dimension (OW/underground/DLC) when they cross"));
    ImGui::SameLine();
    ImGui::Checkbox(tr("Icons"), &s_show_icons);   // real category icons vs plain dots
    ImGui::SameLine();
    ImGui::Checkbox(tr("Labels"), &s_show_labels); // coarse region name labels (A7)
    ImGui::SameLine();
    ImGui::Checkbox(tr("Relief"), &s_show_relief); // heightfield hillshade backdrop (D2.3)
    ImGui::SameLine();
    if (ImGui::Checkbox(tr("Graces"), &s_show_graces) && s_show_graces) s_graces_built = false; // Track B sidebar
    ImGui::SameLine();
    ImGui::Checkbox(tr("Markers"), &s_show_markers_panel);  // F1→vmap: category visibility controls
    ImGui::SameLine();
    ImGui::Checkbox(tr("Custom"), &s_show_custom);  // our own player-placed markers (right-click to drop)
    ImGui::SameLine();
    ImGui::Checkbox(tr("Items"), &s_show_item_search);  // A9: search + locate items on the vmap itself
    ImGui::SameLine();
    // 1024u extent = the ACCURATE zone: the world→cast-local transform is a translation captured at the
    // player, valid only near the player's physics chunk (ER recenters the frame across tiles) — 1024u
    // holds ~75% hits vs ~36% at 2048u where far cells cast in the wrong frame. Full coverage = accumulate
    // as the player moves (D2.4). res 48 → ~21u cells, dense.
    if (ImGui::SmallButton(tr("Sample terrain")))  // cast a grid around the player (map must be CLOSED)
    {
        goblin::overlay_api::heightfield_request_sample(1024.f, 48);
        s_show_relief = true;   // so the result is visible even if Relief was toggled off
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);                                // dev: tune the warp id offset live
    ImGui::DragInt(tr("warp off"), &s_warp_offset, 10.0f, -100000, 100000);  // double-click a grace to test
    // Load ER's real map ART tiles (WIP — see below). Needs the game map OPEN + you moving on it a
    // moment (so the projection resolves). Loads overworld coarse tiles around the marker area.
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Load ER map tiles"))) virtual_map_load_lod(0, 3, 240);
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Resident cells"))) virtual_map_load_resident();  // engine positions (aligned)
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Clear tiles"))) virtual_map_clear_tiles();
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Flip X"))) s_sx = -s_sx;   // orientation calibration vs native map
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Flip Z"))) s_sz = -s_sz;
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
            {
                goblin::vworld::set_active(p.first);
                goblin::vworld::save_default();   // persist the choice (incl. Base ER=0) → survives restart
                if (p.first == 0) s_focus_player = true;   // switched to base ER → recentre on the player
            }
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
    ImGui::TextDisabled(tr("drag = pan   wheel = zoom   |   centre (%.0f, %.0f)  zoom %.3f px/u   markers %d   relief %d/%d"),
                        s_cam_x, s_cam_z, s_zoom, s_drawn, s_relief_drawn, s_relief_hits);
    if (!s_tiles.empty() || !s_tile_status.empty())
        ImGui::TextDisabled(tr("map tiles: %d loaded   |   last: %s"), (int)s_tiles.size(),
                            s_tile_status.c_str());

    // Draggable vertical splitter between a sidebar and the next element — lets the user resize the
    // sidebar (ImGui has no built-in splitter; an invisible drag-bar that nudges the width is the idiom).
    auto vsplitter = [&](const char *id, float &w, float wmin, float wmax) {
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::InvisibleButton(id, ImVec2(6.0f, ImGui::GetContentRegionAvail().y));
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActive()) w += ImGui::GetIO().MouseDelta.x;
        if (w < wmin) w = wmin;
        if (w > wmax) w = wmax;
        ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2((mn.x + mx.x) * 0.5f - 1.0f, mn.y), ImVec2((mn.x + mx.x) * 0.5f + 1.0f, mx.y),
            hot ? IM_COL32(230, 210, 140, 220) : IM_COL32(90, 96, 110, 160));
        ImGui::SameLine(0.0f, 0.0f);
    };

    // F1→vmap port (first slice): host the F1 "Sections & categories" controls in a vmap sidebar. It's a
    // standalone draw_X(ctx, Filter&) with zero coupling to the F1 window, so it drops straight in — the
    // toggles it drives are shared config, so hiding a category here hides it on the vmap markers too. The
    // eventual home is a unified tabbed sidebar (Markers|Search|Quests|…); this proves the reuse path.
    if (s_show_markers_panel)
    {
        ImGui::BeginChild("##markers_panel", ImVec2(s_markers_w, 0.0f), true);
        // Master "show all markers" toggle (the F1 icon master) — ported here so the categories list has
        // its on/off head; drives overlay_api::icons_enabled (gates every vmap marker + the native map).
        bool master_on = goblin::overlay_api::icons_enabled();
        if (ImGui::Checkbox(tr("Show all markers"), &master_on))
            goblin::overlay_api::set_icons_enabled(master_on);
        ImGui::Separator();
        draw_sections_categories(ctx, s_markers_filter, /*with_err_integration=*/false);   // ERR moot on ImGui map
        ImGui::EndChild();
        vsplitter("##markers_split", s_markers_w, 200.0f, 640.0f);
    }

    // Track B — grace warp-menu sidebar (Base ER only; graces are ER's). A collapsible list next to the
    // canvas: search + every grace grouped-by-state, discovered = warpable (double-click), undiscovered =
    // locate-only (single-click pans the canvas — warping to an undiscovered grace hangs on infinite load).
    // Data is the SAME grace layer the canvas draws (row_id/discover_flag/name_id); rebuilt on open/Refresh,
    // discovered-state re-read live per row. Drawn BEFORE the canvas so SameLine leaves it the rest.
    if (s_show_graces && active_world == 0)
    {
        const int kGraceCatSb = static_cast<int>(goblin::generated::Category::WorldGraces);
        if (!s_graces_built)
        {
            s_graces.clear();
            for (auto *L : overlay_layers())
            {
                if (!L) continue;
                for (const goblin::worldmap::Marker &m : L->markers())
                    if (m.category == kGraceCatSb && m.row_id != 0)
                    {
                        GraceRow g{};
                        g.wx = m.worldX; g.wz = m.worldZ; g.rowId = m.row_id;
                        g.discFlag = m.discover_flag; g.group = m.group; g.regionIdx = -1;
                        g.name = m.name_id > 0 ? goblin::overlay_api::lookup_text_utf8(m.name_id) : std::string();
                        if (g.name.empty()) g.name = "(unnamed grace)";
                        s_graces.push_back(std::move(g));
                    }
            }
            // Assign each grace to the nearest same-group major region (reuse the A7 anchors) so the list
            // groups by region. Project each anchor to world XZ once via marker_world_pos (as A7 does).
            struct RA { float wx, wz; int grp; const char *name; };
            std::vector<RA> ranch;
            {
                using namespace goblin::generated;
                for (int i = 0; i < (int)MAJOR_REGION_ANCHOR_COUNT; ++i)
                {
                    const MajorRegionAnchor &a = MAJOR_REGION_ANCHORS[i];
                    int ga = 0; float wx = 0.f, wz = 0.f;
                    if (goblin::overlay_api::marker_world_pos(a.area, a.gx, a.gz, a.px, a.pz, ga, wx, wz, true))
                        ranch.push_back({wx, wz, goblin::marker_group_from(a.area, ga), a.name});
                }
            }
            for (GraceRow &g : s_graces)
            {
                int best = -1; float bd = 1e30f;
                for (int i = 0; i < (int)ranch.size(); ++i)
                {
                    if (ranch[i].grp != g.group) continue;
                    const float du = ranch[i].wx - g.wx, dv = ranch[i].wz - g.wz, d = du * du + dv * dv;
                    if (d < bd) { bd = d; best = i; }
                }
                g.regionIdx = best;
                g.region = best >= 0 ? ranch[best].name : "Other";
            }
            std::sort(s_graces.begin(), s_graces.end(), [](const GraceRow &a, const GraceRow &b) {
                if (a.region != b.region) return a.region < b.region;
                return a.name < b.name;
            });
            s_graces_built = true;
        }
        ImGui::BeginChild("##grace_sidebar", ImVec2(s_graces_w, 0.0f), true);
        ImGui::Text(tr("Graces (%d)"), (int)s_graces.size());
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Refresh"))) s_graces_built = false;
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##gfilter", tr("search"), s_grace_filter, sizeof(s_grace_filter));
        std::string flt = s_grace_filter;
        for (char &c : flt) c = (char)std::tolower((unsigned char)c);

        // Filter tabs: All / Discovered / Undiscovered (the useful warp-menu filter; the multi-world
        // All/Current/Other-worlds split is redundant with the World selector + only Base ER has graces).
        int mode = 0;
        if (ImGui::BeginTabBar("##gtabs"))
        {
            const char *tabs[3] = {"All", "Discovered", "Undiscovered"};
            for (int t = 0; t < 3; ++t)
                if (ImGui::BeginTabItem(tr(tabs[t]))) { mode = t; ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }

        auto render_row = [&](const GraceRow &g, bool disc) {
            ImGui::PushID((int)(g.rowId & 0x7fffffff));
            ImVec2 cp = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(cp.x + 5.0f, cp.y + ImGui::GetTextLineHeight() * 0.5f), 4.0f,
                disc ? IM_COL32(230, 190, 80, 255) : IM_COL32(105, 105, 105, 255));
            ImGui::Dummy(ImVec2(14.0f, 0.0f));
            ImGui::SameLine();
            if (ImGui::Selectable(g.name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick))
            {
                if (disc && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    s_warp_pending = g.rowId;                         // discovered → teleport (post-frame)
                else { s_cam_x = g.wx; s_cam_z = g.wz; s_group = g.group; }  // locate: pan the canvas to it
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", disc ? tr("double-click: warp · click: locate")
                                             : tr("undiscovered — click: locate"));
            ImGui::PopID();
        };

        ImGui::BeginChild("##glist");
        std::string curRegion; bool started = false, regionOpen = false;
        for (const GraceRow &g : s_graces)
        {
            if (!flt.empty())
            {
                std::string ln = g.name;
                for (char &c : ln) c = (char)std::tolower((unsigned char)c);
                if (ln.find(flt) == std::string::npos) continue;
            }
            const bool disc = g.discFlag > 0 && goblin::overlay_api::read_event_flag((uint32_t)g.discFlag);
            if ((mode == 1 && !disc) || (mode == 2 && disc)) continue;   // tab filter
            if (!started || g.region != curRegion)                       // region header (only for shown regions)
            {
                curRegion = g.region; started = true;
                regionOpen = ImGui::CollapsingHeader(curRegion.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
            }
            if (!regionOpen) continue;
            render_row(g, disc);
        }
        ImGui::EndChild();
        ImGui::EndChild();
        vsplitter("##graces_split", s_graces_w, 180.0f, 520.0f);
    }

    // Item-search sidebar (A9): search PLACED item markers by name and click to locate on THIS canvas —
    // the vmap-native equivalent of the F1 item search, so search works with the vmap as the sole surface
    // (no native ER map needed). Rebuilds the hit list only when the query changes (name lookups cached).
    if (s_show_item_search && active_world == 0)
    {
        struct IHit { std::string label; int32_t name_id; int group; int count; };
        static std::vector<IHit> s_ihits;
        static std::string s_ihits_q = "\x01";   // sentinel ≠ "" so an empty box builds once (clears list)

        ImGui::BeginChild("##item_sidebar", ImVec2(s_items_w, 0.0f), true);
        ImGui::Text(tr("Item search"));
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##ifilter", tr("item name (2+ chars)"), s_item_filter, sizeof(s_item_filter));

        // Mark-all-results controls: toggle, max count, Clear. Changing the toggle/max re-runs the marks
        // against the current query (force a rebuild by resetting the query sentinel).
        ImGui::Checkbox(tr("Mark results"), &s_search_mark_on);
        ImGui::SameLine(); ImGui::TextDisabled("max"); ImGui::SameLine();
        ImGui::SetNextItemWidth(64.f * uiScale);
        ImGui::InputInt("##markmax", &s_search_mark_max, 0);
        if (s_search_mark_max < 1) s_search_mark_max = 1;
        if (s_search_mark_max > 4000) s_search_mark_max = 4000;
        ImGui::SameLine();
        if (ImGui::Button(tr("Clear marks"))) { goblin::search_marks::clear(); s_search_mark_on = false; }
        {
            static bool s_prev_on = s_search_mark_on; static int s_prev_max = s_search_mark_max;
            if (s_prev_on != s_search_mark_on || s_prev_max != s_search_mark_max)
            { s_prev_on = s_search_mark_on; s_prev_max = s_search_mark_max; s_ihits_q = "\x01"; }
        }

        if (std::string(s_item_filter) != s_ihits_q)
        {
            s_ihits_q = s_item_filter;
            s_ihits.clear();
            std::vector<goblin::search_marks::Mark> marks;   // regenerated for the new query, then published
            if (s_ihits_q.size() >= 2)
            {
                std::unordered_map<int32_t, std::string> loc_cache, en_cache;
                std::map<int64_t, int> hitc;                 // (name_id<<2 | group) -> instance count
                for (auto *L : overlay_layers())
                {
                    if (!L) continue;
                    for (const goblin::worldmap::Marker &m : L->markers())
                    {
                        if (m.name_id < 0) continue;
                        auto li = loc_cache.find(m.name_id);
                        if (li == loc_cache.end())
                        {
                            li = loc_cache.emplace(m.name_id,
                                                   goblin::overlay_api::lookup_text_utf8(m.name_id)).first;
                            en_cache.emplace(m.name_id,
                                             goblin::overlay_api::lookup_name_en_disk_utf8(m.name_id));
                        }
                        const std::string &loc = li->second;
                        const std::string &en = en_cache[m.name_id];
                        if (loc.empty() && en.empty()) continue;
                        if (!matches_all_tokens(loc + " " + en, s_item_filter)) continue;
                        const int g = m.group & 3;
                        // Mark EVERY matching instance (not deduped like the list rows), capped.
                        if (s_search_mark_on && (int)marks.size() < s_search_mark_max)
                            marks.push_back({m.worldX, m.worldZ, g});
                        const int64_t k = ((int64_t)m.name_id << 2) | g;
                        if (hitc[k]++ == 0)
                        {
                            std::string label = loc.empty() ? en : loc;
                            if (!en.empty() && en != label) label += " (" + en + ")";
                            s_ihits.push_back({label, m.name_id, g, 0});
                        }
                    }
                }
                for (auto &h : s_ihits) h.count = hitc[((int64_t)h.name_id << 2) | h.group];
                std::sort(s_ihits.begin(), s_ihits.end(), [](const IHit &a, const IHit &b) {
                    int c = a.label.compare(b.label);
                    return c != 0 ? c < 0 : a.group < b.group;
                });
            }
            goblin::search_marks::set(std::move(marks));   // publish (empty when query<2 → clears)
        }

        if (std::string(s_item_filter).size() < 2)
            ImGui::TextDisabled("%s", tr("type 2+ characters"));
        else if (s_ihits.empty())
            ImGui::TextDisabled("%s", tr("no item matches"));
        else
            ImGui::TextDisabled(tr("%d result(s) — click to locate"), (int)s_ihits.size());

        ImGui::BeginChild("##ilist");
        for (const IHit &h : s_ihits)
        {
            char row[224];
            std::snprintf(row, sizeof(row), "%s  (x%d) - %s##ih%d", h.label.c_str(), h.count,
                          (h.group >= 0 && h.group < 4) ? kGroupNames[h.group] : "?", h.name_id);
            if (ImGui::Selectable(row))
                virtual_map_locate(h.name_id, h.group);   // centre the canvas on the hit + switch page
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(tr("click: centre the map on it (%s)"),
                                  (h.group >= 0 && h.group < 4) ? kGroupNames[h.group] : "?");
        }
        ImGui::EndChild();
        ImGui::EndChild();
        vsplitter("##items_split", s_items_w, 180.0f, 520.0f);
    }

    // Custom-marker LIST sidebar (#2) — the DX answer to "where's my custom marker?": each shows which map
    // (group) + coords, with Go (pan the canvas there), TP (teleport in-game, intra-region), and delete.
    if (s_show_custom && active_world == 0)
    {
        ImGui::BeginChild("##custom_panel", ImVec2(s_custom_w, 0.0f), true);
        auto cm = goblin::custom_markers::snapshot();
        ImGui::Text(tr("Custom markers (%d/%d this map)"),
                    (int)goblin::custom_markers::count_in_group(s_group), goblin::custom_markers::kMaxPerGroup);
        ImGui::TextDisabled("%s", tr("right-click the map to add / delete"));
        ImGui::Separator();
        int del = -1;
        for (int i = 0; i < (int)cm.size(); ++i)
        {
            const goblin::custom_markers::Marker &c = cm[i];
            ImGui::PushID(i);
            char nbuf[64];
            std::snprintf(nbuf, sizeof(nbuf), "%s", c.name.c_str());
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText("##nm", nbuf, sizeof(nbuf))) goblin::custom_markers::set_name((size_t)i, nbuf);
            const char *gname = (c.group >= 0 && c.group < 4) ? kGroupNames[c.group] : "?";
            ImGui::TextDisabled("%s  (%.0f, %.0f)", tr(gname), c.wx, c.wz);
            if (ImGui::SmallButton(tr("Go")))
            {
                s_cam_x = c.wx; s_cam_z = c.wz; s_group = c.group;
                if (s_zoom < 0.10f) s_zoom = 0.10f;
            }
            // TP button hidden: coordinate-teleport doesn't exist in ER (warp_to_world_xz is an
            // intra-region local-pos poke, unreliable). Keep only "Go" (pan) until a proper streaming
            // teleport lands — see the Track-B fast-travel note in docs/plans/custom_markers_plan.md.
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Delete"))) del = i;
            ImGui::Separator();
            ImGui::PopID();
        }
        if (del >= 0) goblin::custom_markers::remove_at((size_t)del);
        ImGui::EndChild();
        vsplitter("##custom_split", s_custom_w, 180.0f, 480.0f);
    }

    // Canvas = the remaining content region. An InvisibleButton captures drag/scroll over exactly it.
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 32.0f) size.x = 32.0f;
    if (size.y < 32.0f) size.y = 32.0f;
    const ImVec2 canvas_end(origin.x + size.x, origin.y + size.y);
    const ImVec2 center(origin.x + size.x * 0.5f, origin.y + size.y * 0.5f);

    ImGui::InvisibleButton("##vmap_canvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
                           ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    ImGuiIO &io = ImGui::GetIO();

    // Focus-on-player one-shot: centre the camera on the live player position + switch to their group,
    // at a readable close zoom. Base ER only (custom worlds have no player pos). Do it BEFORE w2s so the
    // new cam applies this frame.
    if (s_focus_player && active_world == 0)
    {
        int parea = 0, pgroup = 0;
        float pwx = 0.f, pwz = 0.f;
        bool got = goblin::overlay_api::get_player_map_pos(parea, pwx, pwz, nullptr, nullptr, &pgroup);
        spdlog::info("[VMAP] focus player: got={} area={} group={} worldXZ=({:.0f},{:.0f})", got, parea, pgroup, pwx, pwz);
        if (got)
        {
            s_cam_x = pwx; s_cam_z = pwz; s_group = pgroup;
            if (s_zoom < 0.12f) s_zoom = 0.12f;   // ensure a useful close-in view, don't zoom back out
            s_focus_player = false;               // consumed (only clears once we actually had a position)
            s_player_group_prev = pgroup;         // seed the follow edge-detector so the open doesn't re-switch
        }
    }

    // Auto-follow the player's DIMENSION: when the player crosses OW<->underground<->DLC, switch the vmap
    // PAGE (group) to match. Edge-triggered on a real dimension change so a manual group pick between
    // crossings sticks (and toggle "Follow" off to browse other pages freely). Camera is left alone — this
    // switches the page, not the view (unlike the one-shot "Player" recenter). Base ER only. Reuses the
    // already-RE'd pgroup from get_player_map_pos (the player dot reads it every frame too).
    if (s_follow_player_dim && active_world == 0 && !s_focus_player)
    {
        int fa = 0, fg = 0; float fwx = 0.f, fwz = 0.f;
        if (goblin::overlay_api::get_player_map_pos(fa, fwx, fwz, nullptr, nullptr, &fg))
        {
            if (fg != s_player_group_prev)   // player crossed a dimension boundary this frame
            {
                if (s_player_group_prev != -1 && fg != s_group)
                {
                    s_group = fg;            // switch the page to the player's new dimension
                    spdlog::info("[VMAP] follow: player crossed to group {} -> switch page", fg);
                }
                s_player_group_prev = fg;    // record (also seeds on first sight without switching)
            }
        }
    }

    // world → screen and screen → world. Z is FLIPPED (screen-Y decreases as worldZ increases) to match the
    // native ER map orientation — the engine converter flips Z (mapZ = -worldZ + bias); without this the
    // vmap was a vertical mirror of the game map. Keep w2s/s2w inverse so zoom-about-cursor stays exact.
    auto w2s = [&](float wx, float wz) {
        return ImVec2(center.x + s_sx * (wx - s_cam_x) * s_zoom, center.y + s_sz * (wz - s_cam_z) * s_zoom);
    };
    auto s2w = [&](ImVec2 s, float &wx, float &wz) {
        wx = s_cam_x + (s.x - center.x) / (s_sx * s_zoom);
        wz = s_cam_z + (s.y - center.y) / (s_sz * s_zoom);
    };

    // Pan: dragging moves the camera opposite the mouse delta (in world units).
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
    {
        s_cam_x -= io.MouseDelta.x / (s_sx * s_zoom);
        s_cam_z -= io.MouseDelta.y / (s_sz * s_zoom);   // axis signs (s_sx/s_sz) keep drag correct on flip
    }
    // Right-click the canvas: near an existing custom pin of this group → DELETE it (#1, a second delete
    // path besides the sidebar button); otherwise → DROP a new marker at that world XZ, tagged with the
    // current group (which map). add() enforces the per-world cap (#2). Base ER only.
    if (hovered && active_world == 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
    {
        float mwx, mwz;
        s2w(io.MousePos, mwx, mwz);
        auto cm = goblin::custom_markers::snapshot();
        int hit = -1;
        float bestd = 14.0f * 14.0f;   // delete radius in px
        for (int i = 0; i < (int)cm.size(); ++i)
        {
            if (cm[i].group != s_group) continue;
            ImVec2 ps = w2s(cm[i].wx, cm[i].wz);
            float dx = ps.x - io.MousePos.x, dy = ps.y - io.MousePos.y, d = dx * dx + dy * dy;
            if (d < bestd) { bestd = d; hit = i; }
        }
        if (hit >= 0)
            goblin::custom_markers::remove_at((size_t)hit);
        else if (!goblin::custom_markers::add(mwx, mwz, s_group,
                     std::string("Marker ") + std::to_string(s_custom_seq++), IM_COL32(90, 200, 255, 255)))
            s_tile_status = "custom marker cap reached for this map";
        s_show_custom = true;
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

    // ER map ART tiles: draw each loaded tile at its world quad, under the grid+markers. srv==0 = a
    // position-only resident cell (harvested rect, no texture yet) → faint fill + frame so it's visible.
    for (const LoadedTile &t : s_tiles)
    {
        ImVec2 a = w2s(t.wx0, t.wz0), b = w2s(t.wx1, t.wz1);
        if (t.srv)
        {
            dl->AddImage((ImTextureID)t.srv, a, b);
            dl->AddRect(a, b, IM_COL32(90, 160, 220, 120));
        }
        else  // resident cell outline (position calibration view)
        {
            dl->AddRectFilled(a, b, IM_COL32(70, 130, 90, 60));
            dl->AddRect(a, b, IM_COL32(120, 220, 150, 180));
        }
    }

    // Heightfield relief (D2.3): draw the last completed sample as hillshaded quads UNDER the grid +
    // markers — the mod-agnostic terrain backdrop (sampled live from the 3D world, correct for any mod).
    // Each hit cell = a filled world-space square coloured by dot(surface-normal, light). Base ER only
    // (the sample is in ER world XZ). Cheap: one AddRectFilled per hit cell, off-canvas culled.
    s_relief_hits = 0; s_relief_drawn = 0;
    if (s_show_relief)
    {
        GOBLIN_BENCH_QUIET("vmap.relief");   // per-cell quad draw + snapshot copy
        // Centred hint near the top of the canvas, so "nothing shows" is never silent (the #1 confusion:
        // relief is Base-ER-only, or no sample has run yet, or a sample is in flight).
        auto relief_hint = [&](const char *msg) {
            ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.f, msg);
            ImVec2 p((origin.x + canvas_end.x) * 0.5f - ts.x * 0.5f, origin.y + 24.f);
            dl->AddRectFilled(ImVec2(p.x - 6, p.y - 4), ImVec2(p.x + ts.x + 6, p.y + ts.y + 4),
                              IM_COL32(20, 22, 28, 205), 4.f);
            dl->AddText(p, IM_COL32(232, 212, 140, 255), msg);
        };
        if (active_world != 0)
        {
            // Custom worlds have their own coordinate space; the ER terrain sample doesn't apply.
            relief_hint(tr("Terrain relief is Base-ER only — set the World selector to \"Base ER\""));
        }
        else
        {
            // D-far -1 v0 auto-build: (re)build the MSB Y-cloud field for the ACTIVE group the first time
            // Relief is shown and whenever the group (dimension) changes — so the 3 maps (overworld /
            // underground / DLC) each get their own relief with no manual RPC. Host-side, ~ms; latched by
            // built-group so it runs once per switch (a transient "parse not ready" leaves the latch → retries).
            if (goblin::overlay_api::far_relief_built_group() != s_group)
                goblin::overlay_api::far_relief_build(s_group, 128);
            // D-far -1 v0: the MSB Y-cloud relief. Drawn UNDER the near raycast (below) so the exact near
            // samples overdraw the cloud estimate where both exist. Cooler tint so cloud vs raycast read apart.
            {
                static std::vector<goblin::heightfield::Cell> s_far;
                goblin::overlay_api::far_relief_snapshot(s_far);
                const float fcell = goblin::overlay_api::far_relief_step();
                if (fcell > 0.f && !s_far.empty())
                {
                    const float h = fcell * 0.5f;
                    const float Lx = 0.40f, Ly = 0.82f, Lz = 0.40f;
                    for (const goblin::heightfield::Cell &c : s_far)
                    {
                        if (!c.hit) continue;
                        ImVec2 a = w2s(c.wx - h, c.wz - h), b = w2s(c.wx + h, c.wz + h);
                        ImVec2 p0(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y);
                        ImVec2 p1(a.x < b.x ? b.x : a.x, a.y < b.y ? b.y : a.y);
                        if (p1.x < origin.x || p0.x > canvas_end.x || p1.y < origin.y || p0.y > canvas_end.y) continue;
                        float nl = std::sqrt(c.nx * c.nx + c.ny * c.ny + c.nz * c.nz);
                        float sh = nl > 1e-4f ? (c.nx * Lx + c.ny * Ly + c.nz * Lz) / nl : 0.6f;
                        sh = sh < 0.20f ? 0.20f : (sh > 1.10f ? 1.10f : sh);
                        auto ch = [sh](float base) { int v = (int)(base * sh + 0.5f); return v < 0 ? 0 : (v > 255 ? 255 : v); };
                        dl->AddRectFilled(p0, p1, IM_COL32(ch(104), ch(118), ch(128), 210));  // cooler = cloud estimate
                        ++s_relief_drawn;
                    }
                    s_relief_hits += (int)s_far.size();   // suppress the "no terrain" hint when the cloud is up
                }
            }
            static std::vector<goblin::heightfield::Cell> s_relief;
            goblin::overlay_api::heightfield_snapshot(s_relief);
            const float cell = goblin::overlay_api::heightfield_cell_step();
            for (const auto &c : s_relief) if (c.hit) ++s_relief_hits;
            if (cell > 0.f && !s_relief.empty())
            {
                const float h = cell * 0.5f;
                const float Lx = 0.40f, Ly = 0.82f, Lz = 0.40f;   // light from upper-NW (hillshade), normalized
                for (const goblin::heightfield::Cell &c : s_relief)
                {
                    if (!c.hit) continue;
                    ImVec2 a = w2s(c.wx - h, c.wz - h), b = w2s(c.wx + h, c.wz + h);
                    ImVec2 p0(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y);
                    ImVec2 p1(a.x < b.x ? b.x : a.x, a.y < b.y ? b.y : a.y);
                    if (p1.x < origin.x || p0.x > canvas_end.x || p1.y < origin.y || p0.y > canvas_end.y)
                        continue;
                    float nl = std::sqrt(c.nx * c.nx + c.ny * c.ny + c.nz * c.nz);
                    float sh = nl > 1e-4f ? (c.nx * Lx + c.ny * Ly + c.nz * Lz) / nl : 0.6f;
                    sh = sh < 0.20f ? 0.20f : (sh > 1.10f ? 1.10f : sh);   // ambient floor + gentle clip
                    auto ch = [sh](float base) { int v = (int)(base * sh + 0.5f); return v < 0 ? 0 : (v > 255 ? 255 : v); };
                    dl->AddRectFilled(p0, p1, IM_COL32(ch(120), ch(132), ch(110), 220));
                    ++s_relief_drawn;
                }
            }
            if (goblin::overlay_api::heightfield_sampling())
                relief_hint(tr("Sampling terrain…"));
            else if (s_relief_hits == 0)
                relief_hint(tr("No terrain sampled yet — press 'Sample terrain' (in gameplay, map closed)"));
        }
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

    // Markers: project the ACTIVE world's markers onto the canvas + accumulate a bbox for Fit. Real
    // category icons draw on-canvas (`s_show_icons`: base ER → `draw_marker_glyph`; custom → `icon_for`),
    // with a colored dot fallback when no glyph resolves. Base ER (world 0) → the live ER markers of the
    // selected group (slice B); a custom world → its OWN markers in its own coordinate namespace (slice C).
    int drawn = 0;
    float minx = 1e30f, minz = 1e30f, maxx = -1e30f, maxz = -1e30f;
    // Per-category icon cache (resolve once per frame, reuse across the thousands of markers).
    struct IcoRes { void *tex; ImVec2 uv0, uv1; bool ok, tried; };
    std::unordered_map<int, IcoRes> icoCache;
    auto icon_for = [&](int cat) -> IcoRes & {
        IcoRes &r = icoCache[cat];
        if (!r.tried) { r.tried = true; r.ok = s_show_icons && cat >= 0 && resolve_category_icon(ctx, cat, r.tex, r.uv0, r.uv1); }
        return r;
    };
    // On-canvas icon size (px, resolution-scaled). GROWS with zoom so pins aren't microscopic when zoomed
    // in — and, since the spiderfy fan geometry (spacing/baseR/keepR) is all sized off icoHalf, the hover
    // fan grows with it (fixes "spiderfy far too small at high zoom"). Bounded: 1× at the overview default
    // (s_zoom≈0.05–0.15), ramping via sqrt to ~2.2× when zoomed right in (s_zoom→kZoomMax).
    float zoomIco = std::sqrt(s_zoom / 0.15f);
    if (zoomIco < 1.0f) zoomIco = 1.0f;
    if (zoomIco > 2.2f) zoomIco = 2.2f;
    const float icoHalf = 8.0f * uiScale * zoomIco;
    const bool nativeIcons = *goblin::overlay_api::cfg_nativeItemIcons_ptr();
    constexpr int kGraceCat = static_cast<int>(goblin::generated::Category::WorldGraces);
    // Hover tooltip: track the marker nearest the cursor (within a pixel radius) while drawing.
    // Graces draw ON TOP (2nd pass) so they must also WIN the hover — track a priority (grace=1) so a
    // grace within radius beats an underlying non-grace, matching the visible z-order (the tooltip/warp
    // then targets the grace you see on top, not the object beneath it).
    float hoverBestD = 1e18f; int hoverName = -1, hoverCat = -1, hoverDisc = 0, hoverPrio = -1; std::string hoverV; uint64_t hoverRow = 0;
    float hoverWx = 0.f, hoverWz = 0.f;  // world pos of the hovered marker (for warp diagnostics)
    int hoverArea = -1;                  // the hovered marker's REAL area (mp->raw_area), for warp diagnostics
    // mp != null (base ER) → reuse the native state-aware per-marker draw (grace discovered/undiscovered
    // sprite, collected-dim, cleared check, rune glow, badge) so the vmap does NOT duplicate that logic.
    auto plot = [&](float wx, float wz, uint32_t col, int cat, int nameId, const char *vname, uint64_t rowId,
                    int discFlag, const goblin::worldmap::Marker *mp) {
        if (wx < minx) minx = wx;
        if (wx > maxx) maxx = wx;
        if (wz < minz) minz = wz;
        if (wz > maxz) maxz = wz;
        ImVec2 ps = w2s(wx, wz);
        if (ps.x < origin.x || ps.x > canvas_end.x || ps.y < origin.y || ps.y > canvas_end.y) return;
        if (mp && s_show_icons)
            goblin::worldmap::draw_marker_glyph(dl, *mp, ps.x, ps.y, ctx.atlas_srv, nativeIcons, icoHalf);
        else
        {
            const IcoRes &r = icon_for(cat);
            if (s_show_icons && r.ok)
                dl->AddImage((ImTextureID)r.tex, ImVec2(ps.x - icoHalf, ps.y - icoHalf),
                             ImVec2(ps.x + icoHalf, ps.y + icoHalf), r.uv0, r.uv1);
            else
                dl->AddCircleFilled(ps, 3.0f * uiScale, col ? col : IM_COL32(235, 130, 90, 255));
        }
        drawn++;
        if (hovered)
        {
            float dx = ps.x - io.MousePos.x, dy = ps.y - io.MousePos.y, d = dx * dx + dy * dy;
            const int prio = (cat == kGraceCat) ? 1 : 0;   // graces (drawn on top) win the hover
            if (d < icoHalf * icoHalf * 2.0f && (prio > hoverPrio || (prio == hoverPrio && d < hoverBestD)))
            { hoverBestD = d; hoverPrio = prio; hoverName = nameId; hoverCat = cat; hoverV = vname ? vname : ""; hoverRow = rowId; hoverDisc = discFlag; hoverWx = wx; hoverWz = wz; hoverArea = mp ? mp->raw_area : -1; }
        }
    };
    // ── Region-hide gate precompute (A7 interactive region labels) ─────────────────────────────────
    // Project each major-region anchor to WORLD space ONCE (9 anchors) — reused by BOTH the marker gate
    // here and the region-label draw below. regValid[i] = projected; regGrp[i] = its marker group. The
    // on/off flags are the shared map_renderer state (same flags the native chips use).
    // LIVE-projected vmap position for a marker/anchor (raw area/grid/pos → the unified marker world frame).
    // Uses the engine's live WorldMapViewModel converter (worldmap_probe::project). NB the converter does
    // NOT need the native map to be OPEN each time: find_view_model() CACHES the VM (static s_vm) + only
    // re-scans when stale, and the VM object persists after the map closes — so once the map has been opened
    // ONCE this session (to publish the cursor for the first scan), project() keeps working map-closed. Until
    // that first resolve, project() returns false → we fall back to baked + a bounded rebuild retry. Baked
    // worldX/worldZ is correct for the
    // OVERWORLD (area 60) and DLC-overworld (61), but UNDERGROUND (12) is only approximate and DLC-UNDERGROUND
    // (40-43) is NOT folded at all by the baked path → those land near-origin (bottom-left, e.g. Nameless
    // Eternal City). For those, re-project via the engine converter → map-space (u,v) → worldX=u+7040,
    // worldZ=16512-v (the proven overworld affine, the inverse of world_to_mapspace). *ok=false if the
    // converter isn't resident yet → baked fallback + a bounded rebuild retry. Overworld = no-op (baked).
    auto vmap_proj = [](int area, int gx, int gz, float px, float pz, float bakedX, float bakedZ,
                        float &wx, float &wz, bool *ok = nullptr) {
        wx = bakedX; wz = bakedZ;
        if (ok) *ok = true;
        if (area == 12 || (area >= 40 && area <= 43))
        {
            float u = 0.f, v = 0.f; int pg = -1;
            if (goblin::worldmap_probe::project(area, gx, gz, px, pz, u, v, pg))
            { wx = u + 7040.0f; wz = 16512.0f - v; }
            else if (ok) *ok = false;
        }
    };
    constexpr int kRegCap = 16;  // MAJOR_REGION_ANCHOR_COUNT is 9; cap defensively.
    float regWx[kRegCap] = {}, regWz[kRegCap] = {};
    int regGrp[kRegCap];
    bool regValid[kRegCap] = {};
    bool anyRegionOff = false;
    const int regN = (int)goblin::generated::MAJOR_REGION_ANCHOR_COUNT;
    for (int i = 0; i < regN && i < kRegCap; ++i)
    {
        regGrp[i] = -1;
        const goblin::generated::MajorRegionAnchor &a = goblin::generated::MAJOR_REGION_ANCHORS[i];
        int ga = 0;
        float rwx = 0.f, rwz = 0.f;
        if (!goblin::overlay_api::marker_world_pos(a.area, a.gx, a.gz, a.px, a.pz, ga, rwx, rwz,
                                                   /*conv_underground=*/true))
            continue;
        // Re-project the anchor with the SAME converter the markers use, so an underground/DLC anchor and
        // its markers share one frame (else the region gate assigns the wrong area — Deeproot/Ainsel bug).
        float pwx = rwx, pwz = rwz;
        vmap_proj(a.area, a.gx, a.gz, a.px, a.pz, rwx, rwz, pwx, pwz);
        regWx[i] = pwx;
        regWz[i] = pwz;
        regGrp[i] = goblin::marker_group_from(a.area, ga);
        regValid[i] = true;
        if (!goblin::worldmap::region_enabled(i))
            anyRegionOff = true;
    }
    // Nearest same-group region anchor to a marker (world-space Voronoi assignment), or -1.
    auto nearest_region_world = [&](int grp, float wx, float wz) -> int {
        int best = -1;
        float bd = 1e30f;
        for (int i = 0; i < regN && i < kRegCap; ++i)
        {
            if (!regValid[i] || regGrp[i] != grp)
                continue;
            const float du = regWx[i] - wx, dv = regWz[i] - wz, d = du * du + dv * dv;
            if (d < bd) { bd = d; best = i; }
        }
        return best;
    };
    // Categories EXEMPT from a region-hide — they stay visible even when the region is toggled off.
    // Deliberately minimal + navigation-critical: only graces (fast-travel anchors). The player marker
    // is a separate always-on draw (below), so it is exempt by construction. Everything else — loot,
    // collectibles, landmarks, bosses, interactables — is the clutter the toggle exists to hide. To keep
    // another family visible when a region is off, add its Category to this test.
    auto region_hide_exempt = [&](int cat) -> bool { return cat == kGraceCat; };
    // True when marker `m` must be hidden because its region is toggled off and it isn't exempt.
    auto region_gated = [&](const goblin::worldmap::Marker &m) -> bool {
        if (!anyRegionOff || region_hide_exempt(m.category))
            return false;
        const int ri = nearest_region_world(m.group, m.worldX, m.worldZ);
        return ri >= 0 && !goblin::worldmap::region_enabled(ri);
    };

    // Graces draw ON TOP of every other marker (parity with the native map, which draws the grace layer
    // last). Two passes: pass 1 = all non-grace markers, pass 2 = graces — so a grace effigy is never
    // hidden under a co-located loot/landmark glyph. (kGraceCat defined above for the hover priority.)
    if (active_world == 0)
    {
        GOBLIN_BENCH_QUIET("vmap.markers");
        // Spatial index replaces the old O(n) per-frame loop over all 6837 markers (the profiled ~99%
        // bottleneck). (Re)build for the open group (markers are static ⇒ rebuild only on group change);
        // origin-defaulted (0,0) markers are excluded (the "hors map" stray icons). Graces are NOT indexed
        // — they draw individually on top (few, must stay clickable/warpable).
        // Category/section/master visibility gate — parity with the native map (the F1 category toggles).
        // Include a marker only if its category AND section are enabled and the icon master is on. The
        // index rebuilds whenever visibility_generation() bumps (any toggle), so hiding a category removes
        // its markers from BOTH the singles AND the cluster piles. (Before this the vmap drew every marker
        // regardless — the reported "toggles do nothing".)
        auto marker_shown = [](const goblin::worldmap::Marker &m) -> bool {
            if (!goblin::overlay_api::icons_enabled()) return false;
            if (m.category < 0) return true;
            if (!goblin::overlay_api::category_visible(m.category)) return false;
            const int sec = goblin::overlay_api::category_section(m.category);
            return sec < 0 || goblin::overlay_api::section_visible(sec);
        };
        const uint32_t vis_gen = goblin::overlay_api::visibility_generation();
        // Region-name toggles don't bump vis_gen (region_set_enabled only persists the flag), so fold a
        // fingerprint of the enabled regions into the rebuild key → a region toggle re-includes/-excludes
        // its markers in BOTH the piles (count/centroid) and singles, not just the draw-time singles gate.
        uint32_t region_mask = 0;
        for (int i = 0; i < regN && i < kRegCap && i < 32; ++i)
            if (regValid[i] && goblin::worldmap::region_enabled(i)) region_mask |= (1u << i);
        // A UG/DLC group whose live projection was incomplete (converter still warming up) forces a bounded
        // rebuild retry — so the correct positions land once the WorldMapViewModel is resident.
        const bool proj_retry = s_qt_proj_incomplete && s_qt_proj_retries < 60;
        if (s_qt_group != s_group || s_qt_vis_gen != vis_gen || s_qt_region_mask != region_mask || proj_retry)
        {
            // Reject "hors map" markers: origin-defaulted (0,0) and wildly-out-of-frame coords (a few
            // markers project to garbage like (110767,-59445)). ER base world XZ is ~[0..20000]; a generous
            // ±40000 box keeps every real marker while dropping the outliers that otherwise (a) draw as
            // stray icons and (b) blow up the Fit bbox so everything renders microscopic.
            auto implausible = [](float x, float z) {
                return (x == 0.f && z == 0.f) || x < -40000.f || x > 40000.f || z < -40000.f || z > 40000.f;
            };
            // PASS 1 — build the live-projected marker COPIES for this group (underground/DLC re-projected
            // through the engine converter; overworld = baked no-op). region_gated then reads the LIVE pos.
            s_vmarkers.clear();
            s_vmarkers.reserve(8192);
            bool proj_incomplete = false;
            for (auto *L : overlay_layers())
            {
                if (!L) continue;
                for (const goblin::worldmap::Marker &m : L->markers())
                {
                    if (m.group != s_group || !marker_shown(m)) continue;
                    goblin::worldmap::Marker cm = m;   // Marker is trivially copyable (POD + a static const char*)
                    bool ok = true;
                    vmap_proj(m.raw_area, m.raw_gx, m.raw_gz, m.raw_px, m.raw_pz, m.worldX, m.worldZ,
                              cm.worldX, cm.worldZ, &ok);
                    if (!ok) proj_incomplete = true;
                    if (implausible(cm.worldX, cm.worldZ)) continue;
                    s_vmarkers.push_back(cm);
                }
            }
            // PASS 2 — gate (region_gated on the LIVE pos) + split graces, pointing into the final vector.
            std::vector<const goblin::worldmap::Marker *> pts;
            pts.reserve(s_vmarkers.size());
            s_grace_pts.clear();
            for (const goblin::worldmap::Marker &cm : s_vmarkers)
            {
                if (region_gated(cm)) continue;   // exclude region-hidden → piles de-count them too (Fork 1)
                if (cm.category == kGraceCat)
                    s_grace_pts.push_back(&cm);    // graces drawn on top (region-exempt); into s_vmarkers
                else
                    pts.push_back(&cm);
            }
            s_qt.build(pts);
            s_qt_group = s_group;
            s_qt_vis_gen = vis_gen;
            s_qt_region_mask = region_mask;
            s_qt_proj_incomplete = proj_incomplete;
            s_qt_proj_retries = proj_incomplete ? (s_qt_proj_retries + 1) : 0;
        }
        // Fit frames ALL markers of the group → take the bbox from the index (not just the drawn subset).
        s_qt.bounds(minx, minz, maxx, maxz);

        // Visible world rect from the canvas corners (s2w flips Z → normalise). A node smaller than
        // kPilePx on screen collapses to one "×N" pile; sparse leaves in view draw individually.
        float wxa, wza, wxb, wzb;
        s2w(origin, wxa, wza);
        s2w(canvas_end, wxb, wzb);
        const float vMinX = (std::min)(wxa, wxb), vMaxX = (std::max)(wxa, wxb);
        const float vMinZ = (std::min)(wza, wzb), vMaxZ = (std::max)(wza, wzb);
        constexpr float kPilePx = 26.0f;
        const float clusterWorld = kPilePx / (s_zoom > 1e-6f ? s_zoom : 1e-6f);
        static std::vector<MarkerQuadtree::Pile> s_piles;
        static std::vector<const goblin::worldmap::Marker *> s_singles;
        s_piles.clear();
        s_singles.clear();
        s_qt.query(vMinX, vMinZ, vMaxX, vMaxZ, clusterWorld, s_piles, s_singles);

        // Cluster piles (drawn UNDER singles + graces): a disc sized ~log(count) + the count.
        for (const MarkerQuadtree::Pile &pl : s_piles)
        {
            ImVec2 ps = w2s(pl.cx, pl.cz);
            if (ps.x < origin.x || ps.x > canvas_end.x || ps.y < origin.y || ps.y > canvas_end.y) continue;
            float r = (7.0f + std::log2((float)pl.count) * 2.2f) * uiScale;
            if (r > 22.f * uiScale) r = 22.f * uiScale;
            dl->AddCircleFilled(ps, r, IM_COL32(40, 46, 60, 225));
            dl->AddCircle(ps, r, IM_COL32(230, 210, 140, 235), 0, 1.5f * uiScale);
            char cbuf[16];
            std::snprintf(cbuf, sizeof(cbuf), "%d", pl.count);
            ImVec2 ts = ImGui::CalcTextSize(cbuf);
            dl->AddText(ImVec2(ps.x - ts.x * 0.5f, ps.y - ts.y * 0.5f), IM_COL32(240, 235, 220, 255), cbuf);
            ++drawn;
            if (hovered)
            {
                float dx = ps.x - io.MousePos.x, dy = ps.y - io.MousePos.y;
                // Suppress the expand hint whenever ANY spiderfy fan is open (not just THIS pile's): a fan
                // is already showing, so "Ctrl+hover to expand" is wrong (with no-steal, hovering another
                // pile won't open it) and it leaks over the open fan's icons.
                if (dx * dx + dy * dy < r * r && !s_fan_open)
                    ImGui::SetTooltip("%d %s", pl.count,
                        (goblin::config::clusterSpiderfy && goblin::config::spiderfyHoldCtrl)
                            ? tr("markers — Ctrl+hover to expand, or zoom in")
                            : tr("markers — zoom in to expand"));
            }
        }
        // Non-grace singles (region-gated), then graces ON TOP (separate viewport-culled loop). Collect
        // each drawn single's screen pos so the spiderfy pass can fan EXACT/near-coincident ones (several
        // items on one loot spot never separate by zoom — the residual cluster case).
        static std::vector<std::pair<ImVec2, const goblin::worldmap::Marker *>> s_single_screen;
        s_single_screen.clear();
        for (const goblin::worldmap::Marker *m : s_singles)
            if (!region_gated(*m))
            {
                ImVec2 ps = w2s(m->worldX, m->worldZ);
                if (ps.x >= origin.x && ps.x <= canvas_end.x && ps.y >= origin.y && ps.y <= canvas_end.y)
                    s_single_screen.emplace_back(ps, m);
                plot(m->worldX, m->worldZ, m->color, m->category, m->name_id, nullptr, m->row_id, m->discover_flag, m);
            }
        for (const goblin::worldmap::Marker *m : s_grace_pts)
            if (!region_gated(*m) && m->worldX >= vMinX && m->worldX <= vMaxX &&
                m->worldZ >= vMinZ && m->worldZ <= vMaxZ)
                plot(m->worldX, m->worldZ, m->color, m->category, m->name_id, nullptr, m->row_id, m->discover_flag, m);

        // ── Spiderfy (hover fan-out) — piles AND coincident singles ──────────────────────────────────
        // Hover a cluster → its members fan around it (ring ≤12, else spiral) with legs, each fanned icon
        // hover/warp-able via the shared accumulator. Covers BOTH the quadtree piles (zoom-out clusters) and
        // exact/near-coincident SINGLES (the case zoom can't separate). Sticky on a world-quantized key so
        // it survives the per-frame rebuild; closes when the cursor leaves the fan extent. Gate: config flag.
        if (goblin::config::clusterSpiderfy && (hovered || s_force_spiderfy))
        {
            struct FanCluster { ImVec2 anchor; uint64_t key; int pileIdx; };  // pileIdx>=0 → gather from QT
            std::vector<FanCluster> clusters;
            auto qkey = [](float wx, float wz, uint64_t tag) -> uint64_t { return spiderfy_key(wx, wz, tag); };
            // Piles big enough on screen to be worth fanning (else "zoom in to expand" suffices).
            for (int i = 0; i < (int)s_piles.size(); ++i)
            {
                const MarkerQuadtree::Pile &pl = s_piles[i];
                ImVec2 ps = w2s(pl.cx, pl.cz);
                if (ps.x < origin.x || ps.x > canvas_end.x || ps.y < origin.y || ps.y > canvas_end.y) continue;
                clusters.push_back({ps, qkey(pl.cx, pl.cz, 1), i});
            }
            // Coincident singles: bucket by an icon-sized screen cell; groups of ≥2 = an unseparable pile.
            {
                const float K = (std::max)(6.0f, icoHalf);
                std::map<std::pair<int, int>, std::vector<const goblin::worldmap::Marker *>> buckets;
                for (auto &sp : s_single_screen)
                    buckets[{(int)std::floor(sp.first.x / K), (int)std::floor(sp.first.y / K)}].push_back(sp.second);
                for (auto &kv : buckets)
                    if (kv.second.size() >= 2)
                    {
                        const goblin::worldmap::Marker *m0 = kv.second.front();
                        clusters.push_back({w2s(m0->worldX, m0->worldZ), qkey(m0->worldX, m0->worldZ, 2), -1});
                    }
            }

            // Dev/test (vmap spiderfy 1): precise headless hover is unreliable, so force-open the fan on the
            // LARGEST visible pile to screenshot the geometry. No effect in normal use (flag off).
            if (s_force_spiderfy && !clusters.empty())
            {
                int best = -1, bestc = -1;
                for (int i = 0; i < (int)clusters.size(); ++i)
                    if (clusters[i].pileIdx >= 0 && s_piles[clusters[i].pileIdx].count > bestc)
                    { bestc = s_piles[clusters[i].pileIdx].count; best = i; }
                if (best >= 0) { s_fan_open = true; s_fan_key = clusters[best].key; }
            }
            // Hit-test the cursor against a cluster anchor → (re)latch the sticky key.
            for (const FanCluster &fc : clusters)
            {
                float dx = fc.anchor.x - io.MousePos.x, dy = fc.anchor.y - io.MousePos.y;
                const float hitR = 22.0f * uiScale;
                // Gate the OPEN on Ctrl (default) so panning/hovering the map doesn't pop fans as the
                // cursor sweeps clusters; once open the keep-open logic holds it without Ctrl.
                const bool mod_ok = !goblin::config::spiderfyHoldCtrl || io.KeyCtrl;
                // DON'T steal an already-open fan: only latch a NEW cluster when none is open. Otherwise
                // moving the cursor toward a fanned icon could re-latch to a neighbouring islet the path
                // passes over (fan A stops → fan B draws). The open fan closes via keep-open (cursor left
                // its reach) below, and only THEN can the next frame's hit-test open a different one.
                if (!s_fan_open && mod_ok && dx * dx + dy * dy <= hitR * hitR) { s_fan_open = true; s_fan_key = fc.key; break; }
            }
            // Resolve the open cluster + its members.
            const FanCluster *open = nullptr;
            for (const FanCluster &fc : clusters)
                if (s_fan_open && fc.key == s_fan_key) { open = &fc; break; }
            if (s_fan_open && !open) s_fan_open = false;   // the hovered cluster vanished (zoom/page changed)

            if (open)
            {
                std::vector<const goblin::worldmap::Marker *> members;
                if (open->pileIdx >= 0)
                    s_qt.gather_pile(s_piles[open->pileIdx], members);
                else
                {
                    // rebuild the coincident group at the open anchor (cheap; a handful of markers)
                    const float K = (std::max)(6.0f, icoHalf);
                    int bx = (int)std::floor(open->anchor.x / K), by = (int)std::floor(open->anchor.y / K);
                    for (auto &sp : s_single_screen)
                        if ((int)std::floor(sp.first.x / K) == bx && (int)std::floor(sp.first.y / K) == by)
                            members.push_back(sp.second);
                }
                // Dedup identical members (same name/category/icon) → one icon + an ×N badge. When
                // collected_graying is on, DROP collected/cleared members: draw_marker_glyph hides them
                // anyway (blank fan slots), so keeping them only bloats a big fan → less churn, and the
                // fan reads as an action list of what's still LEFT to grab/kill (user request 2026-07-05).
                const bool drop_done = goblin::config::collectedGraying;
                struct FE { const goblin::worldmap::Marker *m; int n; };
                std::vector<FE> ents;
                {
                    std::unordered_map<uint64_t, size_t> seen;
                    for (auto *m : members)
                    {
                        if (drop_done && goblin::worldmap::marker_is_done(*m)) continue;
                        uint64_t k = ((uint64_t)(uint32_t)m->name_id << 32) ^
                                     ((uint64_t)(uint32_t)m->category << 8) ^
                                     ((uintptr_t)m->icon_key & 0xFFu);
                        auto it = seen.find(k);
                        if (it == seen.end()) { seen.emplace(k, ents.size()); ents.push_back({m, 1}); }
                        else ents[it->second].n++;
                    }
                }
                const int n_total = (int)ents.size();
                constexpr int kFanMax = 40;
                const int n = n_total > kFanMax ? kFanMax : n_total;
                if (n >= 2)
                {
                    const ImVec2 c = open->anchor;
                    const float spacing = 2.0f * icoHalf + 8.0f * uiScale;
                    const float baseR = icoHalf * 2.4f;
                    std::vector<ImVec2> pos(n);
                    float max_r = baseR;
                    if (n <= 12)
                    {
                        float r = spacing * n / 6.2831853f;
                        if (r < baseR) r = baseR;
                        max_r = r;
                        for (int k = 0; k < n; ++k)
                        { float a = -1.5707963f + 6.2831853f * k / n; pos[k] = ImVec2(c.x + r * std::cos(a), c.y + r * std::sin(a)); }
                    }
                    else
                    {
                        float r = baseR, a = -1.5707963f;
                        for (int k = 0; k < n; ++k)
                        { pos[k] = ImVec2(c.x + r * std::cos(a), c.y + r * std::sin(a)); max_r = (std::max)(max_r, r); a += spacing / r; r += (spacing / 6.2831853f * 1.35f) * (spacing / r); }
                    }
                    // Keep-open: close once the cursor leaves the fan's reach (glyph→leg→icon travel allowed).
                    const float keepR = max_r + icoHalf + 24.0f * uiScale;
                    float mdx = c.x - io.MousePos.x, mdy = c.y - io.MousePos.y;
                    if (!s_force_spiderfy && mdx * mdx + mdy * mdy > keepR * keepR) s_fan_open = false;
                    // MODAL absorb: the fan is drawn on top, so inside its backdrop it must OWN the hover —
                    // clear whatever base marker/pile UNDER it set in the accumulator, so their tooltip/warp
                    // can't leak through the gaps between fanned icons. Only a fanned icon (below) re-sets it.
                    const float absorbR = max_r + icoHalf + 6.0f * uiScale;
                    if (mdx * mdx + mdy * mdy <= absorbR * absorbR)
                    {
                        hoverBestD = 1e18f; hoverPrio = -1; hoverName = -1; hoverCat = -1;
                        hoverV.clear(); hoverRow = 0; hoverDisc = 0;
                    }
                    dl->AddCircleFilled(c, max_r + icoHalf + 6.0f * uiScale, IM_COL32(18, 22, 30, 150));
                    for (int k = 0; k < n; ++k)
                    {
                        dl->AddLine(c, pos[k], IM_COL32(205, 195, 145, 180), 1.3f * uiScale);
                        const goblin::worldmap::Marker *m = ents[k].m;
                        goblin::worldmap::draw_marker_glyph(dl, *m, pos[k].x, pos[k].y, ctx.atlas_srv, nativeIcons, icoHalf);
                        if (ents[k].n > 1)
                        { char b[8]; std::snprintf(b, sizeof(b), "x%d", ents[k].n);
                          dl->AddText(ImVec2(pos[k].x + icoHalf * 0.4f, pos[k].y - icoHalf), IM_COL32(255, 230, 150, 255), b); }
                        // Hover a fanned icon → feed the shared accumulator (prio 2 wins) so the existing
                        // tooltip + grace double-click-warp path (below) fires for it.
                        float fdx = pos[k].x - io.MousePos.x, fdy = pos[k].y - io.MousePos.y;
                        if (fdx * fdx + fdy * fdy < icoHalf * icoHalf * 2.0f)
                        {
                            hoverBestD = 0.f; hoverPrio = 2; hoverName = m->name_id; hoverCat = m->category;
                            hoverV = ""; hoverRow = m->row_id; hoverDisc = m->discover_flag;
                            hoverWx = m->worldX; hoverWz = m->worldZ; hoverArea = m->raw_area;
                        }
                    }
                    if (n_total > n)
                    { char b[16]; std::snprintf(b, sizeof(b), "+%d", n_total - n);
                      dl->AddText(ImVec2(c.x + max_r, c.y), IM_COL32(235, 220, 200, 255), b); }
                }
            }
        }
    }
    else
    {
        goblin::vworld::World w;
        if (goblin::vworld::get_world(active_world, w))
            for (const goblin::vworld::Marker &m : w.markers)
                plot(m.x + w.originX, m.z + w.originZ, m.color, m.category, -1, m.name.c_str(), 0, 0, nullptr);  // C1
    }
    // Tooltip for the hovered marker (name via FMG + category label). A grace (has a warp rowId) also
    // offers double-click → fast-travel (the make-or-break map feature).
    const bool hoverGrace = (hoverCat == kGraceCat && hoverRow != 0);
    // ONLY discovered graces are warpable — the layer holds discovered AND undiscovered graces, and warping
    // to an undiscovered one hangs on an infinite loading screen. Discovered = its discovery event flag set.
    const bool graceDiscovered = hoverGrace && hoverDisc > 0 && goblin::overlay_api::read_event_flag((uint32_t)hoverDisc);
    if (hoverBestD < 1e17f)
    {
        std::string nm = !hoverV.empty() ? hoverV
                         : (hoverName > 0 ? goblin::overlay_api::lookup_text_utf8(hoverName) : std::string());
        const char *clab = hoverCat >= 0 ? goblin::overlay_api::category_label(hoverCat) : nullptr;
        if (!nm.empty() || clab || hoverGrace)
        {
            ImGui::BeginTooltip();
            if (!nm.empty()) ImGui::TextUnformatted(nm.c_str());
            else ImGui::TextDisabled("(unnamed)");
            if (clab) ImGui::TextDisabled("%s", clab);
            if (hoverGrace)
            {
                graceDiscovered ? ImGui::TextColored(ImVec4(0.90f, 0.78f, 0.35f, 1.f), "double-click to warp")
                                : ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.f), "not discovered — cannot warp");
                // Dev: warp rowId + the grace's REAL area, so a bad grace→rowId mapping shows on hover
                // (a base grace — area 60 — carrying a DLC rowId, or an area-61 grace shown in group 0).
                ImGui::TextDisabled("rowId %llu · area %d · group %d", (unsigned long long)hoverRow,
                                    hoverArea, s_group);
            }
            ImGui::EndTooltip();
        }
        // Double-click a DISCOVERED grace → fast-travel. Deferred to next frame's top (warp tears down UI
        // state; firing mid-ImGui-draw is unsafe). Double-click avoids the drag-pan click conflict.
        if (graceDiscovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            s_warp_pending = hoverRow;
            // Log WHAT we're warping to — the name + rowId + world pos let us catch a bad grace→rowId
            // mapping (e.g. a base-overworld grace carrying a DLC BonfireWarpParam rowId → area-61 stall).
            std::string gname = !hoverV.empty() ? hoverV
                                : (hoverName > 0 ? goblin::overlay_api::lookup_text_utf8(hoverName) : std::string("?"));
            spdlog::info("[VMAP] warp queued: grace '{}' rowId={} area={} discFlag={} nameId={} group={} worldXZ=({:.0f},{:.0f})",
                         gname, hoverRow, hoverArea, hoverDisc, hoverName, s_group, hoverWx, hoverWz);
        }
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
        dl->AddCircleFilled(o, 4.0f * uiScale, IM_COL32(220, 180, 90, 255));
        dl->AddText(ImVec2(o.x + 6 * uiScale, o.y + 4 * uiScale), IM_COL32(220, 180, 90, 255), "0,0");
    }
    // Player position marker + heading arrow (A11 parity — the native minimap has this; the vmap must
    // too before it can replace the native map). Base ER only, and only when the player's group matches
    // Item-search RESULT marks — a persistent orange diamond on every matching instance of the current query
    // (distinct from the blue custom pins + yellow item icons), for the active group. Drawn ON TOP of markers
    // and independent of the visibility gate, so all results stay visible at once until the next search/Clear.
    if (active_world == 0)
    {
        for (const goblin::search_marks::Mark &sm : goblin::search_marks::snapshot())
        {
            if (sm.group != s_group) continue;
            ImVec2 p = w2s(sm.wx, sm.wz);
            if (p.x < origin.x || p.x > canvas_end.x || p.y < origin.y || p.y > canvas_end.y) continue;
            const float r = 6.0f * uiScale;
            const ImVec2 d0(p.x, p.y - r), d1(p.x + r, p.y), d2(p.x, p.y + r), d3(p.x - r, p.y);
            dl->AddQuadFilled(d0, d1, d2, d3, IM_COL32(255, 140, 20, 235));
            dl->AddQuad(d0, d1, d2, d3, IM_COL32(35, 20, 0, 235), 1.5f * uiScale);
        }
    }

    // Item-search LOCATE highlight — a pulsing ring at every match on the page, drawn ON TOP of all markers
    // and INDEPENDENT of the visibility gate, so a search hit is visible even with markers toggled off (and
    // gives the "found it" feedback the plain re-centre lacked). Only for Base-ER (locate is Base-ER-only).
    if (active_world == 0 && !s_locate_pts.empty())
    {
        const double now = ImGui::GetTime();
        if (s_locate_arm) { s_locate_until = now + 6.0; s_locate_arm = false; }  // 6 s of pulse per locate
        if (now < s_locate_until)
        {
            const float pulse = 0.5f + 0.5f * std::sin((float)now * 6.0f);       // 0..1 breathe
            const float fade = (float)((std::min)(1.0, (s_locate_until - now) / 1.0));  // fade the last second
            const int a = (int)(220.0f * fade);
            for (const ImVec2 &wp : s_locate_pts)
            {
                ImVec2 ps = w2s(wp.x, wp.y);
                if (ps.x < origin.x || ps.x > canvas_end.x || ps.y < origin.y || ps.y > canvas_end.y) continue;
                const float r = (13.0f + pulse * 10.0f) * uiScale;
                dl->AddCircle(ps, r, IM_COL32(255, 220, 90, a), 0, 2.5f * uiScale);
                dl->AddCircle(ps, r + 3.0f * uiScale, IM_COL32(255, 220, 90, a / 2), 0, 1.5f * uiScale);
            }
        }
        else
            s_locate_pts.clear();   // expired → stop drawing
    }

    // the displayed group (underground overlaps the overworld in XZ, so a cross-group dot would mislead).
    if (active_world == 0)
    {
        int parea = 0, pgroup = 0;
        float pwx = 0.f, pwz = 0.f;
        if (goblin::overlay_api::get_player_map_pos(parea, pwx, pwz, nullptr, nullptr, &pgroup) &&
            pgroup == s_group)
        {
            ImVec2 pp = w2s(pwx, pwz);
            if (pp.x >= origin.x && pp.x <= canvas_end.x && pp.y >= origin.y && pp.y <= canvas_end.y)
            {
                // Heading arrow when the yaw resolves, else a plain dot. Same convention as the minimap
                // (a = yaw + π), but expressed in WORLD dir then run through the vmap's axis signs so it
                // stays correct if the canvas is flipped: world fwd = (sin a, cos a); screen dir =
                // (s_sx·fwd_x, s_sz·fwd_z) → matches the minimap's (sin a, −cos a) at the default signs.
                float yaw = 0.f;
                if (goblin::overlay_api::get_player_facing_yaw(yaw))
                {
                    const float a = yaw + 3.14159265f;
                    float fx = s_sx * std::sin(a), fz = s_sz * std::cos(a);
                    const float len = std::sqrt(fx * fx + fz * fz);
                    if (len > 1e-4f) { fx /= len; fz /= len; }
                    const ImVec2 fwd(fx, fz), rgt(-fwd.y, fwd.x);
                    // NATIVE player cursor = MENU_MAP_Player_01 (arrow+circle) rotated to face yaw, same as
                    // the minimap. Tall sprite (72x150) → keep aspect. Red-triangle fallback if unresolved.
                    void *pt = nullptr; float pu0, pv0, pu1, pv1;
                    if (goblin::overlay_api::map_point_glyph_uv("MENU_MAP_Player_01", -1, pt, pu0, pv0, pu1, pv1) && pt)
                    {
                        const float hh = 16.0f * uiScale, hw = hh * (72.0f / 150.0f);
                        // Cool glow halo + bright tint so the yellow player pin stands out from the yellow markers.
                        dl->AddCircleFilled(pp, hh * 0.9f, IM_COL32(40, 130, 255, 95));
                        const ImVec2 tl(pp.x - rgt.x * hw + fwd.x * hh, pp.y - rgt.y * hw + fwd.y * hh);
                        const ImVec2 tr(pp.x + rgt.x * hw + fwd.x * hh, pp.y + rgt.y * hw + fwd.y * hh);
                        const ImVec2 br(pp.x + rgt.x * hw - fwd.x * hh, pp.y + rgt.y * hw - fwd.y * hh);
                        const ImVec2 bl(pp.x - rgt.x * hw - fwd.x * hh, pp.y - rgt.y * hw - fwd.y * hh);
                        dl->AddImageQuad((ImTextureID)pt, tl, tr, br, bl, ImVec2(pu0, pv0), ImVec2(pu1, pv0),
                                         ImVec2(pu1, pv1), ImVec2(pu0, pv1), IM_COL32(255, 255, 210, 255));
                    }
                    else
                    {
                        const float L = 11.f * uiScale, B = 6.f * uiScale;
                        const ImVec2 tip(pp.x + fwd.x * L, pp.y + fwd.y * L);
                        const ImVec2 bl(pp.x - fwd.x * B + rgt.x * B, pp.y - fwd.y * B + rgt.y * B);
                        const ImVec2 br(pp.x - fwd.x * B - rgt.x * B, pp.y - fwd.y * B - rgt.y * B);
                        dl->AddTriangleFilled(tip, bl, br, IM_COL32(255, 48, 48, 255));
                        dl->AddTriangle(tip, bl, br, IM_COL32(255, 255, 255, 235), 1.5f * uiScale);
                    }
                }
                else
                {
                    dl->AddCircleFilled(pp, 5.0f * uiScale, IM_COL32(255, 48, 48, 255));
                    dl->AddCircle(pp, 5.0f * uiScale, IM_COL32(255, 255, 255, 235), 0, 1.5f * uiScale);
                }
            }
        }
    }

    // Custom player-placed markers — drawn ON TOP of every icon (like the player dot): a colored pin
    // (teardrop) + name, for the markers tagged to the displayed group. Right-click the canvas to drop one.
    if (active_world == 0)
    {
        for (const goblin::custom_markers::Marker &c : goblin::custom_markers::snapshot())
        {
            if (c.group != s_group) continue;
            ImVec2 p = w2s(c.wx, c.wz);
            if (p.x < origin.x || p.x > canvas_end.x || p.y < origin.y || p.y > canvas_end.y) continue;
            const float r = 8.0f * uiScale;
            const ImVec2 head(p.x, p.y - r * 1.3f);
            dl->AddTriangleFilled(ImVec2(p.x - r * 0.7f, p.y - r * 1.3f), ImVec2(p.x + r * 0.7f, p.y - r * 1.3f), p, c.color);
            dl->AddCircleFilled(head, r * 0.7f, c.color);
            dl->AddCircle(head, r * 0.7f, IM_COL32(255, 255, 255, 240), 0, 1.5f * uiScale);
            dl->AddText(ImVec2(p.x + 7.0f * uiScale, p.y - r * 1.3f - 8.0f * uiScale), IM_COL32(230, 240, 255, 255), c.name.c_str());
        }
        // Death marker (dropped runes / bloodstain) — the NATIVE MENU_MAP_DropSoul sprite from the resident
        // map-symbol sheet (mod-agnostic; same resolve path as the grace glyph). Falls back to a red disc
        // until the sprite is resident (map opened once).
        float dwx, dwz; int dgrp, dsouls;
        if (goblin::death_marker::get(dwx, dwz, dgrp, dsouls) && dgrp == s_group)
        {
            ImVec2 p = w2s(dwx, dwz);
            if (p.x >= origin.x && p.x <= canvas_end.x && p.y >= origin.y && p.y <= canvas_end.y)
            {
                void *tex = nullptr; float u0, v0, u1, v1; int nw = 0, nh = 0;
                const float h = 11.0f * uiScale;
                if (goblin::overlay_api::map_point_glyph_uv("MENU_MAP_DropSoul", -1, tex, u0, v0, u1, v1, &nw, &nh) && tex)
                    dl->AddImage((ImTextureID)tex, ImVec2(p.x - h, p.y - h), ImVec2(p.x + h, p.y + h),
                                 ImVec2(u0, v0), ImVec2(u1, v1));
                else
                {
                    dl->AddCircleFilled(p, 6.0f * uiScale, IM_COL32(200, 60, 60, 235));
                    dl->AddCircle(p, 6.0f * uiScale, IM_COL32(255, 255, 255, 235), 0, 1.5f * uiScale);
                }
                // Hover tooltip (drawn after the marker loop's tooltip, so it wins over the icon).
                const ImVec2 mp = ImGui::GetIO().MousePos;
                if (hovered && std::fabs(mp.x - p.x) <= h && std::fabs(mp.y - p.y) <= h)
                    ImGui::SetTooltip("%s — %d %s", tr("Bloodstain"), dsouls, tr("runes"));
            }
        }
    }

    // Region name labels (A7 parity — the coarse major-region names Limgrave/Caelid/… the native map
    // draws). Base ER only (custom worlds carry no ER regions) and only anchors on the displayed group
    // (underground overlaps the overworld in XZ). Reuses the world-space projection precomputed above so
    // the label rides the same pan/zoom as its region's markers. CLICKABLE, parity with the native chip:
    // a click toggles that region off — which hides its CLUTTER markers (loot/landmarks/bosses) while
    // graces + the player stay visible (see region_hide_exempt above). The on/off flag is the SHARED
    // map_renderer state, so a toggle syncs with the native map and persists via config::regionToggles.
    // Drawn last-but-one so names sit over the markers.
    if (active_world == 0 && s_show_labels)
    {
        ImFont *font = ImGui::GetFont();
        const float fontSize = ImGui::GetFontSize() * 1.4f * uiScale;
        const bool clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        for (int i = 0; i < regN && i < kRegCap; ++i)
        {
            if (!regValid[i] || regGrp[i] != s_group)
                continue;
            const char *name = goblin::generated::MAJOR_REGION_ANCHORS[i].name;
            ImVec2 p = w2s(regWx[i], regWz[i]);
            if (p.x < origin.x - 64 || p.x > canvas_end.x + 64 ||
                p.y < origin.y - 32 || p.y > canvas_end.y + 32)
                continue;
            ImVec2 ts = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, name);
            ImVec2 tp(p.x - ts.x * 0.5f, p.y - ts.y * 0.5f);
            const float pad = 5.f * uiScale;
            ImVec2 r0(tp.x - pad, tp.y - pad), r1(tp.x + ts.x + pad, tp.y + ts.y + pad);
            const bool hot = io.MousePos.x >= r0.x && io.MousePos.x <= r1.x &&
                             io.MousePos.y >= r0.y && io.MousePos.y <= r1.y;
            if (hot && clicked)
                goblin::worldmap::region_set_enabled(i, !goblin::worldmap::region_enabled(i));
            const bool on = goblin::worldmap::region_enabled(i);
            // Pill (warmer when hovered, reddish when off) + hover outline + text (gold on / dim off) +
            // a strike-through when off — same visual language as the native draw_region_labels chip.
            const ImU32 bg = on ? IM_COL32(30, 26, 18, hot ? 175 : 120)
                                : IM_COL32(46, 22, 22, hot ? 175 : 120);
            dl->AddRectFilled(r0, r1, bg, 4.f);
            if (hot)
                dl->AddRect(r0, r1, IM_COL32(238, 226, 188, 220), 4.f, 0, 1.5f);
            const ImU32 col = on ? IM_COL32(238, 226, 188, 235) : IM_COL32(150, 140, 120, 160);
            dl->AddText(font, fontSize, ImVec2(tp.x + 1.5f * uiScale, tp.y + 1.5f * uiScale), IM_COL32(0, 0, 0, 190), name);
            dl->AddText(font, fontSize, tp, col, name);
            if (!on)
                dl->AddLine(ImVec2(tp.x, p.y), ImVec2(tp.x + ts.x, p.y), col, 2.0f * uiScale);
        }
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
