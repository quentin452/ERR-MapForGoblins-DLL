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
#include "worldmap/map_entry_layer.hpp" // far_relief_snapshot/step/built_group (relief draw, post split resync)
#include "worldmap/loot_disk.hpp"       // read_game_file_decompressed (vmap emevd RE probe)
#include "worldmap/msbe_parser.hpp"     // msbe::emevd_inits (vmap emevd RE probe)
#include "worldmap/map_renderer.hpp"   // draw_marker_glyph — reuse the native state-aware per-marker draw
#include "goblin_config.hpp"           // config::clusterSpiderfy — hover fan-out gate (spiderfy)
#include "worldmap/maptile.hpp"        // maptile ART reader (endgame phase-1a slice 2/3)
#include "goblin_worldmap_probe.hpp"   // live converter affine + map-space extent (slice 3 tile placement)
#include "goblin_virtual_world.hpp"    // vworld registry — the active custom world's markers (slice C)
#include "goblin_inject.hpp"           // goblin::world_map_open() + marker_group_from (slice D / A7)
#include "goblin_major_regions.hpp"    // MAJOR_REGION_ANCHORS — coarse region name labels (A7 parity)
#include "goblin_bench.hpp"            // GOBLIN_BENCH_QUIET — profile the vmap draw at high marker counts
#include "goblin_custom_markers.hpp"   // shared player-placed marker store (vmap + minimap read it)
#include "goblin_coop.hpp"             // coop::markers() — co-op partner blips (MENU_MAP_Host, no rotation)
#include "goblin/goblin_map_flags.hpp" // story-state flags (Erdtree burns / charm / seal) — item-search Royal vs Ashen
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
    bool s_from_map = false;  // vmap was opened by the game MAP KEY (→ draw fullscreen over the native map)
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
    float s_items_w = 320.0f;          // its resizable width (wide enough for the Royal/Ashen state tag)
    char s_item_filter[64] = "";       // file-scope so a dev RPC (vmap items <q>) can drive it for tests
    // Auto-name for a new pin: the SMALLEST FREE "Marker N" (N>=1) across the store, recomputed at each
    // add. A plain ++ counter never came back down when a pin was deleted, so after deleting "Marker 3"
    // the next pin was still "Marker 7" (user 2026-07-28). Scanning is O(pins) with a 24/group cap.
    std::string next_marker_name()
    {
        std::vector<bool> used;
        for (const goblin::custom_markers::Marker &m : goblin::custom_markers::snapshot())
        {
            if (m.name.compare(0, 7, "Marker ") != 0) continue;
            char *end = nullptr;
            const char *digits = m.name.c_str() + 7;
            const long n = std::strtol(digits, &end, 10);
            if (end == digits || n < 1 || n >= 4096) continue;   // "Marker foo" / out of range → not a slot
            if ((long)used.size() <= n) used.resize((size_t)n + 1, false);
            used[(size_t)n] = true;
        }
        int n = 1;
        while (n < (int)used.size() && used[(size_t)n]) ++n;
        return std::string("Marker ") + std::to_string(n);
    }
    char s_grace_filter[64] = "";   // grace-list search box
    // Grace list cache (rebuilt on open / Refresh; discovered-state re-read live per visible row).
    struct GraceRow { std::string name; float wx, wz; uint64_t rowId; int discFlag; int group;
                      std::string region; int regionIdx;
                      bool hub; };   // hub = a grace with no place in the Voronoi (the Roundtable tile)
    std::vector<GraceRow> s_graces;
    bool s_graces_built = false;
    uint64_t s_locate_grace_row = 0;   // "locate" target, resolved to its LIVE position after the QT build
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

// True when the vmap is open AND standing in FULLSCREEN for the native map (opened by the game map
// key with vmap_on_map_key). In that state it draws opaque over the native-map marker pass, so that
// pass is skipped (map_renderer::render_markers early-returns) to avoid computing hidden markers.
bool virtual_map_fullscreen() { return s_open && s_from_map; }

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
// Dump every grace marker (name / dimension group / discovered / warp rowId) to the log — a warp-target
// list for driving the game (the rowId is what `warp <id>` + the vmap double-click both take). `groupf`
// -1 = all, else only that dimension group (0 OW / 1 base-UG / 2 DLC). Discovered graces are warpable;
// undiscovered warp hangs on an infinite load. RPC `vmap graces [group]`.
std::string virtual_map_graces_dump(int groupf)
{
    const int kGraceCat = static_cast<int>(goblin::generated::Category::WorldGraces);
    int total = 0, disc = 0, shown = 0;
    int byGroupDisc[4] = {0, 0, 0, 0};
    for (auto *L : overlay_layers())
    {
        if (!L) continue;
        for (const goblin::worldmap::Marker &m : L->markers())
        {
            if (m.category != kGraceCat || m.row_id == 0) continue;
            ++total;
            // Discovered = the discovery event flag is SET, read LIVE — same check the warp gate
            // uses (line ~2084). discover_flag alone is the flag ID (nonzero for EVERY grace), so
            // testing it != 0 counted all graces as discovered (bug caught by the user, 2026-07-07).
            const bool d = m.discover_flag > 0 &&
                           goblin::overlay_api::read_event_flag((uint32_t)m.discover_flag);
            if (d) ++disc;
            const int g = (m.group >= 0 && m.group < 4) ? m.group : 0;
            if (d) byGroupDisc[g]++;
            if (groupf >= 0 && g != groupf) continue;
            if (!d) continue;                         // list only DISCOVERED (warpable) graces
            {
                std::string nm = m.name_id > 0 ? goblin::overlay_api::lookup_text_utf8(m.name_id) : std::string();
                spdlog::info("[VMGRACES]   g{} rowId={} '{}' w({:.0f},{:.0f})", g, m.row_id,
                             nm.empty() ? "(unnamed)" : nm.c_str(), m.worldX, m.worldZ);
            }
            ++shown;
        }
    }
    spdlog::info("[VMGRACES] {} graces ({} discovered) | discovered by group: OW={} UG={} DLC={} — listed {} (filter group={})",
                 total, disc, byGroupDisc[0], byGroupDisc[1], byGroupDisc[2], shown, groupf);
    char out[176];
    std::snprintf(out, sizeof(out), "ok vmap graces: %d total, %d discovered (OW=%d UG=%d DLC=%d), listed %d — see [VMGRACES] log",
                  total, disc, byGroupDisc[0], byGroupDisc[1], byGroupDisc[2], shown);
    return out;
}

std::string virtual_map_offmap_probe()
{
    // Gross test: origin-zero or way past any map extent.
    auto crude_bad = [](float x, float z) {
        return (x == 0.f && z == 0.f) || x < -40000.f || x > 40000.f || z < -40000.f || z > 40000.f;
    };
    // In-frame MARGIN test: the crude ±40000/(0,0) gate misses a marker that lands inside the frame but
    // well outside where its page's markers actually cluster — a degenerate margin dupe (the Banished
    // Knight Engvall type-1 at area60 grid(13,9) = world(2856,2366), below the populated overworld; the
    // crude test called it "onmap"). Derive each page's (group's) valid extent LIVE from the bulk of its
    // OWN markers (robust 0.5%/99.5% percentiles, padded) so it stays mod-agnostic — no hardcoded ER grid
    // range — then flag in-frame outliers as MARGIN. Needs enough markers to trust a bound.
    std::vector<float> gxs[4], gzs[4];
    for (auto *L : overlay_layers())
    {
        if (!L) continue;
        for (const goblin::worldmap::Marker &m : L->markers())
        {
            if (crude_bad(m.worldX, m.worldZ)) continue;
            int g = (m.group >= 0 && m.group < 4) ? m.group : 0;
            gxs[g].push_back(m.worldX); gzs[g].push_back(m.worldZ);
        }
    }
    float xlo[4] = {0}, xhi[4] = {0}, zlo[4] = {0}, zhi[4] = {0};
    bool have_bounds[4] = {false, false, false, false};
    auto pct = [](std::vector<float> &v, float p) {
        std::sort(v.begin(), v.end());
        return v[(size_t)(p * (v.size() - 1))];
    };
    for (int g = 0; g < 4; ++g)
    {
        if (gxs[g].size() < 64) continue;   // too few to trust a bound (sparse/custom page) — no margin test
        float x0 = pct(gxs[g], 0.005f), x1 = pct(gxs[g], 0.995f);
        float z0 = pct(gzs[g], 0.005f), z1 = pct(gzs[g], 0.995f);
        float padx = (x1 - x0) * 0.05f + 512.f, padz = (z1 - z0) * 0.05f + 512.f;
        xlo[g] = x0 - padx; xhi[g] = x1 + padx; zlo[g] = z0 - padz; zhi[g] = z1 + padz;
        have_bounds[g] = true;
    }
    auto margin_bad = [&](const goblin::worldmap::Marker &m) {
        int g = (m.group >= 0 && m.group < 4) ? m.group : 0;
        if (!have_bounds[g]) return false;
        return m.worldX < xlo[g] || m.worldX > xhi[g] || m.worldZ < zlo[g] || m.worldZ > zhi[g];
    };

    size_t total = 0, off = 0, zero = 0, oob = 0, marg = 0;
    std::map<int, int> byArea, byCat;   // raw ER area -> count, category -> count
    std::vector<std::string> samples;
    for (auto *L : overlay_layers())
    {
        if (!L) continue;
        for (const goblin::worldmap::Marker &m : L->markers())
        {
            ++total;
            const bool z0 = (m.worldX == 0.f && m.worldZ == 0.f);
            const bool crude = crude_bad(m.worldX, m.worldZ);
            const bool mg = !crude && margin_bad(m);
            if (!crude && !mg) continue;
            ++off;
            const char *kind;
            if (z0) { ++zero; kind = "ZERO"; }
            else if (crude) { ++oob; kind = "OOB"; }
            else { ++marg; kind = "MARGIN"; }
            byArea[m.raw_area]++; byCat[m.category]++;
            if (samples.size() < 25)
            {
                std::string nm = m.name_id >= 0 ? goblin::overlay_api::lookup_text_utf8(m.name_id) : std::string();
                const char *cl = goblin::overlay_api::category_label(m.category);
                char b[208];
                std::snprintf(b, sizeof(b), "'%s' area%d grid(%d,%d) g%d [%s] w(%.0f,%.0f) %s",
                              nm.c_str(), m.raw_area, m.raw_gx, m.raw_gz, m.group, cl ? cl : "?",
                              m.worldX, m.worldZ, kind);
                samples.push_back(b);
            }
        }
    }
    spdlog::info("[OFFMAP] {} off-map of {} markers ({} origin-zero, {} out-of-range, {} in-frame margin)",
                 off, total, zero, oob, marg);
    { std::string s; for (auto &kv : byArea) { char b[24]; std::snprintf(b, sizeof(b), " area%d:%d", kv.first, kv.second); s += b; }
      spdlog::info("[OFFMAP] by raw ER area (which map):{}", s.empty() ? " (none)" : s.c_str()); }
    { std::string s; for (auto &kv : byCat) { char b[32]; const char *cl = goblin::overlay_api::category_label(kv.first);
        std::snprintf(b, sizeof(b), " %s:%d", cl ? cl : "?", kv.second); s += b; }
      spdlog::info("[OFFMAP] by category:{}", s.empty() ? " (none)" : s.c_str()); }
    for (auto &s : samples) spdlog::info("[OFFMAP]   {}", s);
    char out[208];
    std::snprintf(out, sizeof(out), "ok offmap: %zu off-map of %zu markers (%zu origin-zero, %zu out-of-range, %zu margin) — see [OFFMAP] log",
                  off, total, zero, oob, marg);
    return out;
}

// By-name marker SOURCE finder: dump EVERY marker whose resolved name matches `query`, across all
// layers, with its provenance (layer index, source, lotId/type, raw area/grid, world pos, off-map?).
// The tool for "why is X on the map twice / off-map" — two hits with the SAME lotId+layer = one entity
// double-emitted (fold/pass bug); DIFFERENT lotId/layer = a genuine extra source. `query` matches a
// numeric name_id exactly, else a case-insensitive substring of the marker's in-game text. RPC `vmap find`.
std::string virtual_map_find(const std::string &query)
{
    if (query.empty()) return "err usage: vmap find <name|name_id>";
    // Numeric query → exact name_id match; else case-insensitive substring on the resolved text.
    int32_t want_id = 0; bool by_id = false;
    try { size_t used = 0; want_id = (int32_t)std::stol(query, &used, 0); by_id = (used == query.size()); }
    catch (...) { by_id = false; }
    auto lower = [](std::string s) { for (char &c : s) c = (char)std::tolower((unsigned char)c); return s; };
    const std::string needle = lower(query);
    auto srclabel = [](goblin::worldmap::Source s) {
        switch (s) { case goblin::worldmap::Source::Baked: return "Baked";
                     case goblin::worldmap::Source::DiskMSB: return "DiskMSB";
                     case goblin::worldmap::Source::Live: return "Live"; }
        return "?";
    };
    auto implausible = [](float x, float z) {
        return (x == 0.f && z == 0.f) || x < -40000.f || x > 40000.f || z < -40000.f || z > 40000.f;
    };
    const char *const kGrp[4] = {"OW", "UG", "DLC", "DLC-UG"};
    size_t hits = 0; int li = -1;
    spdlog::info("[VMFIND] query='{}' ({})", query, by_id ? "name_id" : "substr");
    for (auto *L : overlay_layers())
    {
        ++li;
        if (!L) continue;
        for (const goblin::worldmap::Marker &m : L->markers())
        {
            std::string nm = m.name_id >= 0 ? goblin::overlay_api::lookup_text_utf8(m.name_id) : std::string();
            bool match = by_id ? (m.name_id == want_id) : (!nm.empty() && lower(nm).find(needle) != std::string::npos);
            if (!match) continue;
            ++hits;
            const char *cl = goblin::overlay_api::category_label(m.category);
            const bool offmap = implausible(m.worldX, m.worldZ);
            spdlog::info("[VMFIND]   '{}' [L{}] src={} lot={}/{} area{} grid({},{}) g{}({}) [{}] w({:.0f},{:.0f}) {}",
                         nm, li, srclabel(m.source), m.lotId, (int)m.lotType, m.raw_area, m.raw_gx, m.raw_gz,
                         m.group, (m.group >= 0 && m.group < 4) ? kGrp[m.group] : "?", cl ? cl : "?",
                         m.worldX, m.worldZ, offmap ? "OFFMAP" : "onmap");
        }
    }
    spdlog::info("[VMFIND] {} match(es)", hits);
    char out[128];
    std::snprintf(out, sizeof(out), "ok vmap find '%s': %zu match(es) — see [VMFIND] log", query.c_str(), hits);
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

// Marker extractor: log every marker whose world XZ falls inside [wminx..wmaxx]×[wminz..wmaxz], each line
// tagged with a crude off-map flag (origin0 = piled at 0,0 / OOB = |coord|>40000 / ok). A debug aid to
// triage markers that land off the map artwork (user 2026-07-23). Reuses the dump_markers_csv columns and
// the offmap_probe crude test; goes to logs/MapForGoblins.log via spdlog. Returns the count logged.
int extract_region_log(float wminx, float wmaxx, float wminz, float wmaxz)
{
    spdlog::info("[VMEXTRACT] region worldX[{:.0f}..{:.0f}] worldZ[{:.0f}..{:.0f}] — cols: offmap,group,category,srcArea,raw_area,worldX,worldZ,name_id",
                 wminx, wmaxx, wminz, wmaxz);
    int n = 0, off = 0;
    for (auto *L : overlay_layers())
    {
        if (!L) continue;
        for (const goblin::worldmap::Marker &m : L->markers())
        {
            if (m.worldX < wminx || m.worldX > wmaxx || m.worldZ < wminz || m.worldZ > wmaxz) continue;
            const bool o0 = (m.worldX == 0.0f && m.worldZ == 0.0f);
            // Real ER markers sit thousands of units from origin; anything within ~200u is almost certainly
            // a projection FAILURE dumped near (0,0) — the actual "off-map" case the crude origin0/OOB test
            // missed (user 2026-07-23: selected off-map markers all logged "ok").
            const bool near0 = !o0 && (m.worldX > -200.f && m.worldX < 200.f && m.worldZ > -200.f && m.worldZ < 200.f);
            const bool oob = (m.worldX < -40000.f || m.worldX > 40000.f || m.worldZ < -40000.f || m.worldZ > 40000.f);
            const char *flag = o0 ? "origin0" : (oob ? "OOB" : (near0 ? "near0" : "ok"));
            if (o0 || oob || near0) ++off;
            spdlog::info("[VMEXTRACT] {},{},{},{},{},{:.1f},{:.1f},{}",
                         flag, m.group, m.category, m.srcArea, m.raw_area, m.worldX, m.worldZ, m.name_id);
            ++n;
        }
    }
    spdlog::info("[VMEXTRACT] done: {} markers in region ({} flagged off-map)", n, off);
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

// Pin the NEXT tooltip to the canvas pointer instead of letting ImGui place it.
//
// ImGui positions a tooltip via NavCalcPreferredRefPos(): while gamepad nav is active
// (`!NavDisableHighlight && NavDisableMouseHover`) that returns the NAV-FOCUSED WIDGET's rect and
// ignores io.MousePos entirely — so a canvas tooltip rendered in pad-mode appeared next to whatever
// sidebar row happened to hold nav focus, not next to the reticle the player is aiming with. (We do
// override io.MousePos to the reticle; that fixes hit-testing, not placement, which is why the earlier
// ImGuiHoveredFlags_NoNavOverride sweep didn't cover this one.) SetNextWindowPos sets
// window_pos_set_by_api, which bypasses that auto-placement — the same escape hatch ImGui's own
// drag-and-drop tooltips use.
//
// Mouse mode is left alone: ImGui's own placement already follows the cursor AND clamps to the
// viewport, which is strictly better than anything we'd hand-roll.
static void pin_tooltip_to_pointer(bool pad_mode, const ImVec2 &ptr)
{
    if (!pad_mode) return;
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const float off = 16.0f * ImGui::GetIO().FontGlobalScale;
    ImVec2 pos(ptr.x + off, ptr.y + off), pivot(0.0f, 0.0f);
    // No auto-clamp on this path (that's the trade for bypassing the placement), so flip the anchor
    // when the pointer nears an edge instead of letting the tooltip run off-screen.
    if (pos.x > vp->Pos.x + vp->Size.x * 0.72f) { pos.x = ptr.x - off; pivot.x = 1.0f; }
    if (pos.y > vp->Pos.y + vp->Size.y * 0.72f) { pos.y = ptr.y - off; pivot.y = 1.0f; }
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
}

void draw_virtual_map(const OverlayFrameCtx &ctx)
{
    // Slice D: open the virtual map with the game MAP KEY when a CUSTOM world is active (the production
    // "M, not F1" UX). On map-open edge with active world ≠ Base ER → open; on map-close, close it if WE
    // opened it (so a Dev-toggle-opened vmap isn't closed by the game map). Runs every frame (this entry is
    // called unconditionally, independent of the F1 panel).
    {
        static bool s_prev_map = false;  // s_from_map is file-scope (read at the fullscreen Begin below)
        const bool map_now = goblin::overlay_api::world_map_open();
        if (*goblin::overlay_api::cfg_vmapOnMapKey_ptr())
        {
            // REDIRECT: the WorldMapDialog create-callback is HOOKED (goblin_native_map_redirect) — pressing
            // the map key (kb OR gamepad) is intercepted BEFORE the native constructs, so the native never
            // opens; the hook TOGGLES the redirect flag instead. Mirror the vmap open state to that flag.
            const bool want = goblin::overlay_api::vmap_redirect();
            s_open = want;
            s_from_map = want;  // fullscreen stand-in
        }
        else
        {
            // Custom virtual world (legacy "M, not F1"): open on the native map-open edge, COVER the native.
            if (map_now && !s_prev_map && goblin::vworld::active() != 0) { s_open = true; s_from_map = true; }
            else if (!map_now && s_prev_map && s_from_map) { s_open = false; s_from_map = false; }
        }
        s_prev_map = map_now;
    }
    // Close the map from INSIDE the panel (pad B, a queued warp). With vmap_on_map_key the open state is
    // SLAVED to the redirect flag by the block above — it re-reads it every frame, so clearing s_open
    // alone would be undone on the next one. The flag is the real switch, the same one the map key and
    // Escape flip. The custom-world path has no flag, so there the local state IS the state.
    auto close_vmap = [&]() {
        if (*goblin::overlay_api::cfg_vmapOnMapKey_ptr() || goblin::overlay_api::vmap_redirect())
            goblin::overlay_api::set_vmap_redirect(false);
        s_open = false;
        s_from_map = false;
    };

    // Focus the player on OPEN (rising edge of s_open, ANY open path). Tracked BEFORE the early return
    // so a close→reopen RE-fires — else s_was_open never resets while closed (the fn returns first) and
    // focus fired only on the very first open.
    {
        static bool s_was_open = false;
        if (s_open && !s_was_open) s_focus_player = true;
        s_was_open = s_open;
    }
    if (!s_open) return;

    // Hoisted above the sidebar (its value is set from stick/button input further down; here it holds
    // last frame's) so the sidebar can gate its hover-tooltips on it: in gamepad pad-mode the reticle
    // is the single pointer and it's clamped to the CANVAS, so a sidebar tooltip driven by the (frozen)
    // real mouse would render at the stale cursor spot, not where the pad is looking — bug "tooltip
    // shows at the old gamepad selection, not the reticle". Pad tooltips come only from the canvas.
    static bool s_pad_mode = false;
    static ImVec2 s_pad_cursor(0, 0);       // right-stick reticle; init'd + clamped to the canvas below
    static bool s_pad_cursor_init = false;
    // SINGLE-POINTER override: in pad-mode point ImGui's mouse at the reticle for the ENTIRE frame,
    // BEFORE the sidebar/category panel draw. The reticle is clamped to the canvas, so no sidebar widget
    // (grace list, category checkboxes, search) is "hovered" off the stale/frozen real mouse → no tooltip
    // teleports to a nav-selected widget. Uses last frame's reticle (static; recomputed + re-applied for
    // the canvas below). Restored before End() so ImGui's next-frame MouseDelta isn't corrupted.
    const ImVec2 s_pad_saved_mouse = ImGui::GetIO().MousePos;
    if (s_pad_mode) ImGui::GetIO().MousePos = s_pad_cursor;

    // ── Y-warp latch, shared by the grace SIDEBAR and the canvas reticle ──────────────────────────
    // Hoisted here (it only needs key state) because the sidebar draws before the canvas and both fire
    // on it. Press-order-independent: armed on Y-down, CLEARED the moment the toggle combo's other
    // button (R3) joins the hold, consumed on Y-up. So a Y tap = warp, and Y+R3 in ANY order = close
    // the overlay, never a warp. That collision is why the sidebar's Y-warp was pulled in the input
    // pass — it fired off mere nav focus, so closing with the combo teleported you. With the latch the
    // binding is safe again, and the row tooltip that still promised "Y: warp" stops lying (user
    // 2026-07-28). pad_warp_taken keeps ONE press from warping twice when a grace row holds nav focus
    // AND the reticle sits on a grace: the focused row wins (it is the more explicit aim).
    static bool s_y_warp_armed = false;
    const bool pad_combo = goblin::overlay_api::gamepad_combo_held();
    if (s_pad_mode && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)) s_y_warp_armed = true;
    if (pad_combo) s_y_warp_armed = false;
    const bool pad_warp_fire = s_y_warp_armed && ImGui::IsKeyReleased(ImGuiKey_GamepadFaceUp);
    if (ImGui::IsKeyReleased(ImGuiKey_GamepadFaceUp)) s_y_warp_armed = false;
    bool pad_warp_taken = false;

    // Opened by the game MAP KEY (s_from_map) → draw FULLSCREEN + opaque so it stands in for the
    // native map (which still renders underneath; we cover it rather than suppress it — the native
    // render flag "does not hide the map", proven live). Otherwise a floating dev window.
    const bool fullscreen = s_from_map;
    ImGuiWindowFlags win_flags = 0;
    if (fullscreen)
    {
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.05f, 0.06f, 1.0f));  // opaque cover
        win_flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus;
    }
    else
    {
        ImGui::SetNextWindowSize(ImVec2(720.0f, 560.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(470.0f, 40.0f), ImGuiCond_FirstUseEver);
    }
    const bool vmap_visible =
        ImGui::Begin(tr("MapForGoblins \xE2\x80\x94 Virtual World Map (WIP)"), &s_open, win_flags);
    if (fullscreen) ImGui::PopStyleColor();
    if (!vmap_visible)
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
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_NoNavOverride)) ImGui::SetTooltip("%s", tr("auto-switch the map page to the player's dimension (OW/underground/DLC) when they cross"));
    ImGui::SameLine();
    ImGui::Checkbox(tr("Icons"), &s_show_icons);   // real category icons vs plain dots
    ImGui::SameLine();
    ImGui::Checkbox(tr("Labels"), &s_show_labels); // coarse region name labels (A7)
    ImGui::SameLine();
    ImGui::Checkbox(tr("Relief"), &s_show_relief); // heightfield hillshade backdrop (D2.3)
    ImGui::SameLine();
    if (ImGui::Checkbox(tr("Graces"), &s_show_graces) && s_show_graces) s_graces_built = false; // Track B sidebar
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_NoNavOverride))
        ImGui::SetTooltip("%s", tr("Grace list + warp. Gamepad: LB opens/closes it from anywhere on the map."));
    ImGui::SameLine();
    ImGui::Checkbox(tr("Markers"), &s_show_markers_panel);  // F1→vmap: category visibility controls
    ImGui::SameLine();
    ImGui::Checkbox(tr("Custom"), &s_show_custom);  // our own player-placed markers (right-click to drop)
    ImGui::SameLine();
    ImGui::Checkbox(tr("Items"), &s_show_item_search);  // A9: search + locate items on the vmap itself
    ImGui::SameLine();
    // Dev/calibration strip — hidden from normal users, shown only with Verbose logging on (same gate as
    // "Baked-only"): terrain sampling, ER-tile loaders, orientation flips + the marker extractor. These
    // are RE/debug controls, not map features (user 2026-07-23). Extract state persists across frames.
    static bool s_extract_mode = false, s_extract_dragging = false;
    static ImVec2 s_extract_a{}, s_extract_b{};
    if (*goblin::overlay_api::cfg_debugLogging_ptr())
    {
        // 1024u extent = the ACCURATE zone: the world→cast-local transform is a translation captured at the
        // player, valid only near the player's physics chunk. res 48 → ~21u cells, dense.
        if (ImGui::SmallButton(tr("Sample terrain")))  // cast a grid around the player (map must be CLOSED)
        {
            goblin::overlay_api::heightfield_request_sample(1024.f, 48);
            s_show_relief = true;   // so the result is visible even if Relief was toggled off
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);                                // dev: tune the warp id offset live
        ImGui::DragInt(tr("warp off"), &s_warp_offset, 10.0f, -100000, 100000);  // double-click a grace to test
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
        // Marker extractor (user 2026-07-23): toggle, then left-drag a box on the canvas → every marker
        // inside is logged ([VMEXTRACT]) with an off-map flag, to debug markers landing off the map.
        ImGui::SameLine();
        if (ImGui::SmallButton(s_extract_mode ? tr("Extract: drag a box") : tr("Extract region")))
        { s_extract_mode = !s_extract_mode; s_extract_dragging = false; }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_NoNavOverride))  // mouse-only (bug-1: no nav-focus teleport)
            ImGui::SetTooltip("%s", tr("Toggle, then left-drag a rectangle on the map. Every marker inside is\n"
                                       "logged ([VMEXTRACT] in logs/MapForGoblins.log) with an off-map flag\n"
                                       "(origin0 / OOB / ok) — a debug aid for markers that land off the map."));
    }
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
                if (ImGui::Selectable(tr(kGroupNames[i]), s_group == i) && i != s_group)
                {
                    // Switching page used to keep the old camera + zoom, so you landed on empty canvas
                    // somewhere off the new page and had to hunt for it (user 2026-07-28). Frame the new
                    // page: on the PLAYER when they are standing on it (the useful answer to "what's
                    // around me down here"), else fit that page's marker bbox. Both mechanisms already
                    // exist and are consumed further down, AFTER the quadtree rebuilds for the new group.
                    s_group = i;
                    int pa = 0, pg = 0; float px = 0.f, pz = 0.f;
                    if (goblin::overlay_api::get_player_map_pos(pa, px, pz, nullptr, nullptr, &pg) && pg == i)
                        s_focus_player = true;
                    else
                        s_fit_requested = true;
                }
            ImGui::EndCombo();
        }
    }
    ImGui::TextDisabled(tr("drag = pan   wheel = zoom   |   centre (%.0f, %.0f)  zoom %.3f px/u   markers %d   relief %d/%d"),
                        s_cam_x, s_cam_z, s_zoom, s_drawn, s_relief_drawn, s_relief_hits);
    // This readout sits ABOVE the canvas, and the canvas takes whatever GetContentRegionAvail() is left
    // — so letting the row APPEAR or vanish resizes the map by one text line. That was the "the map
    // flickers / changes aspect the FIRST time I warp" report: with no tiles loaded the row didn't exist
    // yet, the warp set s_tile_status, the row appeared, and the canvas lost a line's height in one
    // frame. Only the first time, because the status string never becomes empty again.
    // So the row is ALWAYS laid out; only its text is conditional.
    if (!s_tiles.empty() || !s_tile_status.empty())
        ImGui::TextDisabled(tr("map tiles: %d loaded   |   last: %s"), (int)s_tiles.size(),
                            s_tile_status.c_str());
    else
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));   // reserve the same height, no text

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

    // ── Sidebars OVERLAY the map, they no longer eat its width ────────────────────────────────────
    // They used to be laid out in the flow, so opening one (e.g. X drops a pin AND opens the marker
    // list) shrank the canvas — and since the canvas centre moved with it, the whole map JUMPED
    // sideways under the cursor mid-action (user 2026-07-28). Now each sidebar is placed absolutely
    // over the canvas and the cursor is restored afterwards, so the canvas keeps the FULL region and
    // its geometry never changes when a panel opens. Child windows render after their parent's draw
    // list, so they sit on top of the map for free; ImGui also hit-tests children in front of the
    // parent, so the canvas InvisibleButton underneath can't steal their clicks, drags or wheel.
    // The one thing they need is an opaque background — ImGuiCol_ChildBg is transparent by default,
    // which was fine over a solid panel and is not over map art.
    const ImVec2 sb_origin = ImGui::GetCursorScreenPos();
    float sb_x = 0.0f;   // width already taken by the sidebars placed so far (they sit side by side)
    auto sidebar_begin = [&](const char *id, float w) {
        ImGui::SetCursorScreenPos(ImVec2(sb_origin.x + sb_x, sb_origin.y));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.08f, 0.10f, 0.97f));
        ImGui::BeginChild(id, ImVec2(w, 0.0f), true);
    };
    auto sidebar_end = [&](const char *splitId, float &w, float wmin, float wmax) {
        ImGui::EndChild();
        ImGui::PopStyleColor();
        vsplitter(splitId, w, wmin, wmax);   // still a real drag-to-resize, drawn at the sidebar's edge
        sb_x += w + 6.0f;                    // 6 = the splitter's own width
        ImGui::SetCursorScreenPos(sb_origin);
    };

    // F1→vmap port (first slice): host the F1 "Sections & categories" controls in a vmap sidebar. It's a
    // standalone draw_X(ctx, Filter&) with zero coupling to the F1 window, so it drops straight in — the
    // toggles it drives are shared config, so hiding a category here hides it on the vmap markers too. The
    // eventual home is a unified tabbed sidebar (Markers|Search|Quests|…); this proves the reuse path.
    if (s_show_markers_panel)
    {
        sidebar_begin("##markers_panel", s_markers_w);
        // Master "show all markers" toggle (the F1 icon master) — ported here so the categories list has
        // its on/off head; drives overlay_api::icons_enabled (gates every vmap marker + the native map).
        bool master_on = goblin::overlay_api::icons_enabled();
        if (ImGui::Checkbox(tr("Show all markers"), &master_on))
            goblin::overlay_api::set_icons_enabled(master_on);
        ImGui::Separator();
        draw_sections_categories(ctx, s_markers_filter, /*with_err_integration=*/false);   // ERR moot on ImGui map
        sidebar_end("##markers_split", s_markers_w, 200.0f, 640.0f);
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
                        // The Roundtable hall (m11_10) is drawn as a small INSET on the overworld page,
                        // so its graces project to a spot in the middle of Liurnia-ish nowhere and the
                        // region Voronoi files them under whatever anchor happens to be nearest —
                        // meaningless for a warp list (user 2026-07-28). Give the tile its own group.
                        // Tile-based, so it also catches ERR's "Gilded Court", the endgame hall in the
                        // SAME m11_10 (measured 2026-07-23, see offmap-area45-missing-anchor.md), and any
                        // other mod's hall grace there — no name matching, nothing ERR-specific.
                        g.hub = (m.raw_area == 11 && m.raw_gx == 10);
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
                if (g.hub) { g.region = tr("Roundtable Hold"); continue; }   // own group, skip the Voronoi
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
            // Hub graces FIRST (the hall is the single most warped-to destination in a run), then the
            // regions alphabetically as before.
            std::sort(s_graces.begin(), s_graces.end(), [](const GraceRow &a, const GraceRow &b) {
                if (a.hub != b.hub) return a.hub;
                if (a.region != b.region) return a.region < b.region;
                return a.name < b.name;
            });
            s_graces_built = true;
        }
        sidebar_begin("##grace_sidebar", s_graces_w);
        ImGui::Text(tr("Graces (%d)"), (int)s_graces.size());
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Refresh"))) s_graces_built = false;
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##gfilter", tr("search"), s_grace_filter, sizeof(s_grace_filter));
        draw_gamepad_keyboard_button("##vmap_gracefilter_kbd", s_grace_filter, sizeof(s_grace_filter));
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
                else
                {
                    // Locate: pan the canvas to it. The baked g.wx/g.wz is only a FALLBACK — the canvas
                    // draws graces from s_vmarkers, whose positions are re-projected through the live
                    // engine converter, so centring on the baked pair lands next to the icon instead of
                    // on it (user 2026-07-28). Hand the row id to the resolver below, which runs after
                    // the quadtree rebuild and reads the exact position the icon is drawn at.
                    s_cam_x = g.wx; s_cam_z = g.wz; s_group = g.group;
                    s_locate_grace_row = g.rowId;
                }
            }
            // Gamepad: A (nav-activate) LOCATES the grace (pans the canvas to it), like a single mouse
            // click; Y on the FOCUSED row warps it, the list equivalent of aiming the reticle at it on
            // the canvas. Gated on IsItemFocused (this exact row, not "some row exists") and on the
            // shared Y latch, so the Y+R3 close-combo can never teleport you — the reason this binding
            // was pulled. Discovered only: warping to an undiscovered grace hangs on a load screen.
            if (disc && pad_warp_fire && !pad_warp_taken && ImGui::IsItemFocused())
            {
                s_warp_pending = g.rowId;
                pad_warp_taken = true;   // this press is spent; the canvas reticle must not warp too
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_NoNavOverride))   // mouse-only (bug-1: no nav-focus teleport)
                ImGui::SetTooltip("%s", disc ? tr("double-click / Y: warp · click: locate")
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
        sidebar_end("##graces_split", s_graces_w, 180.0f, 520.0f);
    }

    // Item-search sidebar (A9): search PLACED item markers by name and click to locate on THIS canvas —
    // the vmap-native equivalent of the F1 item search, so search works with the vmap as the sole surface
    // (no native ER map needed). Rebuilds the hit list only when the query changes (name lookups cached).
    if (s_show_item_search && active_world == 0)
    {
        // state_id splits a same-name/same-page item into ROYAL vs ASHEN Capital (and other pre/post
        // story-state regions), so a player doesn't hunt an item in the wrong game state — `state` is the
        // human tag, `reachable` = is that state the CURRENT game state (via the live story flag).
        struct IHit { std::string label; int32_t name_id; int group; int count; int state_id; std::string state; bool reachable; };
        static std::vector<IHit> s_ihits;
        static std::string s_ihits_q = "\x01";   // sentinel ≠ "" so an empty box builds once (clears list)

        sidebar_begin("##item_sidebar", s_items_w);
        ImGui::Text(tr("Item search"));
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##ifilter", tr("item name (2+ chars)"), s_item_filter, sizeof(s_item_filter));
        draw_gamepad_keyboard_button("##vmap_itemfilter_kbd", s_item_filter, sizeof(s_item_filter));

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
                std::unordered_map<int, bool> flag_cache;    // story flag -> live state (read once per flag)
                std::map<int64_t, int> hitc;                 // (name_id, state, group) -> instance count
                // A marker's pre/post story state: secondary_flag = appears-post-event (Ashen);
                // hide_when_flag = pre-event / consumed-post-event (Royal). state_id splits the rows;
                // reachable = does the current game state match (live flag). Never both flags on a tile.
                auto story = [&](const goblin::worldmap::Marker &m, int &sid, bool &reach) -> std::string {
                    int f = m.secondary_flag; bool post = true;
                    if (!f) { f = m.hide_when_flag; post = false; }
                    if (!f) { sid = 0; reach = true; return std::string(); }
                    auto fc = flag_cache.find(f);
                    if (fc == flag_cache.end())
                        fc = flag_cache.emplace(f, goblin::overlay_api::read_event_flag((uint32_t)f)).first;
                    reach = post ? fc->second : !fc->second;   // post: reachable when flag SET; pre: when UNSET
                    sid = f * 2 + (post ? 1 : 0);
                    const char *t;
                    if (f == goblin::flag::StoryErdtreeOnFire)        t = post ? "Ashen Capital"      : "Royal Capital";
                    else if (f == goblin::flag::StoryCharmBroken)     t = post ? "post charm-break"   : "pre charm-break";
                    else if (f == goblin::flag::StorySealingTreeBurnt)t = post ? "post seal-tree-burn": "pre seal-tree-burn";
                    else                                             t = post ? "post-event"        : "pre-event";
                    return std::string(t);
                };
                auto keyof = [](int32_t nid, int sid, int g) {
                    return ((int64_t)nid << 40) | ((int64_t)(sid & 0x3FFFFFFF) << 2) | (int64_t)g;
                };
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
                        int sid; bool reach; std::string st = story(m, sid, reach);
                        // Mark EVERY matching instance (not deduped like the list rows), capped.
                        if (s_search_mark_on && (int)marks.size() < s_search_mark_max)
                            marks.push_back({m.worldX, m.worldZ, g});
                        const int64_t k = keyof(m.name_id, sid, g);
                        if (hitc[k]++ == 0)
                        {
                            std::string label = loc.empty() ? en : loc;
                            if (!en.empty() && en != label) label += " (" + en + ")";
                            s_ihits.push_back({label, m.name_id, g, 0, sid, std::move(st), reach});
                        }
                    }
                }
                for (auto &h : s_ihits) h.count = hitc[keyof(h.name_id, h.state_id, h.group)];
                std::sort(s_ihits.begin(), s_ihits.end(), [](const IHit &a, const IHit &b) {
                    int c = a.label.compare(b.label);
                    if (c != 0) return c < 0;
                    if (a.group != b.group) return a.group < b.group;
                    return a.state_id < b.state_id;   // Royal/Ashen rows of one item sort adjacent + stable
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
            const char *gname = (h.group >= 0 && h.group < 4) ? kGroupNames[h.group] : "?";
            char row[288];
            // State-gated items get a [Royal Capital] / [Ashen Capital] (…) suffix so the player knows
            // WHICH game state holds the item + whether it is reachable RIGHT NOW — "here now" when the
            // current state matches, "GATED" when it needs the other state (pre/post the story event).
            if (h.state.empty())
                std::snprintf(row, sizeof(row), "%s  (x%d) - %s##ih%d_%d", h.label.c_str(), h.count,
                              gname, h.name_id, h.state_id);
            else
                // Gated item: the state (Royal/Ashen Capital) is more useful than the redundant OW group,
                // so it REPLACES it — and leads with a reachability badge so a wrong-state item is obvious
                // at a glance even if the row is clipped.  [+] = here in the current game state, [x] = gated.
                std::snprintf(row, sizeof(row), "%s %s  (x%d) - %s##ih%d_%d",
                              h.reachable ? "[+]" : "[x]", h.label.c_str(), h.count, h.state.c_str(),
                              h.name_id, h.state_id);
            if (ImGui::Selectable(row))
                virtual_map_locate(h.name_id, h.group);   // centre the canvas on the hit + switch page
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_NoNavOverride))   // mouse-only (bug-1: no nav-focus teleport)
            {
                if (h.state.empty())
                    ImGui::SetTooltip(tr("click: centre the map on it (%s)"), gname);
                else
                    ImGui::SetTooltip(h.reachable
                                          ? tr("%s — in the CURRENT game state; click to centre")
                                          : tr("%s — needs a different game state (the item is not here "
                                               "in this playthrough state yet); click to centre"),
                                      h.state.c_str());
            }
        }
        ImGui::EndChild();
        sidebar_end("##items_split", s_items_w, 180.0f, 520.0f);
    }

    // Custom-marker LIST sidebar (#2) — the DX answer to "where's my custom marker?": each shows which map
    // (group) + coords, with Go (pan the canvas there), TP (teleport in-game, intra-region), and delete.
    if (s_show_custom && active_world == 0)
    {
        sidebar_begin("##custom_panel", s_custom_w);
        auto cm = goblin::custom_markers::snapshot();
        ImGui::Text(tr("Custom markers (%d/%d this map)"),
                    (int)goblin::custom_markers::count_in_group(s_group), goblin::custom_markers::kMaxPerGroup);
        ImGui::TextDisabled("%s", tr("right-click the map (pad: X) to add / delete"));
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
            // Rename with a gamepad: the InputText alone is unreachable without a keyboard (user
            // 2026-07-28) — same on-screen keyboard every filter field already uses. It writes into
            // nbuf, which is re-seeded from the store each frame, so commit whatever it produced.
            draw_gamepad_keyboard_button("##nm_kbd", nbuf, sizeof(nbuf));
            if (c.name != nbuf) goblin::custom_markers::set_name((size_t)i, nbuf);
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
        sidebar_end("##custom_split", s_custom_w, 180.0f, 480.0f);
    }

    // Gamepad focus arbitration (user 2026-07-28: pressing X while browsing the custom-marker sidebar
    // dropped a pin on the canvas behind it). The canvas X-place is only legal when nav focus is NOT
    // parked on a sidebar widget — every sidebar is a CHILD window, so "a child is focused, not this
    // window itself" is exactly the sidebar case — nor inside a popup (the on-screen keyboard, a combo).
    // Evaluated after all sidebars are drawn and before the canvas, so it reflects THIS frame's focus.
    const bool nav_in_sidebar =
        (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::IsWindowFocused()) ||
        ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);

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

    // PIN LOCK (the custom-marker twin of the grace lock, user 2026-07-28): catch radius in screen px,
    // resolution-scaled — the old delete test was a bare 14 px, so it shrank to nothing at 4K. Nearest
    // same-group pin to a screen point, or -1. ONE place, because the lock ring, the right-click delete
    // and the pad-X delete must never disagree about which pin you are on: the ring IS the promise that
    // this is the one that goes. (This folded the two pre-existing copies of the loop into the helper.)
    const float kPinCatch = 16.0f * uiScale;
    auto pin_at = [&](ImVec2 p, const std::vector<goblin::custom_markers::Marker> &cm) -> int {
        int hit = -1;
        float bestd = kPinCatch * kPinCatch;
        for (int i = 0; i < (int)cm.size(); ++i)
        {
            if (cm[i].group != s_group) continue;
            const ImVec2 ps = w2s(cm[i].wx, cm[i].wz);
            const float dx = ps.x - p.x, dy = ps.y - p.y, d = dx * dx + dy * dy;
            if (d < bestd) { bestd = d; hit = i; }
        }
        return hit;
    };

    // Extract mode (dev): left-drag draws a selection box instead of panning; on release, dump every marker
    // inside it to the log. Takes over the left-drag while active, so panning is suspended.
    if (hovered && s_extract_mode)
    {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { s_extract_a = io.MousePos; s_extract_b = io.MousePos; s_extract_dragging = true; }
        if (s_extract_dragging && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) s_extract_b = io.MousePos;
        if (s_extract_dragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            s_extract_dragging = false;
            float ax, az, bx, bz;
            s2w(s_extract_a, ax, az); s2w(s_extract_b, bx, bz);
            const int got = extract_region_log((std::min)(ax, bx), (std::max)(ax, bx),
                                               (std::min)(az, bz), (std::max)(az, bz));
            s_tile_status = "extracted " + std::to_string(got) + " markers to log ([VMEXTRACT])";
        }
    }
    // Pan: dragging moves the camera opposite the mouse delta (in world units). Suspended in extract mode.
    else if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
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
        const int hit = pin_at(io.MousePos, cm);
        if (hit >= 0)
            goblin::custom_markers::remove_at((size_t)hit);
        else if (!goblin::custom_markers::add(mwx, mwz, s_group,
                     next_marker_name(), IM_COL32(90, 200, 255, 255)))
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

    // ── Gamepad canvas control (M4 nav-parity). LEFT stick pans, TRIGGERS (L2/R2) zoom, RIGHT stick drives
    // a visible virtual cursor (reticle) so the player sees what they aim at with the pad. Reads ImGui's
    // gamepad analog keys (fed by ImGui_ImplWin32_UpdateGamepads). RIGHT stick + triggers deliberately DON'T
    // collide with ImGui nav, which uses the LEFT stick / dpad for sidebar widget focus. A latch keeps the
    // reticle shown only while the pad is actually driving (a real mouse move exits pad-mode).
    // s_pad_mode / s_pad_cursor / s_pad_cursor_init hoisted to the top of draw_virtual_map (single-pointer)
    {
        auto an = [&io](ImGuiKey k) { return io.KeysData[k - ImGuiKey_KeysData_OFFSET].AnalogValue; };
        auto ax = [&](ImGuiKey neg, ImGuiKey pos) {
            float v = an(pos) - an(neg);
            return (v > 0.18f || v < -0.18f) ? v : 0.0f;   // deadzone
        };
        const float lsx = ax(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight);
        const float lsy = ax(ImGuiKey_GamepadLStickUp,   ImGuiKey_GamepadLStickDown);
        const float rsx = ax(ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight);
        const float rsy = ax(ImGuiKey_GamepadRStickUp,   ImGuiKey_GamepadRStickDown);
        float zoomAx = an(ImGuiKey_GamepadR2) - an(ImGuiKey_GamepadL2);
        if (zoomAx < 0.12f && zoomAx > -0.12f) zoomAx = 0.0f;

        // Enter pad-mode on ANY stick/trigger OR a face-button press — a pad that feeds buttons but little
        // analog (or a user who presses X to place a marker before nudging a stick) still activates it.
        const bool pad_btn = ImGui::IsKeyDown(ImGuiKey_GamepadFaceUp) || ImGui::IsKeyDown(ImGuiKey_GamepadFaceLeft) ||
                             ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown) || ImGui::IsKeyDown(ImGuiKey_GamepadFaceRight);
        if (lsx || lsy || rsx || rsy || zoomAx || pad_btn) s_pad_mode = true;
        // Exit only on a REAL mouse move — Wine/Proton can micro-jitter the cursor every frame, which used to
        // kill pad-mode instantly (suspected cause of "pad doesn't work in vmap", user 2026-07-23).
        if (io.MouseDelta.x > 2.0f || io.MouseDelta.x < -2.0f || io.MouseDelta.y > 2.0f || io.MouseDelta.y < -2.0f)
            s_pad_mode = false;
        goblin::overlay_api::set_vmap_pad_mode(s_pad_mode);  // host reads this to hide/show the mouse cursor
        if (!s_pad_cursor_init) { s_pad_cursor = center; s_pad_cursor_init = true; }

        if (s_pad_mode)
        {
            const float dt = io.DeltaTime > 0.0f ? io.DeltaTime : (1.0f / 60.0f);
            const float span = canvas_end.x - origin.x;
            const float sens = *goblin::overlay_api::cfg_gamepadSensitivity_ptr();  // user speed multiplier
            // RIGHT stick → move the virtual cursor (≈ full-canvas sweep in ~1.1s), clamp on-canvas.
            s_pad_cursor.x += rsx * (0.9f * span * sens) * dt;
            s_pad_cursor.y += rsy * (0.9f * span * sens) * dt;
            s_pad_cursor.x = s_pad_cursor.x < origin.x ? origin.x : (s_pad_cursor.x > canvas_end.x ? canvas_end.x : s_pad_cursor.x);
            s_pad_cursor.y = s_pad_cursor.y < origin.y ? origin.y : (s_pad_cursor.y > canvas_end.y ? canvas_end.y : s_pad_cursor.y);
            // LEFT stick → pan (push-to-pan; world units, zoom-aware; axis signs match the mouse-drag path).
            s_cam_x += lsx * (0.6f * span * sens) * dt / (s_sx * s_zoom);
            s_cam_z += lsy * (0.6f * span * sens) * dt / (s_sz * s_zoom);
            // TRIGGERS → zoom about the reticle (keep the world point under it fixed). R2 in, L2 out.
            if (zoomAx != 0.0f)
            {
                float wxb, wzb; s2w(s_pad_cursor, wxb, wzb);
                const float zoomSens = *goblin::overlay_api::cfg_gamepadZoomSensitivity_ptr();
                s_zoom *= std::pow(1.9f, zoomAx * dt * zoomSens);
                if (s_zoom < kZoomMin) s_zoom = kZoomMin;
                if (s_zoom > kZoomMax) s_zoom = kZoomMax;
                float wxa, wza; s2w(s_pad_cursor, wxa, wza);
                s_cam_x += wxb - wxa;
                s_cam_z += wzb - wza;
            }
        }
    }

    // ── Gamepad reticle → hover / activate / place (M4). The RIGHT-stick reticle (s_pad_cursor) becomes
    // the effective canvas pointer in pad-mode, so the pad hovers marker/grace tooltips and targets
    // actions exactly like the mouse. Buttons chosen to NOT collide with ImGui nav (FaceDown=Activate,
    // FaceRight=Cancel, dpad/LStick=widget nav): FaceUp (Y/△) = activate (warp a hovered discovered grace),
    // FaceLeft (X/□) = place / delete a custom marker (the right-click equivalent). Mouse path unchanged.
    // Re-apply the single-pointer override with THIS frame's freshly-computed reticle (the top-of-function
    // override used last frame's static, for the sidebar). Restored to the real mouse before End() (see
    // s_pad_saved_mouse) so ImGui's next-frame MouseDelta isn't corrupted. vptr == reticle in pad-mode.
    if (s_pad_mode) io.MousePos = s_pad_cursor;
    const ImVec2 vptr = io.MousePos;
    const bool pad_over_canvas = s_pad_mode && s_pad_cursor.x >= origin.x && s_pad_cursor.x <= canvas_end.x &&
                                 s_pad_cursor.y >= origin.y && s_pad_cursor.y <= canvas_end.y;
    const bool hovered_eff = hovered || pad_over_canvas;
    // WARP TRIGGER: the shared Y latch computed at the top of this function (see its comment for the
    // press-order-independent Y-vs-Y+R3 rule). `pad_warp_taken` means a focused grace ROW in the sidebar
    // already spent this press, so the reticle must not warp on the same tap.
    const bool pad_activate = pad_warp_fire && !pad_warp_taken;
    // B (Cancel) CLOSES the map — the button every ER menu is closed with, so reaching for it was the
    // reflex (user 2026-07-28). Layered under ImGui's own Cancel rather than fighting it: while nav
    // focus sits in a sidebar or a popup is open, B belongs to ImGui (back out of the child, close the
    // on-screen keyboard) and only a second press — once there is nothing left to back out of — closes
    // the map. Not part of the toggle combo, so no latch needed.
    if (s_pad_mode && !pad_combo && !nav_in_sidebar &&
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
        close_vmap();
    // Place/delete a custom marker: press-edge is fine (X is not part of the toggle combo). Suppressed
    // while nav focus sits on a sidebar/popup widget — X there belongs to the widget, not the canvas.
    const bool pad_place = s_pad_mode && !pad_combo && !nav_in_sidebar &&
                           ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false);
    // Y warps EITHER the sidebar's focused grace row (handled up there) or the reticle's hovered grace
    // (below) — never both, and never on the close combo.
    // X is CONTEXT-SENSITIVE, mirroring the mouse split (left-click a region chip toggles it, right-click
    // the canvas drops a pin): the region-label loop further down consumes this latch when the reticle is
    // over a chip, otherwise the place/delete runs right after that loop. Deferred that far because the
    // chip rects are only known there — and it also stops X from burying a pin under a region name.
    bool pad_place_pending = pad_place && pad_over_canvas && active_world == 0;
    // QOL (user 2026-07-28): LB opens/closes the grace sidebar from anywhere on the map, so reaching the
    // warp list is one button instead of navigating up to its checkbox. L1/R1 are free for us — ImGui uses
    // them ONLY inside its gamepad window-switcher, which we disarm every frame (goblin_overlay.cpp) — and
    // while the vmap covers the map the game gets a zeroed pad anyway (input_capture_active), so a bare LB
    // cannot leak into gameplay. Suppressed under a popup (the on-screen keyboard owns the pad there).
    if (s_pad_mode && !pad_combo && ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
    {
        s_show_graces = !s_show_graces;
        if (s_show_graces) s_graces_built = false;   // same rebuild the checkbox triggers
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
            if (goblin::worldmap::far_relief_built_group() != s_group)
                goblin::overlay_api::far_relief_build(s_group, 128);
            // D-far -1 v0: the MSB Y-cloud relief. Drawn UNDER the near raycast (below) so the exact near
            // samples overdraw the cloud estimate where both exist. Cooler tint so cloud vs raycast read apart.
            {
                static std::vector<goblin::heightfield::Cell> s_far;
                goblin::worldmap::far_relief_snapshot(s_far);
                const float fcell = goblin::worldmap::far_relief_step();
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
                    // Sea-tagged cells (underwater seabed) render water-blue; land uses the terrain tone.
                    if (c.sea)
                        dl->AddRectFilled(p0, p1, IM_COL32(ch(40), ch(96), ch(150), 220));
                    else
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
    // Constant-size (no zoom growth) draw half for NPC/Merchant/boss pins, to match the NATIVE map
    // (its icons are fixed-px, zoom-independent) — the vmap's `icoHalf` grows to ~2.2× at high zoom,
    // which made these read too big when zoomed in (user 2026-07-23). Only the DRAW size; the spiderfy
    // fan geometry keeps `icoHalf`.
    const float icoHalfFixed = 8.0f * uiScale;
    const bool nativeIcons = *goblin::overlay_api::cfg_nativeItemIcons_ptr();
    constexpr int kGraceCat = static_cast<int>(goblin::generated::Category::WorldGraces);
    constexpr int kBossCat = static_cast<int>(goblin::generated::Category::WorldBosses);
    constexpr int kNpcCat = static_cast<int>(goblin::generated::Category::WorldQuestNPC);
    constexpr int kMerchCat = static_cast<int>(goblin::generated::Category::WorldMerchant);
    // Grace catch radius, as a multiple of (icon half-size)² — see the GRACE LOCK note in `plot`.
    constexpr float kGraceSnapSq = 6.0f;   // radius ≈ 2.45 * icoHalf vs 1.41 for everything else
    // Hover tooltip: track the marker nearest the cursor (within a pixel radius) while drawing.
    // Graces draw ON TOP (2nd pass) so they must also WIN the hover — track a priority (grace=1) so a
    // grace within radius beats an underlying non-grace, matching the visible z-order (the tooltip/warp
    // then targets the grace you see on top, not the object beneath it).
    float hoverBestD = 1e18f; int hoverName = -1, hoverCat = -1, hoverDisc = 0, hoverPrio = -1; std::string hoverV; uint64_t hoverRow = 0;
    ImVec2 hoverPs(0, 0);                // SCREEN pos of the hovered marker — drives the grace lock ring
                                         // + the pad reticle's magnetic snap (see kGraceSnapSq below).
    const goblin::worldmap::Marker *hoverMp = nullptr;  // the hovered marker itself — drives the
                                         // provenance block (config::debugMarkerTooltip). Points into
                                         // s_vmarkers, which is static and outlives the frame.
    bool hoverAnon = false;              // spoiler-free hides this marker's name in the tooltip (same predicate as the native map)
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
        // NPC/Merchant/boss draw at a constant (native-like) size; everything else keeps the zoom-scaled icoHalf.
        const float mIco = (cat == kBossCat || cat == kNpcCat || cat == kMerchCat) ? icoHalfFixed : icoHalf;
        if (mp && s_show_icons)
            goblin::worldmap::draw_marker_glyph(dl, *mp, ps.x, ps.y, ctx.atlas_srv, nativeIcons, mIco);
        else
        {
            const IcoRes &r = icon_for(cat);
            if (s_show_icons && r.ok)
                dl->AddImage((ImTextureID)r.tex, ImVec2(ps.x - mIco, ps.y - mIco),
                             ImVec2(ps.x + mIco, ps.y + mIco), r.uv0, r.uv1);
            else
                dl->AddCircleFilled(ps, 3.0f * uiScale, col ? col : IM_COL32(235, 130, 90, 255));
        }
        drawn++;
        if (hovered_eff)
        {
            float dx = ps.x - vptr.x, dy = ps.y - vptr.y, d = dx * dx + dy * dy;
            const int prio = (cat == kGraceCat) ? 1 : 0;   // graces (drawn on top) win the hover
            // GRACE LOCK: graces get a bigger catch radius than any other marker (~1.7x), because they
            // are the one marker you AIM at rather than merely read — warping is the map's main action,
            // and hitting a grace-sized icon with a thumbstick reticle is otherwise fiddly. Combined
            // with prio, passing anywhere near a grace latches onto it; the lock is then made visible
            // (ring below) and the pad reticle snaps to it.
            const float rad2 = mIco * mIco * (cat == kGraceCat ? kGraceSnapSq : 2.0f);
            if (d < rad2 && (prio > hoverPrio || (prio == hoverPrio && d < hoverBestD)))
            { hoverBestD = d; hoverPrio = prio; hoverName = nameId; hoverCat = cat; hoverV = vname ? vname : ""; hoverRow = rowId; hoverDisc = discFlag; hoverWx = wx; hoverWz = wz; hoverArea = mp ? mp->raw_area : -1; hoverAnon = mp && goblin::worldmap::marker_is_anonymized(*mp); hoverPs = ps; hoverMp = mp; }
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
        // Grace "locate" (sidebar click): now that s_vmarkers holds THIS group's live-projected copies,
        // centre on the grace's real drawn position instead of the baked one the row cached. Runs in the
        // same frame as the click (the sidebar draws first, and the rebuild above already ran for the
        // group the click selected). Silently keeps the baked fallback if the row isn't here — hidden by
        // a category toggle, or its projection didn't resolve.
        if (s_locate_grace_row)
        {
            for (const goblin::worldmap::Marker &cm : s_vmarkers)
                if (cm.row_id == s_locate_grace_row && cm.category == kGraceCat)
                { s_cam_x = cm.worldX; s_cam_z = cm.worldZ; break; }
            s_locate_grace_row = 0;
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
        // Grouping obeys the manual "Enable clustering" toggle + its "Cluster size" sub-option, which
        // now OVERRIDE the always-on zoom-LOD (user 2026-07-23): OFF → clusterWorld < 0 disables the
        // collapse test, so every marker draws individually at any zoom. ON → the zoom-scaled pile size,
        // but a node only piles once it holds >= clusterThreshold markers, so small groups stay expanded.
        const bool clusterOn = goblin::config::enableClustering;
        const float clusterWorld = clusterOn ? (kPilePx / (s_zoom > 1e-6f ? s_zoom : 1e-6f)) : -1.0f;
        const int clusterMin = clusterOn ? (std::max)(2, (int)goblin::config::clusterThreshold) : 2;
        static std::vector<MarkerQuadtree::Pile> s_piles;
        static std::vector<const goblin::worldmap::Marker *> s_singles;
        s_piles.clear();
        s_singles.clear();
        s_qt.query(vMinX, vMinZ, vMaxX, vMaxZ, clusterWorld, s_piles, s_singles, clusterMin);

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
            if (hovered_eff)
            {
                float dx = ps.x - vptr.x, dy = ps.y - vptr.y;
                // Suppress the expand hint whenever ANY spiderfy fan is open (not just THIS pile's): a fan
                // is already showing, so "Ctrl+hover to expand" is wrong (with no-steal, hovering another
                // pile won't open it) and it leaks over the open fan's icons.
                if (dx * dx + dy * dy < r * r && !s_fan_open)
                {
                    pin_tooltip_to_pointer(s_pad_mode, vptr);
                    ImGui::SetTooltip("%d %s", pl.count,
                        (goblin::config::clusterSpiderfy && goblin::config::spiderfyHoldCtrl)
                            ? tr("markers — Ctrl+hover to expand, or zoom in")
                            : tr("markers — zoom in to expand"));
                }
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
                plot(m->worldX, m->worldZ, m->color, m->category, m->name_id,
                     m->live_name.empty() ? nullptr : m->live_name.c_str(), m->row_id, m->discover_flag, m);
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
        if (goblin::config::clusterSpiderfy && (hovered_eff || s_force_spiderfy))
        {
            // Spiderfy pile source — DECOUPLED from display clustering (user 2026-07-23). When the display
            // "Enable clustering" toggle is OFF, `s_piles` is empty (clusterWorld<0), so hovering a group of
            // markers would never fan. Build a transient pile list here at the screen-overlap radius (the
            // same kPilePx the display would use) so spiderfy still triggers; when clustering is ON, reuse
            // the already-built display piles. gather_pile() below reads whichever list this points at.
            static std::vector<MarkerQuadtree::Pile> s_spiderPiles;
            const std::vector<MarkerQuadtree::Pile> *spiderPilesPtr = &s_piles;
            if (!clusterOn)
            {
                static std::vector<const goblin::worldmap::Marker *> s_spiderSingles;
                s_spiderPiles.clear();
                s_spiderSingles.clear();
                const float spiderWorld = kPilePx / (s_zoom > 1e-6f ? s_zoom : 1e-6f);
                s_qt.query(vMinX, vMinZ, vMaxX, vMaxZ, spiderWorld, s_spiderPiles, s_spiderSingles, 2);
                spiderPilesPtr = &s_spiderPiles;
            }
            const std::vector<MarkerQuadtree::Pile> &spiderPiles = *spiderPilesPtr;
            struct FanCluster { ImVec2 anchor; uint64_t key; int pileIdx; };  // pileIdx>=0 → gather from QT
            std::vector<FanCluster> clusters;
            auto qkey = [](float wx, float wz, uint64_t tag) -> uint64_t { return spiderfy_key(wx, wz, tag); };
            // Piles big enough on screen to be worth fanning (else "zoom in to expand" suffices).
            for (int i = 0; i < (int)spiderPiles.size(); ++i)
            {
                const MarkerQuadtree::Pile &pl = spiderPiles[i];
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
                    if (clusters[i].pileIdx >= 0 && spiderPiles[clusters[i].pileIdx].count > bestc)
                    { bestc = spiderPiles[clusters[i].pileIdx].count; best = i; }
                if (best >= 0) { s_fan_open = true; s_fan_key = clusters[best].key; }
            }
            // Hit-test the cursor against a cluster anchor → (re)latch the sticky key.
            for (const FanCluster &fc : clusters)
            {
                float dx = fc.anchor.x - vptr.x, dy = fc.anchor.y - vptr.y;
                const float hitR = 22.0f * uiScale;
                // Gate the OPEN on Ctrl (default) so panning/hovering the map doesn't pop fans as the
                // cursor sweeps clusters; once open the keep-open logic holds it without Ctrl. In pad-mode
                // the reticle (right stick) is separate from panning (left stick), so a deliberate reticle
                // hover opens the fan — no Ctrl on a pad (the no-steal + keep-open logic prevents thrash).
                const bool mod_ok = !goblin::config::spiderfyHoldCtrl || io.KeyCtrl || s_pad_mode;
                // DON'T steal an already-open fan: only latch a NEW cluster when none is open. Otherwise
                // moving the cursor toward a fanned icon could re-latch to a neighbouring islet the path
                // passes over (fan A stops → fan B draws). The open fan closes via keep-open (cursor left
                // its reach) below, and only THEN can the next frame's hit-test open a different one.
                // GRACE LOCK WINS (user 2026-07-28: zoomed out, aiming at a grace opened a neighbouring
                // pile's fan instead — and warping is the map's whole point). plot() has already run this
                // frame, so hoverPrio==1 means "a grace is latched under the pointer, with its 1.7x catch
                // radius". Graces are drawn on top and never clustered (they are not in s_single_screen),
                // so a fan can only ever COVER one, never contain it — no reason to let it steal the aim.
                // Move off the grace and the lock drops, so the fan opens on the very next frame.
                if (!s_fan_open && mod_ok && hoverPrio < 1 && dx * dx + dy * dy <= hitR * hitR) { s_fan_open = true; s_fan_key = fc.key; break; }
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
                    s_qt.gather_pile(spiderPiles[open->pileIdx], members);
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
                    // Space by the LARGEST member footprint, not the plain icon — ring/glow markers
                    // (Golden Runes draw ~1.6× + a glow halo) overlap when packed on icoHalf alone
                    // (user 2026-07-23). glyph_footprint_half returns the decorated extent.
                    float footHalf = icoHalf;
                    for (const FE &e : ents)
                        footHalf = (std::max)(footHalf, goblin::worldmap::glyph_footprint_half(*e.m, icoHalf));
                    const float spacing = 2.0f * footHalf + 8.0f * uiScale;
                    const float baseR = footHalf * 2.4f;
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
                    float mdx = c.x - vptr.x, mdy = c.y - vptr.y;
                    if (!s_force_spiderfy && mdx * mdx + mdy * mdy > keepR * keepR) s_fan_open = false;
                    // MODAL absorb: the fan is drawn on top, so inside its backdrop it must OWN the hover —
                    // clear whatever base marker/pile UNDER it set in the accumulator, so their tooltip/warp
                    // can't leak through the gaps between fanned icons. Only a fanned icon (below) re-sets it.
                    // EXCEPT a grace lock: a fan that drifted over a grace (already open when the pointer
                    // reached it) must not eat the warp target either — same reason as the open gate above.
                    const float absorbR = max_r + icoHalf + 6.0f * uiScale;
                    if (hoverPrio < 1 && mdx * mdx + mdy * mdy <= absorbR * absorbR)
                    {
                        hoverBestD = 1e18f; hoverPrio = -1; hoverName = -1; hoverCat = -1;
                        hoverV.clear(); hoverRow = 0; hoverDisc = 0; hoverAnon = false; hoverMp = nullptr;
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
                        float fdx = pos[k].x - vptr.x, fdy = pos[k].y - vptr.y;
                        if (fdx * fdx + fdy * fdy < icoHalf * icoHalf * 2.0f)
                        {
                            hoverBestD = 0.f; hoverPrio = 2; hoverName = m->name_id; hoverCat = m->category;
                            // Carry the runtime name (mod-agnostic bosses have no FMG id → hoverName is a
                            // synthetic key that resolves to nothing; the tooltip prefers hoverV). Fanned
                            // pile members bypass plot(), so set it here too or every clustered boss shows
                            // unnamed.
                            hoverV = m->live_name; hoverRow = m->row_id; hoverDisc = m->discover_flag;
                            hoverAnon = goblin::worldmap::marker_is_anonymized(*m);
                            hoverMp = m;   // fanned member: provenance follows the icon you point at
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
        // Spoiler-free: hide the item/boss name. Instead of the exact category (which can be the identity,
        // e.g. "Rune Pieces") show only the COARSE type that matches the disc colour — Boss/NPC/POI/Service/
        // Item — so a red "?" reads "Boss" etc. without naming it. Graces are never anonymized (hoverAnon
        // false), so their warp label is unaffected.
        if (hoverAnon)
        {
            nm = "?";
            clab = (hoverCat >= 0) ? goblin::worldmap::anonymized_kind_label(goblin::worldmap::anonymized_kind(hoverCat))
                                   : nullptr;
        }
        // LOCK FEEDBACK: ring the grace the pointer has latched onto, so "I am on it, act now" is
        // visible before you press anything — the catch radius is wider than the icon, so without this
        // the lock would be invisible and warping would still feel like a guess.
        if (hoverGrace)
        {
            const float lr = icoHalf * 1.7f + 2.0f * uiScale;
            dl->AddCircle(hoverPs, lr, IM_COL32(0, 0, 0, 150), 0, 4.0f * uiScale);
            dl->AddCircle(hoverPs, lr,
                          graceDiscovered ? IM_COL32(240, 205, 105, 235) : IM_COL32(160, 160, 160, 200),
                          0, 2.0f * uiScale);
        }
        if (!nm.empty() || clab || hoverGrace)
        {
            pin_tooltip_to_pointer(s_pad_mode, vptr);
            ImGui::BeginTooltip();
            if (!nm.empty()) ImGui::TextUnformatted(nm.c_str());
            else ImGui::TextDisabled("(unnamed)");
            if (clab) ImGui::TextDisabled("%s", clab);
            // Farmable drops are NOT a pickup lying on the ground — they mark respawning ENEMIES that
            // drop a notable farm target. Every other loot marker means "go here and take it", so an
            // item name on this one reads the same way and sends you looking for something that was
            // never there (user 2026-07-28: pointed at a Smithing Stone [7] as a suspected phantom;
            // its provenance was perfectly sound, the marker just means something else). Say so.
            // …but NOT while this marker is anonymized. Farmable drops ARE anonymized (they name the
            // real notable drop), and the whole point of that mode is to show only the coarse kind —
            // singling these out would hand back exactly what it just blurred: in a blackout run,
            // "this ? is a farm spot, not a chest" is a real hint. Silence is the consistent answer;
            // the same "?" as everything else says nothing, which is what was asked for.
            if (!hoverAnon &&
                hoverCat == static_cast<int>(goblin::generated::Category::WorldFarmableCollectible))
                ImGui::TextColored(ImVec4(0.72f, 0.80f, 0.55f, 1.f),
                                   "%s", tr("dropped by enemies here - nothing on the ground"));
            if (hoverGrace)
            {
                graceDiscovered ? ImGui::TextColored(ImVec4(0.90f, 0.78f, 0.35f, 1.f), "double-click to warp")
                                : ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.f), "not discovered — cannot warp");
                // Dev: warp rowId + the grace's REAL area, so a bad grace→rowId mapping shows on hover
                // (a base grace — area 60 — carrying a DLC rowId, or an area-61 grace shown in group 0).
                ImGui::TextDisabled("rowId %llu · area %d · group %d", (unsigned long long)hoverRow,
                                    hoverArea, s_group);
            }
            // ── Provenance block (config::debug_marker_tooltip) ───────────────────────────────
            // "This thing is on the map and not in the game" is otherwise a log-matching exercise:
            // find the marker in [VMFIND], read its lot, chase the source. Pointing at it and being
            // told where it came from collapses that to one hover (user 2026-07-28, a Smithing-Stone
            // forge that is either absent in game or mis-projected). Everything here is what the
            // marker itself carries, so it also answers the OTHER question the same symptom raises:
            // a bad PROJECTION shows as a sane raw map id next to a wrong world position, while a
            // phantom shows as a raw id that has no business existing.
            if (hoverMp && *goblin::overlay_api::cfg_debugMarkerTooltip_ptr())
            {
                const goblin::worldmap::Marker &m = *hoverMp;
                ImGui::Separator();
                const char *srcName = m.source == goblin::worldmap::Source::DiskMSB ? "DiskMSB"
                                    : m.source == goblin::worldmap::Source::Live    ? "Live"
                                                                                    : "Baked";
                ImGui::TextDisabled("src=%s  cat=%d  group=%d  srcArea=%d", srcName, m.category,
                                    m.group, m.srcArea);
                if (m.raw_area >= 0)
                    ImGui::TextDisabled("raw m%02d_%02d_%02d  local(%.0f, %.0f)", m.raw_area, m.raw_gx,
                                        m.raw_gz, m.raw_px, m.raw_pz);
                else
                    ImGui::TextDisabled("raw: none (baked position, no live re-projection)");
                ImGui::TextDisabled("world(%.0f, %.0f)  y=%.0f", m.worldX, m.worldZ, m.worldY);
                if (m.lotId)
                    ImGui::TextDisabled("lot=%u/%u  lot_backed=%d", m.lotId, (unsigned)m.lotType,
                                        m.lot_backed ? 1 : 0);
                ImGui::TextDisabled("row=%llu  name_id=%d  cluster_key=%d",
                                    (unsigned long long)m.row_id, m.name_id, m.cluster_key);
                ImGui::TextDisabled("flags: frag=%d coll=%d clear=%d disc=%d sec=%d",
                                    m.fragment_flag, m.collected_flag, m.cleared_flag,
                                    m.discover_flag, m.secondary_flag);
            }
            ImGui::EndTooltip();
        }
        // Double-click a DISCOVERED grace → fast-travel. Deferred to next frame's top (warp tears down UI
        // state; firing mid-ImGui-draw is unsafe). Double-click avoids the drag-pan click conflict. Gamepad:
        // FaceUp (pad_activate) over a hovered discovered grace does the same (no mouse double-click by pad).
        if (graceDiscovered && (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || pad_activate))
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

    // NOTE: the PLAYER cursor is drawn further down (after custom pins + the death marker) so it sits ON
    // TOP of them — it used to draw here and got covered. See the "Player cursor" block below.

    // Region name labels (A7 parity — the coarse major-region names Limgrave/Caelid/… the native map
    // draws). Base ER only (custom worlds carry no ER regions) and only anchors on the displayed group
    // (underground overlaps the overworld in XZ). Reuses the world-space projection precomputed above so
    // the label rides the same pan/zoom as its region's markers. CLICKABLE, parity with the native chip:
    // a click toggles that region off — which hides its CLUTTER markers (loot/landmarks/bosses) while
    // graces + the player stay visible (see region_hide_exempt above). The on/off flag is the SHARED
    // map_renderer state, so a toggle syncs with the native map and persists via config::regionToggles.
    // Drawn HERE — under the custom pins / death marker / player cursor that follow — so a player-placed
    // pin is never buried under a region name (user 2026-07-28). Only the map's OWN markers pass beneath.
    // Gamepad: the reticle already hovers a chip (io.MousePos IS the reticle in pad-mode), and X activates
    // it — the pad had NO way to toggle a region before, only highlight one (user 2026-07-28).
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
            const bool hot = vptr.x >= r0.x && vptr.x <= r1.x && vptr.y >= r0.y && vptr.y <= r1.y;
            // Pad X over a chip toggles the region and is CONSUMED here, so the same press can't also
            // drop a pin under the label (the place/delete below only runs on a still-pending latch).
            if (hot && pad_place_pending) { pad_place_pending = false; goblin::worldmap::region_set_enabled(i, !goblin::worldmap::region_enabled(i)); }
            else if (hot && clicked)
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

    // Pad place/delete a custom marker at the reticle (mirrors the right-click path above). Deferred to
    // here from the input block so a region chip under the reticle gets the press first (see the latch).
    // Near an existing same-group pin → delete, else drop a new one (cap-enforced).
    if (pad_place_pending)
    {
        pad_place_pending = false;
        float mwx, mwz;
        s2w(s_pad_cursor, mwx, mwz);
        auto cm = goblin::custom_markers::snapshot();
        const int hit = pin_at(s_pad_cursor, cm);
        if (hit >= 0)
            goblin::custom_markers::remove_at((size_t)hit);
        else if (!goblin::custom_markers::add(mwx, mwz, s_group,
                     next_marker_name(), IM_COL32(90, 200, 255, 255)))
            s_tile_status = "custom marker cap reached for this map";
        s_show_custom = true;
    }

    // Custom player-placed markers — drawn under the player cursor (moved below) but OVER the region
    // labels: a colored pin (teardrop) + name, for the markers tagged to the displayed group. Right-click
    // the canvas (or pad X) to drop one.
    if (active_world == 0)
    {
        const std::vector<goblin::custom_markers::Marker> cpins = goblin::custom_markers::snapshot();
        // Which pin the pointer has latched onto — the SAME test the delete uses (pin_at).
        const int lockPin = hovered_eff ? pin_at(vptr, cpins) : -1;
        for (int ci = 0; ci < (int)cpins.size(); ++ci)
        {
            const goblin::custom_markers::Marker &c = cpins[ci];
            if (c.group != s_group) continue;
            ImVec2 p = w2s(c.wx, c.wz);
            if (p.x < origin.x || p.x > canvas_end.x || p.y < origin.y || p.y > canvas_end.y) continue;
            const float r = 8.0f * uiScale;
            const ImVec2 head(p.x, p.y - r * 1.3f);
            // LOCK FEEDBACK, same idea as the grace ring: the catch radius is wider than the pin, so
            // without a ring "am I on it?" is a guess — and here a wrong guess DELETES the wrong pin.
            // Drawn under the pin so it reads as a halo, not an outline.
            if (ci == lockPin)
            {
                dl->AddCircle(head, kPinCatch, IM_COL32(0, 0, 0, 150), 0, 4.0f * uiScale);
                dl->AddCircle(head, kPinCatch, IM_COL32(120, 215, 255, 235), 0, 2.0f * uiScale);
            }
            dl->AddTriangleFilled(ImVec2(p.x - r * 0.7f, p.y - r * 1.3f), ImVec2(p.x + r * 0.7f, p.y - r * 1.3f), p, c.color);
            dl->AddCircleFilled(head, r * 0.7f, c.color);
            dl->AddCircle(head, r * 0.7f, IM_COL32(255, 255, 255, 240), 0, 1.5f * uiScale);
            dl->AddText(ImVec2(p.x + 7.0f * uiScale, p.y - r * 1.3f - 8.0f * uiScale), IM_COL32(230, 240, 255, 255), c.name.c_str());
        }
        // Name + what the next press will do. Drawn after the loop so it wins over a marker tooltip
        // underneath (the pin is on top, so its tooltip should be too).
        if (lockPin >= 0)
        {
            pin_tooltip_to_pointer(s_pad_mode, vptr);
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(cpins[(size_t)lockPin].name.c_str());
            ImGui::TextDisabled("%s", s_pad_mode ? tr("X: delete this marker") : tr("right-click: delete this marker"));
            ImGui::EndTooltip();
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
                // vptr, not io.MousePos: in pad-mode the reticle IS the canvas pointer, so the raw
                // mouse would test a stale position (and place the tooltip at the nav item).
                if (hovered_eff && std::fabs(vptr.x - p.x) <= h && std::fabs(vptr.y - p.y) <= h)
                {
                    pin_tooltip_to_pointer(s_pad_mode, vptr);
                    ImGui::SetTooltip("%s — %d %s", tr("Bloodstain"), dsouls, tr("runes"));
                }
            }
        }
    }

    // NOTE: the region labels are drawn EARLIER now (above the marker loop's output but UNDER the custom
    // pins / death marker / player cursor) so a pin dropped on a region name is not buried by it. The
    // player cursor still draws last of all, so it stays on top of every layer.

    // Co-op partner markers — the native MENU_MAP_Host figure-in-ring, drawn axis-aligned (NO rotation:
    // remote players' facing isn't reliably synced). Under the local player cursor (below), on the current
    // page only. Mod-agnostic (glyph resolved live); v1 same-tile (goblin::coop / get_chr_map_pos).
    if (active_world == 0)
    {
        void *ct = nullptr; float cu0, cv0, cu1, cv1;
        const bool have_host =
            goblin::overlay_api::map_point_glyph_uv("MENU_MAP_Host", -1, ct, cu0, cv0, cu1, cv1) && ct;
        for (const auto &m : goblin::coop::markers())
        {
            if (m.group != s_group) continue;
            ImVec2 pp = w2s(m.wx, m.wz);
            if (pp.x < origin.x || pp.x > canvas_end.x || pp.y < origin.y || pp.y > canvas_end.y) continue;
            if (have_host)
            {
                const float hs = 13.0f * uiScale;   // Host glyph ≈ 74x76 (square) → keep aspect
                dl->AddImageQuad((ImTextureID)ct,
                                 ImVec2(pp.x - hs, pp.y - hs), ImVec2(pp.x + hs, pp.y - hs),
                                 ImVec2(pp.x + hs, pp.y + hs), ImVec2(pp.x - hs, pp.y + hs),
                                 ImVec2(cu0, cv0), ImVec2(cu1, cv0), ImVec2(cu1, cv1), ImVec2(cu0, cv1),
                                 IM_COL32(255, 255, 255, 255));
            }
            else
                dl->AddCircleFilled(pp, 5.0f * uiScale, IM_COL32(255, 210, 80, 255));  // fallback dot
        }
    }

    // Player cursor — drawn AFTER custom pins, the death marker AND the region labels so it sits ON TOP of
    // all of them (the region name pills used to cover it). Only on the displayed group (underground
    // overlaps the overworld in XZ, so a cross-group dot would mislead).
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
                        // The halo is a CONSTANT-px glow, so zoomed right in it becomes a blob covering the
                        // character on the terrain art — fade it out past ~0.3 px/unit (gone by ~1.0).
                        int haloA = 95;
                        if (s_zoom > 0.30f)
                        {
                            const float f = 1.0f - (s_zoom - 0.30f) / 0.70f;  // 1 at 0.30 px/unit → floor by ~1.0
                            // Floor at 55 (never 0): the halo is the ONLY thing distinguishing the pale
                            // player sprite from the gold grace icon, and zoomed in (exactly when you park
                            // ON a grace) it used to fade to nothing → player invisible on a grace. Keep a
                            // faint blue ring always.
                            haloA = f > 0.0f ? (int)(55.0f + 40.0f * f) : 55;
                        }
                        dl->AddCircleFilled(pp, hh * 0.9f, IM_COL32(40, 130, 255, haloA));
                        // Persistent dark outline ring so the player reads over ANY marker colour (gold
                        // grace, teal, bright loot), independent of zoom/halo.
                        dl->AddCircle(pp, hh * 0.62f, IM_COL32(0, 0, 0, 200), 0, 2.0f * uiScale);
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

    // Grid-step legend (bottom-left) so the scale is readable.
    char legend[64];
    std::snprintf(legend, sizeof(legend), "%s: %.0f u", tr("grid"), step);
    dl->AddText(ImVec2(origin.x + 6, canvas_end.y - 18), IM_COL32(150, 158, 172, 255), legend);

    // Marker-extractor selection box (dev) — drawn ON TOP while dragging so the user sees the region.
    if (s_extract_mode && s_extract_dragging)
    {
        dl->AddRectFilled(s_extract_a, s_extract_b, IM_COL32(90, 200, 255, 45));
        dl->AddRect(s_extract_a, s_extract_b, IM_COL32(130, 210, 255, 230), 0, 0, 1.5f * uiScale);
    }

    // Gamepad virtual cursor (M4) — a reticle drawn ON TOP so the player sees what the RIGHT stick is
    // aiming at. Shown only in pad-mode (hidden once a real mouse move takes over). Dark halo for contrast.
    if (s_pad_mode && s_pad_cursor_init)
    {
        // GRACE LOCK (pad half): while a grace is caught, DRAW the reticle on the grace itself — the
        // aim-assist "clunk" that tells you the stick can stop now. Only the drawn position moves;
        // s_pad_cursor is untouched, so the stick keeps its exact 1:1 feel and pushing on simply
        // leaves the grace's radius. Snapping the real cursor would fight the player's own input.
        const ImVec2 c = hoverGrace ? hoverPs : s_pad_cursor;
        const float r = 10.0f * uiScale, g = 3.0f * uiScale;
        dl->AddCircle(c, r, IM_COL32(0, 0, 0, 170), 0, 4.0f * uiScale);
        dl->AddCircle(c, r, IM_COL32(255, 255, 255, 235), 0, 2.0f * uiScale);
        for (int s = 0; s < 4; ++s)
        {
            ImVec2 a = s == 0 ? ImVec2(c.x - r - 4 * uiScale, c.y) : s == 1 ? ImVec2(c.x + g, c.y)
                     : s == 2 ? ImVec2(c.x, c.y - r - 4 * uiScale) : ImVec2(c.x, c.y + g);
            ImVec2 b = s == 0 ? ImVec2(c.x - g, c.y) : s == 1 ? ImVec2(c.x + r + 4 * uiScale, c.y)
                     : s == 2 ? ImVec2(c.x, c.y - g) : ImVec2(c.x, c.y + r + 4 * uiScale);
            dl->AddLine(a, b, IM_COL32(255, 255, 255, 235), 2.0f * uiScale);
        }
        dl->AddCircleFilled(c, 1.6f * uiScale, IM_COL32(255, 80, 80, 255));
    }

    // TODO(slice B): project the mod's markers for the virtual world's group here (w2s per marker + the
    // marker icon draw), then (slice C) tag markers to a synthetic group / bundle-backed custom world.
    dl->PopClipRect();

    // A warp is queued (from the sidebar row, the sidebar Y, or the reticle Y — all three land in the
    // same latch) → CLOSE the map, like the native one does when you fast-travel. Otherwise the panel
    // stayed up and clickable across the load: you could queue a second warp, or fiddle with buttons
    // belonging to a map you have already left (user 2026-07-28). Done here, once, after everything has
    // drawn — the warp itself is serviced POST-frame by virtual_map_service_pending_warp(), which does
    // not care whether the panel is open, so nothing is lost by closing first.
    if (s_warp_pending) close_vmap();

    if (s_pad_mode) io.MousePos = s_pad_saved_mouse;   // restore the real mouse (see the top-of-fn override)
    ImGui::End();
}

// ── vmap RPC command dispatch (render-side) ───────────────────────────────────────────────────────
// The host's `vmap <sub> [args]` RPC verb forwards its whole argument string here so the WHOLE vmap
// dispatch lives with the panel code it drives — one loader export (MFG_VmapCommand) instead of ~18,
// and a NEW vmap verb never re-breaks the hot-reload split. Moved verbatim from goblin_debug_rpc.cpp
// (2026-07-05 split resync); calls the local panel functions directly (same module).
static std::string vmap_next_token(std::string &s)
{
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) { s.clear(); return {}; }
    size_t e = s.find_first_of(" \t", b);
    std::string tok = s.substr(b, e == std::string::npos ? std::string::npos : e - b);
    s = (e == std::string::npos) ? std::string{} : s.substr(e + 1);
    return tok;
}
// RE probe (dungeon_entrance_fallback_anchor_plan Slice 3): dump an EMEVD's bank-2000 inits to hunt
// the overworld→dungeon warp. No needle → a template-id histogram (what event templates the map uses).
// With a needle (entityId / template / any arg word, decimal or 0x) → the full arg list of every init
// referencing it — used to find the entrance warp by its known trigger entity or destination-map arg.
static std::string emevd_probe(const std::string &mapName, uint32_t needle, bool hasNeedle)
{
    const std::string rel = "event/" + mapName + ".emevd.dcx";
    std::vector<uint8_t> evd = goblin::worldmap::read_game_file_decompressed(rel);
    if (evd.size() < 0x80) return "err emevd: '" + rel + "' not found / too small";
    std::vector<goblin::msbe::EmevdInit> inits = goblin::msbe::emevd_inits(evd.data(), evd.size());
    spdlog::info("[EMEVDUMP] {} : {} bytes, {} inits, needle={}", rel, evd.size(), inits.size(),
                 hasNeedle ? std::to_string(needle) : std::string("none"));
    if (!hasNeedle)
    {
        std::map<uint32_t, int> hist;
        for (const auto &r : inits) hist[r.tmpl]++;
        for (const auto &[t, c] : hist) spdlog::info("[EMEVDUMP]   tmpl={} x{}", t, c);
        char out[144];
        std::snprintf(out, sizeof(out), "ok vmap emevd '%s': %zu inits, %zu templates — see [EMEVDUMP]",
                      mapName.c_str(), inits.size(), hist.size());
        return out;
    }
    size_t hits = 0;
    for (const auto &r : inits)
    {
        bool m = (r.event == needle || r.tmpl == needle);
        if (!m) for (uint32_t w : r.args) if (w == needle) { m = true; break; }
        if (!m) continue;
        ++hits;
        std::string as;
        for (size_t k = 0; k < r.args.size(); ++k)
        { char t[24]; std::snprintf(t, sizeof(t), "%s%u", k ? "," : "", r.args[k]); as += t; }
        spdlog::info("[EMEVDUMP]   ev={} tmpl={} args=[{}]", r.event, r.tmpl, as);
    }
    char out[144];
    std::snprintf(out, sizeof(out), "ok vmap emevd '%s' needle=%u: %zu match(es) — see [EMEVDUMP]",
                  mapName.c_str(), needle, hits);
    return out;
}

// All-instruction modes (round 2): `banks` = histogram of (bank:id) pairs across ALL instructions
// (find the warp instruction by its bank/id); `bank <N>` = dump every instruction of bank N with args
// (e.g. bank 2003 = WarpPlayer family) → read the destination-map arg directly.
static std::string emevd_banks(const std::string &mapName)
{
    std::vector<uint8_t> evd = goblin::worldmap::read_game_file_decompressed("event/" + mapName + ".emevd.dcx");
    if (evd.size() < 0x80) return "err emevd: '" + mapName + "' not found / too small";
    std::vector<goblin::msbe::EmevdInstr> ins = goblin::msbe::emevd_all_instrs(evd.data(), evd.size());
    std::map<uint64_t, int> hist;  // (bank<<32|id) -> count
    for (const auto &r : ins) hist[((uint64_t)r.bank << 32) | r.id]++;
    spdlog::info("[EMEVDUMP] {} : {} instructions, {} distinct (bank:id)", mapName, ins.size(), hist.size());
    for (const auto &[k, c] : hist)
        spdlog::info("[EMEVDUMP]   bank={} id={} x{}", (uint32_t)(k >> 32), (uint32_t)(k & 0xffffffff), c);
    char out[144];
    std::snprintf(out, sizeof(out), "ok vmap emevd '%s' banks: %zu instrs, %zu (bank:id) — see [EMEVDUMP]",
                  mapName.c_str(), ins.size(), hist.size());
    return out;
}

static std::string emevd_bank_dump(const std::string &mapName, uint32_t bank)
{
    std::vector<uint8_t> evd = goblin::worldmap::read_game_file_decompressed("event/" + mapName + ".emevd.dcx");
    if (evd.size() < 0x80) return "err emevd: '" + mapName + "' not found / too small";
    std::vector<goblin::msbe::EmevdInstr> ins = goblin::msbe::emevd_all_instrs(evd.data(), evd.size());
    size_t hits = 0;
    for (const auto &r : ins)
    {
        if (r.bank != bank) continue;
        ++hits;
        std::string as;
        for (size_t k = 0; k < r.args.size(); ++k)
        { char t[24]; std::snprintf(t, sizeof(t), "%s%u", k ? "," : "", r.args[k]); as += t; }
        spdlog::info("[EMEVDUMP]   ev={} bank={} id={} args=[{}]", r.event, r.bank, r.id, as);
    }
    char out[144];
    std::snprintf(out, sizeof(out), "ok vmap emevd '%s' bank=%u: %zu instr(s) — see [EMEVDUMP]",
                  mapName.c_str(), bank, hits);
    return out;
}

// RE probe (Slice 3 path b): dump an MSB's Parts. No type → part-type histogram (find the
// ConnectCollision type). With a type → each part's name, pos, and first 8 typeData words (read the
// ConnectCollision's target MapID from typeData). `vmap msbparts <mapName> [partType]`.
static std::string msbparts_probe(const std::string &mapName, int typeFilter, bool hasType)
{
    std::vector<uint8_t> msb =
        goblin::worldmap::read_game_file_decompressed("map/MapStudio/" + mapName + ".msb.dcx");
    if (msb.size() < 0x10) return "err msbparts: '" + mapName + "' msb not found / too small";
    std::vector<goblin::msbe::MsbPart> parts = goblin::msbe::dump_parts(msb.data(), msb.size());
    spdlog::info("[MSBPARTS] {} : {} parts", mapName, parts.size());
    if (!hasType)
    {
        std::map<int, int> hist;
        for (const auto &p : parts) hist[p.type]++;
        for (const auto &[t, c] : hist) spdlog::info("[MSBPARTS]   type={} x{}", t, c);
        char out[144];
        std::snprintf(out, sizeof(out), "ok vmap msbparts '%s': %zu parts, %zu types — see [MSBPARTS]",
                      mapName.c_str(), parts.size(), hist.size());
        return out;
    }
    size_t hits = 0;
    for (const auto &p : parts)
    {
        if (p.type != typeFilter) continue;
        ++hits;
        // typeData @+0x68: for a ConnectCollision (type 11) it is `int CollisionIndex; sbyte MapID[4]`,
        // so td[0]=CollisionIndex, td[1] bytes = MapID [area,block,cc,dd] (decoded here as m<AA>_<BB>).
        const int32_t mid = p.td[1];
        const int mA = mid & 0xff, mB = (mid >> 8) & 0xff, mC = (mid >> 16) & 0xff, mD = (mid >> 24) & 0xff;
        spdlog::info("[MSBPARTS]   type={} '{}' pos({:.1f},{:.1f},{:.1f}) collIdx={} MapID=m{:02d}_{:02d}_{:02d}_{:02d} "
                     "td=[{},{},{},{}]",
                     p.type, p.name, p.pos[0], p.pos[1], p.pos[2], p.td[0], mA, mB, mC, (int8_t)mD,
                     p.td[0], p.td[1], p.td[2], p.td[3]);
    }
    char out[144];
    std::snprintf(out, sizeof(out), "ok vmap msbparts '%s' type=%d: %zu part(s) — see [MSBPARTS]",
                  mapName.c_str(), typeFilter, hits);
    return out;
}

std::string vmap_rpc_command(std::string rest)
{
    std::string arg = vmap_next_token(rest);
    if (arg == "fit") { virtual_map_open() = true; virtual_map_request_fit(); return "ok vmap fit"; }
    if (arg == "group")
    {
        std::string gs = vmap_next_token(rest);
        if (gs.empty())                                   // no arg → REPORT the current page (for tests/RPC)
            return "ok vmap group=" + std::to_string(virtual_map_group());
        int g = 0;
        try { g = std::stoi(gs); } catch (...) { return "err usage: vmap group [0-3]"; }
        virtual_map_set_group(g);
        return "ok vmap group=" + std::to_string(virtual_map_group());
    }
    if (arg == "tile")
    {
        std::string needle = vmap_next_token(rest);
        if (needle.empty()) return "err usage: vmap tile <needle> [wx0 wz0 wx1 wz1]";
        float r[4] = {0, 0, 0, 0};
        for (int i = 0; i < 4; ++i)
        {
            std::string t = vmap_next_token(rest);
            if (t.empty()) break;
            try { r[i] = std::stof(t); } catch (...) {}
        }
        virtual_map_request_tile(needle.c_str(), r[0], r[1], r[2], r[3]);
        return "ok vmap tile requested " + needle;
    }
    if (arg == "tiles_clear") { virtual_map_clear_tiles(); return "ok vmap tiles_clear"; }
    if (arg == "tiles_resident") return "ok vmap tiles_resident " + virtual_map_load_resident();
    if (arg == "tile_recon") return "ok vmap tile_recon " + virtual_map_tile_recon();
    if (arg == "items")
    {
        std::string q = rest;
        if (!q.empty() && q[0] == ' ') q.erase(0, 1);
        virtual_map_item_search(q.c_str());
        return "ok vmap items query=\"" + q + "\"";
    }
    if (arg == "spiderfy")
    {
        bool on = (vmap_next_token(rest) != "0");
        virtual_map_force_spiderfy(on);
        return std::string("ok vmap spiderfy ") + (on ? "1" : "0");
    }
    if (arg == "offmap") return virtual_map_offmap_probe();
    if (arg == "graces")
    {
        std::string gs = vmap_next_token(rest);
        int gf = -1;
        if (!gs.empty()) { try { gf = std::stoi(gs); } catch (...) { return "err usage: vmap graces [group 0-2]"; } }
        return virtual_map_graces_dump(gf);
    }
    if (arg == "find")
    {
        std::string q = rest;
        size_t b = q.find_first_not_of(" \t");
        size_t e = q.find_last_not_of(" \t");
        q = (b == std::string::npos) ? std::string{} : q.substr(b, e - b + 1);
        if (q.empty()) return "err usage: vmap find <name|name_id>";
        return virtual_map_find(q);
    }
    if (arg == "ename")
    {
        std::string q = rest;
        size_t b = q.find_first_not_of(" \t");
        size_t e = q.find_last_not_of(" \t");
        q = (b == std::string::npos) ? std::string{} : q.substr(b, e - b + 1);
        if (q.empty()) return "err usage: vmap ename <substr | area gx gz>";
        return goblin::worldmap::ename_probe(q);
    }
    if (arg == "prov")
    {
        std::string q = rest;
        size_t b = q.find_first_not_of(" \t");
        size_t e = q.find_last_not_of(" \t");
        q = (b == std::string::npos) ? std::string{} : q.substr(b, e - b + 1);
        if (q.empty()) return "err usage: vmap prov <lot|item-name>";
        return goblin::worldmap::loot_prov_probe(q);
    }
    if (arg == "emevd")
    {
        std::string mn = vmap_next_token(rest);
        if (mn.empty())
            return "err usage: vmap emevd <mapName> [needle | banks | bank <N>]";
        std::string mode = vmap_next_token(rest);
        if (mode == "banks") return emevd_banks(mn);
        if (mode == "bank")
        {
            std::string bs = vmap_next_token(rest);
            uint32_t b = 0; try { b = (uint32_t)std::stoul(bs, nullptr, 0); } catch (...) { return "err usage: vmap emevd <map> bank <N>"; }
            return emevd_bank_dump(mn, b);
        }
        uint32_t needle = 0; bool has = false;
        if (!mode.empty()) { try { needle = (uint32_t)std::stoul(mode, nullptr, 0); has = true; } catch (...) {} }
        return emevd_probe(mn, needle, has);
    }
    if (arg == "msbparts")
    {
        std::string mn = vmap_next_token(rest);
        if (mn.empty()) return "err usage: vmap msbparts <mapName> [partType]";
        std::string ts = vmap_next_token(rest);
        int t = 0; bool has = false;
        if (!ts.empty()) { try { t = std::stoi(ts); has = true; } catch (...) {} }
        return msbparts_probe(mn, t, has);
    }
    if (arg == "relief")
    {
        bool on = (vmap_next_token(rest) != "0");
        virtual_map_set_relief(on);
        return std::string("ok vmap relief ") + (on ? "1" : "0");
    }
    if (arg == "locate")
    {
        std::string ns = vmap_next_token(rest), gs = vmap_next_token(rest);
        int32_t nid = 0; int grp = 0;
        try { nid = (int32_t)std::stol(ns, nullptr, 0); } catch (...) { return "err usage: vmap locate <name_id> [group]"; }
        if (!gs.empty()) { try { grp = std::stoi(gs); } catch (...) {} }
        int found = virtual_map_locate(nid, grp);
        char b[96];
        std::snprintf(b, sizeof(b), "ok vmap locate name_id=%d group=%d -> %d instance(s)%s",
                      nid, grp, found, found ? " (centred)" : " (no such marker)");
        return std::string(b);
    }
    if (arg == "flip")
    {
        std::string m = vmap_next_token(rest);
        bool fx = (m == "x" || m == "xz"), fz = (m == "z" || m == "xz");
        virtual_map_set_flip(fx, fz);
        return "ok vmap flip x=" + std::to_string(fx) + " z=" + std::to_string(fz);
    }
    if (arg == "dump_markers")
    {
        std::string p = vmap_next_token(rest);
        if (p.empty()) return "err usage: vmap dump_markers <path.csv>";
        int n = dump_markers_csv(p.c_str());
        return n >= 0 ? "ok vmap dump_markers " + std::to_string(n) + " -> " + p
                      : "err dump_markers: file open failed";
    }
    if (arg == "view")
    {
        std::string xs = vmap_next_token(rest), zs = vmap_next_token(rest), zm = vmap_next_token(rest);
        float cx = 0, cz = 0, z = 0;
        try { cx = std::stof(xs); cz = std::stof(zs); if (!zm.empty()) z = std::stof(zm); }
        catch (...) { return "err usage: vmap view <camX> <camZ> <zoom>"; }
        virtual_map_set_view(cx, cz, z);
        return "ok vmap view";
    }
    if (arg == "tiles_lod")
    {
        std::string ds = vmap_next_token(rest), ls = vmap_next_token(rest), cs = vmap_next_token(rest);
        int dim = 0, lod = 3, cap = 240;
        try { dim = std::stoi(ds); lod = std::stoi(ls); } catch (...) { return "err usage: vmap tiles_lod <dim> <lod> [cap]"; }
        if (!cs.empty()) { try { cap = std::stoi(cs); } catch (...) {} }
        return "ok vmap tiles_lod " + virtual_map_load_lod(dim, lod, cap);
    }
    if (arg == "0") virtual_map_open() = false;
    else if (arg == "1") virtual_map_open() = true;
    else if (arg == "toggle" || arg.empty()) virtual_map_open() = !virtual_map_open();
    else return "err vmap takes 0|1|toggle | fit | group <0-3> | graces [group] | offmap | tile <needle> [rect] | tiles_lod <dim> <lod> [cap] | tiles_clear";
    return "ok vmap=" + std::to_string(virtual_map_open() ? 1 : 0);
}
} // namespace goblin::overlay::panel
