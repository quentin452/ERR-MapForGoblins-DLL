#include "goblin_inject.hpp"
#include "modutils.hpp"
#include "re_signatures.hpp"
#include "from/params.hpp"
#include "from/paramdef/WORLD_MAP_POINT_PARAM_ST.hpp"
#include "from/paramdef/BONFIRE_WARP_PARAM_ST.hpp"
#include "from/paramdef/BONFIRE_WARP_SUB_CATEGORY_PARAM_ST.hpp"
#include "goblin_messages.hpp"
#include "goblin_map_data.hpp"
#include "goblin_legacy_fold.hpp"
#include "goblin_legacy_conv.hpp"
#include "goblin_logic.hpp"
#include "goblin_name_regions.hpp"
#include "goblin_tile_tabs.hpp"
#include "goblin_region_anchors.hpp"
#include "goblin_major_regions.hpp"

#include <cmath>
#include <map>
#include <string>
#include <vector>
#include <spdlog/spdlog.h>
#include <windows.h>

//
// World-position/grace-data domain — split out of goblin_inject.cpp 2026-07-01
// (docs/plans/goblin_inject_refactor_plan.md PR 4a). Pure relocation, no logic
// changes. Owns: the dungeon-to-overworld legacy-fold projection, runtime
// grace anchors + region/cluster naming, live WorldChrMan player-position
// resolve (world + map-space + raw), live grace capture, and the
// marker_fragment_flag/marker_world_pos thin wrappers over the fold.
// Fully self-contained per audit — every file-static symbol here is used
// only within this span, zero coupling to anything else still in
// goblin_inject.cpp (confirmed via a file-wide grep, not just a local read).
// Two dead-but-still-referenced-elsewhere globals (g_injected_row_ptrs,
// g_lot_backed_set) sit textually between the dungeon-fold and the grace-
// anchors banner in the original file — they stay in goblin_inject.cpp
// (apply_flag_or_pairs/injected_row_ptrs() there still reference them, even
// though PR 4's scoping audit found that whole subsystem is currently dead —
// goblin::inject_map_entries() has no implementation anywhere in the
// codebase). Declarations in goblin_inject.hpp unchanged (facade kept).
//

// Bug A fix: minor dungeons (catacombs/caves/tunnels/hero's graves) have no
// in-game map page, so rows injected with their dungeon areaNo are never
// rendered. The game ships WorldMapLegacyConvParam (baked here as LEGACY_CONV)
// describing how each such sub-map projects onto the overworld (areaNo 60/61).
// We apply that conversion in-place to the injected row so its icon appears
// near the dungeon entrance. Rows already on the overworld, or with no conv
// entry (legacy dungeons that have their own page / unmappable), are untouched.
static bool project_dungeon_row_to_overworld(
    from::paramdef::WORLD_MAP_POINT_PARAM_ST *d,
    float *out_ent_x = nullptr, float *out_ent_z = nullptr,
    bool conv_underground = false)
{
    if (d->areaNo == 60 || d->areaNo == 61)
        return false;
    // INJECTION (conv_underground=false): area 12 / 40-43 have their OWN native world-map
    // page, so keep them native (projecting them onto the overworld surface lands them "in
    // the sea"). OVERLAY (conv_underground=true): the agent's RE proved underground SHARES
    // the overworld map-space (one converter, no area-12 converter) — the underground map is
    // a LAYER of the same space — so area 12 IS projected to unified coords (its conv rows
    // map area-12-local → area-60 geographic), and the overlay draws it on the UG layer
    // (gated separately by ORIGINAL areaNo). DLC underground (40-43) stays native for now
    // (DLC is still on the eyeball path).
    if (d->areaNo >= 40 && d->areaNo <= 43)
        return false;
    if (d->areaNo == 12 && !conv_underground)
        return false;

    // Prefer the LIVE param fold (goblin_legacy_fold): full-block key (area,gx,gz),
    // terminal = area in [50,88], chains composed at fold time — fixes the area-16
    // wrong-region bug + chained dst (m35→11→60) + reads the regulation directly so
    // it never drifts when a mod edits WorldMapLegacyConvParam. Falls through to the
    // baked LEGACY_CONV below when the param isn't resident yet or no chain applies.
    {
        auto fr = goblin::legacy_fold::fold(d->areaNo, d->gridXNo, d->gridZNo, d->posX, d->posZ);
        if (fr.matched)
        {
            if (out_ent_x) *out_ent_x = fr.ent_x;
            if (out_ent_z) *out_ent_z = fr.ent_z;
            d->areaNo = fr.area;
            d->gridXNo = fr.gx;
            d->gridZNo = fr.gz;
            d->posX = fr.posX;
            d->posZ = fr.posZ;
            return true;
        }
        // Live param resident but no row for this block → genuinely unmappable; the
        // baked table can't do better, so don't bother scanning it.
        if (goblin::legacy_fold::available())
            return false;
    }

    // BAKED fallback (param not loaded yet). Prefer an EXACT (src_area, src_gx, src_gz) base-point — its local coords share
    // an origin with this row, so we keep the in-dungeon offset. Else fall back to the
    // NEAREST base-point of the same src_area (by grid distance) and cluster at that
    // overworld entrance — visible, region-correct, if without intra-dungeon spread.
    // (Was: the FIRST entry of the area, which sent the few rows whose gridX matches
    //  no entry — e.g. an m31 cave grace at gx 8 — to an arbitrary far cave mouth.)
    const goblin::generated::LegacyConvEntry *exact = nullptr;
    const goblin::generated::LegacyConvEntry *nearest = nullptr;
    int best_dist = 0x7fffffff;
    for (size_t i = 0; i < goblin::generated::LEGACY_CONV_COUNT; ++i)
    {
        const auto &c = goblin::generated::LEGACY_CONV[i];
        if (c.src_area != d->areaNo)
            continue;
        int dgx = (int)c.src_gx - (int)d->gridXNo; if (dgx < 0) dgx = -dgx;
        int dgz = (int)c.src_gz - (int)d->gridZNo; if (dgz < 0) dgz = -dgz;
        int dist = dgx + dgz;
        if (dist < best_dist) { best_dist = dist; nearest = &c; }
        if (c.src_gx == d->gridXNo && c.src_gz == d->gridZNo)
        {
            exact = &c;
            break;
        }
    }
    const auto *c = exact ? exact : nearest;
    if (!c)
        return false;

    // Base-point TRANSLATION (RE: docs/marker_to_mapspace_re_findings.md §2). The
    // legacy-dungeon→overworld conv is a uniform translation: take the marker's FULL
    // world offset (grid + pos) from the src base point and apply it to the dst base
    // point. The previous code added only the LOCAL pos delta (posX-src_pos_x),
    // dropping the (gridXNo-src_gx)·256 grid term + src_gz entirely → markers in a
    // different grid cell than the base landed in the wrong region (the area-16 bug).
    float dst_base_x = static_cast<float>(c->dst_gx) * 256.0f + c->dst_pos_x;
    float dst_base_z = static_cast<float>(c->dst_gz) * 256.0f + c->dst_pos_z;
    // The conv base point IS the dungeon's overworld ENTRANCE — hand it back so a
    // cluster of this dungeon's markers can sit there instead of at the centroid
    // of their spread-out projected interior (which can drift off into the sea).
    if (out_ent_x) *out_ent_x = dst_base_x;
    if (out_ent_z) *out_ent_z = dst_base_z;
    float marker_x = static_cast<float>(d->gridXNo) * 256.0f + d->posX;
    float marker_z = static_cast<float>(d->gridZNo) * 256.0f + d->posZ;
    float src_base_x = static_cast<float>(c->src_gx) * 256.0f + c->src_pos_x;
    float src_base_z = static_cast<float>(c->src_gz) * 256.0f + c->src_pos_z;
    float wx = dst_base_x + (marker_x - src_base_x);
    float wz = dst_base_z + (marker_z - src_base_z);
    // GUARD: a few baked rows carry abnormal local coords (e.g. area-11 grid(10,0)
    // posX=-4695) → the translation sends wx/wz out of the overworld tile extent →
    // gridXNo (uint8) wraps → the game's icon build (FUN_141eb9ed0) indexes an
    // out-of-range tile and CRASHES on map open. Out of range → fall back to the
    // entrance base point (the old clustering behaviour, always valid).
    if (wx < 0.f || wz < 0.f || wx > 0x3F * 256.0f || wz > 0x3F * 256.0f)
    {
        wx = dst_base_x;
        wz = dst_base_z;
    }
    int gx = static_cast<int>(std::floor(wx / 256.0f));
    int gz = static_cast<int>(std::floor(wz / 256.0f));
    d->areaNo = c->dst_area;
    d->gridXNo = static_cast<uint8_t>(gx);
    d->gridZNo = static_cast<uint8_t>(gz);
    d->posX = wx - static_cast<float>(gx) * 256.0f;
    d->posZ = wz - static_cast<float>(gz) * 256.0f;
    return true;
}

// ── Runtime grace anchors (replacing baked GRACE_ANCHORS) ────────────────────────────
// Built once from the LIVE grace list (g_live_graces, BonfireWarpParam) + the live
// BonfireWarpSubCategoryParam (subCat → region PlaceName + map sub-page tabId). Mirrors the
// old offline tools/build_grace_anchors.py exactly: placename_id = subCat if it resolves to a
// real PlaceName else 0; tab_id = subcat param's tabId. Removes the baked grace_position_index
// dependency from the DLL (graces already read live everywhere else). Built lazily on first
// access so the PlaceName FMG patch (setup_messages, runs after capture_live_graces) is ready.
static std::vector<goblin::GraceAnchor> g_rt_grace_anchors;
static bool g_rt_grace_built = false;

static void build_runtime_grace_anchors()
{
    g_rt_grace_built = true;
    g_rt_grace_anchors.clear();
    // subCategoryId → tabId from the live sub-category param (row id == bonfireSubCategoryId).
    std::map<int, int> subtab;
    try
    {
        for (auto [rid, row] : from::params::get_param<
                 from::paramdef::BONFIRE_WARP_SUB_CATEGORY_PARAM_ST>(L"BonfireWarpSubCategoryParam"))
            subtab[(int)rid] = (int)row.tabId;
    }
    catch (...) {}

    for (const goblin::LiveGrace &e : goblin::live_graces())
    {
        goblin::GraceAnchor a{};
        a.area = e.areaNo;
        a.wx = (float)e.gridXNo * 256.0f + e.posX;
        a.wz = (float)e.gridZNo * 256.0f + e.posZ;
        // region label = the subCat, but only when it's a real PlaceName (some underground/DLC
        // graces store a map TAB id there, not a place name → no label, like the baker dropped).
        a.placename_id = (e.subCat > 0 && goblin::lookup_text(e.subCat)) ? e.subCat : 0;
        auto it = subtab.find(e.subCat);
        a.tab_id = (it != subtab.end()) ? it->second : 0;
        a.gridX = e.gridXNo;
        a.gridZ = e.gridZNo;
        a.posX = e.posX;
        a.posZ = e.posZ;
        g_rt_grace_anchors.push_back(a);
    }
    spdlog::info("[GRACE-ANCHOR] built {} live anchors from BonfireWarpParam + SubCategoryParam",
                 g_rt_grace_anchors.size());
}

const std::vector<goblin::GraceAnchor> &goblin::grace_anchors()
{
    if (!g_rt_grace_built) build_runtime_grace_anchors();
    return g_rt_grace_anchors;
}

// Nearest Site-of-Grace anchor to a world point WITHIN THE SAME AREA. Graces are
// the authoritative named-location anchors (BonfireWarpParam); assigning each
// marker to its nearest grace gives a real, complete location grouping with no
// dependence on the marker's (often-missing) location textId. Returns the grace
// index + its region PlaceName id (for the label).
static bool find_nearest_grace(uint8_t area, float wx, float wz,
                               int *out_idx, int *out_pname, int *out_tab)
{
    const auto &A = goblin::grace_anchors();
    int best = -1, best_named = -1;
    float bestd = 1e30f, bestnd = 1e30f;
    for (size_t i = 0; i < A.size(); i++)
    {
        const auto &g = A[i];
        if (g.area != area) continue;
        float dx = g.wx - wx, dz = g.wz - wz;
        float d = dx * dx + dz * dz;
        if (d < bestd) { bestd = d; best = static_cast<int>(i); }
        // Track the nearest grace that actually has a name, for a label fallback.
        if (g.placename_id > 0 && d < bestnd) { bestnd = d; best_named = static_cast<int>(i); }
    }
    if (best < 0) return false;
    const auto &b = A[best];
    *out_idx = best;                      // grouping: physical-nearest grace
    *out_tab = b.tab_id;
    // Label: the grouped grace's name, else borrow the nearest NAMED grace's region
    // name (some underground/DLC graces store a tab id, not a PlaceName → no name).
    *out_pname = (b.placename_id > 0) ? b.placename_id
               : (best_named >= 0 ? A[best_named].placename_id : 0);
    return true;
}

// Region PlaceName for a marker via the GAME's own logic = point-in-volume containment
// against the MSB MapNameOverride volumes (goblin_name_regions), in the marker's MSB map
// LOCAL frame (area+gridX+gridZ keys the map; posX/posZ are that map's local coords, same
// as the volume's). Returns the PlaceName textId of the SMALLEST (most specific) containing
// volume, or 0 = no volume here (caller falls back to the nearest-grace pname). Far more
// reliable than nearest-anchor for tooltips + cluster labels (cities, region borders).
int goblin::region_name_pname(uint8_t area, uint8_t gx, uint8_t gz, float posX, float posZ)
{
    namespace gen = goblin::generated;
    int near_tid = 0;        // nearest-volume fallback (same map) when nothing contains
    float near_d = 1e30f;
    for (size_t i = 0; i < gen::NAME_REGION_COUNT; ++i) // sorted smallest-first per map
    {
        const auto &r = gen::NAME_REGIONS[i];
        if (r.area != area || r.gx != gx || r.gz != gz)
            continue;
        // Point into the volume's local frame (inverse yaw about its centre).
        float dx = posX - r.px, dz = posZ - r.pz;
        float c = std::cos(r.rot), s = std::sin(r.rot);
        float lx = dx * c + dz * s;
        float lz = -dx * s + dz * c;
        bool inside = (r.shape == 0)
            ? (std::fabs(lx) <= r.half_w && std::fabs(lz) <= r.half_d)
            : (lx * lx + lz * lz <= r.radius * r.radius);
        if (inside)
            return r.text_id; // contained = smallest (sorted) = most specific
        // Track the nearest volume's centre as a fallback. The MapNameOverride volumes only
        // cover specific named sub-areas (small boxes), so most markers sit OUTSIDE every
        // volume — but the nearest named volume IN THE SAME MSB MAP is almost always the
        // right region (regions are contiguous; a map with both Nokron + Siofra volumes
        // disambiguates by proximity). Far better than the nearest-grace heuristic.
        float d2 = dx * dx + dz * dz;
        if (d2 < near_d) { near_d = d2; near_tid = r.text_id; }
    }
    return near_tid; // 0 only when this map has NO named volume → caller falls back to grace
}

// Cluster grouping key for a marker = its nearest Site-of-Grace index (within the
// marker's SOURCE area + raw world gridX*256+pos), matching the native map's
// by-location clustering (replan_clusters grp_key). Returns -1 when no grace anchor
// shares the area → the caller draws it exact. out_pname (optional) = the marker's
// region PlaceName id (MapNameOverride containment, else the group's grace region).
// LABEL fallbacks (defined below) — nearest MSB region, then nearest major-region
// anchor (which borrows area 61 for the DLC underground areas 40-43).
static int find_nearest_region_pname(uint8_t area, float wx, float wz);
static int find_nearest_major_region_pname(uint8_t area, float wx, float wz);

int goblin::marker_cluster_key(uint8_t area, uint8_t gridX, uint8_t gridZ, float posX,
                               float posZ, int *out_pname)
{
    float mwx = static_cast<float>(gridX) * 256.0f + posX;
    float mwz = static_cast<float>(gridZ) * 256.0f + posZ;
    int idx = -1, pname = -1, tab = 0;
    bool has_grace = find_nearest_grace(area, mwx, mwz, &idx, &pname, &tab);
    if (out_pname)
    {
        // Prefer the game's own region naming (point-in-volume); fall back to the
        // nearest grace's region when no MapNameOverride volume contains the marker.
        int region = region_name_pname(area, gridX, gridZ, posX, posZ);
        int pn = region ? region : (has_grace ? pname : 0);
        // Last resort (e.g. DLC underground 40-43: no named volume, no in-area grace):
        // nearest MSB region, then the major-region anchor (borrows area 61 for 40-43),
        // so the tooltip shows a location like the native map instead of a bare name.
        if (!pn) pn = find_nearest_region_pname(area, mwx, mwz);
        if (!pn) pn = find_nearest_major_region_pname(area, mwx, mwz);
        *out_pname = pn ? pn : -1;
    }
    return has_grace ? idx : -1;
}

// Project a grace anchor (by its GRACE_ANCHORS index = a marker's cluster_key) to
// UNIFIED world coords via the same marker_world_pos pipeline the markers use, so the
// overlay can place a cluster pile AT its grace (a real, correctly-placed location)
// instead of the member centroid (which drifts into the sea when a group spans water
// or has a mis-projected member). Returns false on a bad key.
bool goblin::grace_anchor_world(int key, int &out_area, float &wx, float &wz)
{
    const auto &A = goblin::grace_anchors();
    if (key < 0 || static_cast<size_t>(key) >= A.size())
        return false;
    const auto &a = A[key];
    int ga;
    marker_world_pos(a.area, a.gridX, a.gridZ, a.posX, a.posZ, ga, wx, wz,
                     /*conv_underground=*/true);
    out_area = ga;
    return true;
}

// The map sub-page (tabId) of a grace anchor (by its GRACE_ANCHORS index = a marker's
// cluster_key). Underground pages split into sub-pages (12000/12001/12002, DLC 6800+),
// where Euclidean distance is meaningless — distance-adaptive uses this discrete tab
// gradient there (same sub-page as the player = detail). -1 on a bad key.
int goblin::grace_anchor_tab(int key)
{
    const auto &A = goblin::grace_anchors();
    if (key < 0 || static_cast<size_t>(key) >= A.size())
        return -1;
    return A[key].tab_id;
}

// Player MapId TILE -> map sub-page (tabId), via the authoritative tile_region_map
// table. Used for UNDERGROUND distance-adaptive: the player's local float is leaf-
// block-local garbage and the marker param gridXNo is coarse (1/2), so neither
// distance separates sub-regions — but the MapId tile is reliable and maps 1:1 to a
// tabId (Ainsel 12000 / Nokron+Siofra+Mohgwyn 12001 / Deeproot 12002 / DLC 6800..).
// Returns -1 if the tile isn't in the table (overworld tiles aren't — they use the
// Euclidean frame).
static int tab_for_tile(int area, int gx, int gz)
{
    for (size_t i = 0; i < goblin::generated::TILE_TAB_COUNT; i++)
    {
        const auto &t = goblin::generated::TILE_TABS[i];
        if (t.area == area && t.gx == gx && t.gz == gz) return t.tab;
    }
    return -1;
}

// Raw tile (gridX/gridZ) of a grace anchor by its GRACE_ANCHORS index (cluster_key).
// Underground distance-adaptive uses TILE distance (the float is garbage there, but
// the tile is reliable) within the player's sub-page. false on a bad key.
bool goblin::grace_anchor_tile(int key, int &out_gx, int &out_gz)
{
    const auto &A = goblin::grace_anchors();
    if (key < 0 || static_cast<size_t>(key) >= A.size())
        return false;
    const auto &a = A[key];
    out_gx = a.gridX;
    out_gz = a.gridZ;
    return true;
}

// The player's current map sub-page (tabId) from the RELIABLE MapId tile (gx,gz) —
// the float is leaf-block-local garbage underground, so coord/nearest-grace tab is
// unreliable there; this uses tab_for_tile like the native distance-adaptive path.
// Returns -1 on the overworld (those tiles aren't in the tab table) or if unresolved.
int goblin::player_map_tab()
{
    int area = -1, gx = -1, gz = -1;
    float wx = 0, wz = 0;
    if (!get_player_map_pos(area, wx, wz, &gx, &gz))
        return -1;
    return tab_for_tile(area, gx, gz);
}

// Nearest MSB region-volume PlaceName to a world point in the same area — a LABEL
// fallback for piles whose grace has no name (every DLC area-61 grace stores a tab
// id, not a PlaceName). Returns 0 if no named region in this area.
static int find_nearest_region_pname(uint8_t area, float wx, float wz)
{
    int best = -1;
    float bestd = 1e30f;
    for (size_t i = 0; i < goblin::generated::REGION_ANCHOR_COUNT; i++)
    {
        const auto &g = goblin::generated::REGION_ANCHORS[i];
        if (g.area != area) continue;
        float dx = g.wx - wx, dz = g.wz - wz;
        float d = dx * dx + dz * dz;
        if (d < bestd) { bestd = d; best = static_cast<int>(i); }
    }
    return best < 0 ? 0 : goblin::generated::REGION_ANCHORS[best].placename_id;
}

// Coarsest LABEL fallback: nearest WorldMapPlaceNameParam major-region anchor in the
// same area. Returns the anchor's FRESH label_id (its name string is injected into our
// PlaceName bank by setup_messages — the source textId lives in the region-banner FMG,
// unusable as a map-point label). Last resort after grace + named-grace + MSB-region.
static int find_nearest_major_region_pname(uint8_t area, float wx, float wz)
{
    // DLC underground caverns (40-43) have no region anchor of their own — they sit
    // beneath the Realm of Shadow (area 61), so borrow its name as the coarse label.
    uint8_t lookup_area = (area >= 40 && area <= 43) ? 61 : area;
    int best = -1;
    float bestd = 1e30f;
    for (size_t i = 0; i < goblin::generated::MAJOR_REGION_ANCHOR_COUNT; i++)
    {
        const auto &g = goblin::generated::MAJOR_REGION_ANCHORS[i];
        if (g.area != lookup_area) continue;
        float dx = (g.gx * 256.0f + g.px) - wx, dz = (g.gz * 256.0f + g.pz) - wz;
        float d = dx * dx + dz * dz;
        if (d < bestd) { bestd = d; best = static_cast<int>(i); }
    }
    return best < 0 ? 0 : goblin::generated::MAJOR_REGION_ANCHORS[best].label_id;
}

// A stable cluster key for a projected dungeon, derived from its overworld
// ENTRANCE (the conv base point) so one dungeon = one pile. Offset far above the
// grace-index space (0..~443) so the two key spaces never collide.
static int entrance_cluster_key(float ex, float ez)
{
    int qx = static_cast<int>(std::floor(ex / 8.0f));
    int qz = static_cast<int>(std::floor(ez / 8.0f));
    return 1000000 + ((qx & 0xFFFF) << 16) + (qz & 0xFFFF);
}

// Census handed to setup_messages so it can inject each cluster's count string:
// (PlaceName textId, member count).
static std::vector<std::pair<int, std::string>> g_cluster_census;

// Runtime registries (filled during inject build).
// member_flags = the collect-flags (textDisableFlagId1) of this cluster's
// flag-backed members; when ALL are set the pile is depleted → the overlay draws
// the pile glyph in its depleted/green style. Empty = no collectible members (a
// pure grace/boss pile) → never "done".
// Collapsed: clusters visible (real area), members parked (99).
// Expanded: clusters parked (99), members restored — the slow, see-everything view.

// Show / hide the on-map cluster bubbles. ON = the pile glyph is on its page with
// its count on line 2. OFF = the bubble is parked off-page (areaNo 99) entirely —
// no phantom numberless glyph; the per-category overlay census is the count source.
// Members stay parked either way (they only un-park in expanded view), so OFF just
// removes the pile icon, keeping the freeze-fix parking intact.

// Cluster label census for setup_messages (PlaceName textId → member count).
const std::vector<std::pair<int, std::string>> &goblin::cluster_label_census()
{
    return g_cluster_census;
}

// ─── Player world position (WorldChrMan) ─────────────────────────────────────
// WorldChrMan static: AOB `48 8B FA 0F 11 41 70 48 8B 05`; the trailing `48 8B 05`
// is `mov rax,[rip+disp32]` at +7 (ends +0xE) → static slot = finder + 0xE +
// *(int32*)(finder+0xA).
// Player position chain RESOLVED (runtime-confirmed, CE find-what-accesses + RPM walk —
// docs/re/windows_player_pos_RESOLVED_re_findings.md, commit cc53594):
//   LocalPlayer = [WorldChrMan + 0x1E508]
//   X/Y/Z       = float [LocalPlayer + 0x6C0 / +0x6C4 / +0x6C8]   (Y = height)
// Correct on EVERY page (underground included) and updates while the map is CLOSED —
// the single source for distance-adaptive (map open) AND the minimap (map closed).
// Supersedes the dead 0x10EF8 → +0x6B0 chain (LocalPlayer offset drifted, field moved).
static void **g_wcm_static = nullptr;
static bool g_wcm_tried = false;

static void resolve_world_chr_man()
{
    g_wcm_tried = true;
    // Doc-confirmed static slot (runtime RPM walk, windows_player_pos_RESOLVED):
    // WorldChrMan = [eldenring.exe + 0x3D65F88]. Prefer this exact RVA; keep the WCM_FINDER
    // AOB as a patch-drift fallback (on this build both resolve to the same slot).
    uintptr_t er_base = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    void **fixed = er_base ? reinterpret_cast<void **>(er_base + 0x3D65F88) : nullptr;
    void **aob = nullptr;
    if (auto *finder = reinterpret_cast<uint8_t *>(
            modutils::scan<void>({.aob = goblin::sig::WCM_FINDER})))
    {
        int32_t disp = *reinterpret_cast<int32_t *>(finder + 0xA);
        aob = reinterpret_cast<void **>(finder + 0xE + disp);
    }
    g_wcm_static = fixed ? fixed : aob;
    spdlog::info("[PLAYER] WorldChrMan slot: fixed(er+0x3D65F88)={:p} aob={:p} -> using {:p}",
                 (void *)fixed, (void *)aob, (void *)g_wcm_static);
}

// Player-position probe (POD-only; no C++ objects in the __try). Caller reads the
// result OUTSIDE the SEH frame. LocalPlayer = [WorldChrMan+0x1E508]; X/Y/Z =
// LocalPlayer +0x6C0/+0x6C4/+0x6C8 (a 2nd render copy sits at +0x6D4.. — use +0x6C0).
struct PlayerProbe
{
    void *wcm, *lp;   // [static], [wcm+0x1E508]
    float p[3];       // X, Y(height), Z at LocalPlayer +0x6C0/+0x6C4/+0x6C8
    bool ok;
};

// Raw derefs live in a noinline body: the caller's __try then wraps an opaque
// CALL, which clang-cl PRESERVES — __try directly around raw loads gets ELIDED
// by clang-cl (unguarded 0xC0000005; docs/memory/tooling/clang-cl-seh-noinline.md).
// pr->ok is written LAST, so a mid-body fault leaves ok=false.
__declspec(noinline) static void probe_player_body(void **wcm_static, PlayerProbe *pr)
{
    auto *wcm = *reinterpret_cast<uint8_t **>(wcm_static);
    pr->wcm = wcm;
    if (!wcm) return;
    auto *lp = *reinterpret_cast<uint8_t **>(wcm + 0x1E508);
    pr->lp = lp;
    if (!lp) return;
    pr->p[0] = *reinterpret_cast<float *>(lp + 0x6C0); // X (tile-local)
    pr->p[1] = *reinterpret_cast<float *>(lp + 0x6C4); // Y (height)
    pr->p[2] = *reinterpret_cast<float *>(lp + 0x6C8); // Z (tile-local)
    pr->ok = true;
}

static void probe_player_seh(void **wcm_static, PlayerProbe *pr)
{
    pr->wcm = pr->lp = nullptr;
    pr->ok = false;
    __try
    {
        probe_player_body(wcm_static, pr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { pr->ok = false; }
}

bool goblin::get_player_world_pos(float &x, float &y, float &z)
{
    if (!g_wcm_tried) resolve_world_chr_man();
    if (!g_wcm_static) return false;
    PlayerProbe pr{};
    probe_player_seh(g_wcm_static, &pr);
    if (!pr.ok) return false;
    x = pr.p[0]; y = pr.p[1]; z = pr.p[2];
    return true;
}

// ─── Player MARKER-space position — CONFIRMED Target-A chain (playerpos doc) ──
// Both statics are AOB-anchored (drift per patch, so never hardcode the RVAs):
//   player-MapId singleton (was 0x3d691d8): unique site that loads it then calls
//     the +0x2c MapId getter — `mov rcx,[rip]; lea rdx,[rsp+0x20]; call getter;
//     movsd xmm0,[rip]`. relative_offsets {{3,7}} → slot from the mov's rip-disp.
//   CSWorldGeomMan / map-pos mgr (was 0x3d69ba8): same slot goblin_collected
//     resolves; its +0x70/+0x74 hold the player block-local X/Z.
static uintptr_t g_mapid_slot = 0;     // &(player-MapId singleton ptr)
static uintptr_t g_mappos_mgr_slot = 0; // &(CSWorldGeomMan ptr)
static bool g_mappos_tried = false;

static void resolve_player_map_pos_statics()
{
    g_mappos_tried = true;
    g_mapid_slot = reinterpret_cast<uintptr_t>(modutils::scan<void>({
        .aob = goblin::sig::PLAYER_MAPID_SLOT,
        .relative_offsets = {{3, 7}}}));
    g_mappos_mgr_slot = reinterpret_cast<uintptr_t>(modutils::scan<void>({
        .aob = goblin::sig::WORLD_GEOM_MAN_SLOT,
        .relative_offsets = {{3, 7}}}));
    spdlog::info("[PLAYER] map-pos statics: mapId-slot {:p}, geomMgr-slot {:p}",
                 (void *)g_mapid_slot, (void *)g_mappos_mgr_slot);
}

struct MapPosProbe { int area, gx, gz; float lx, lz; bool ok; };
// Same noinline-body shape as probe_player_body (clang-cl SEH-elision guard).
__declspec(noinline) static void probe_map_pos_body(uintptr_t mapid_slot, uintptr_t mgr_slot,
                                                    MapPosProbe *pr)
{
    auto *singleton = *reinterpret_cast<uint8_t **>(mapid_slot);
    auto *mgr = *reinterpret_cast<uint8_t **>(mgr_slot);
    if (!singleton || !mgr) return;
    uint32_t mid = *reinterpret_cast<uint32_t *>(singleton + 0x2c);
    pr->area = (mid >> 24) & 0xff;
    pr->gx   = (mid >> 16) & 0xff;
    pr->gz   = (mid >> 8)  & 0xff;
    // Vec layout (RE FUN_14045e390, doc windows_yellowdot_player_pos): X@+0x70,
    // Y(HEIGHT)@+0x74, Z@+0x78. The old code read +0x74 as Z = HEIGHT → masked
    // overworld (flat, Y small), broken underground (deep, Y swings). Z is +0x78.
    pr->lx = *reinterpret_cast<float *>(mgr + 0x70);  // local X
    pr->lz = *reinterpret_cast<float *>(mgr + 0x78);  // local Z (NOT +0x74 = height)
    pr->ok = true;
}

static void probe_map_pos_seh(uintptr_t mapid_slot, uintptr_t mgr_slot, MapPosProbe *pr)
{
    pr->ok = false;
    __try
    {
        probe_map_pos_body(mapid_slot, mgr_slot, pr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { pr->ok = false; }
}

bool goblin::get_player_map_pos(int &out_area, float &world_x, float &world_z,
                                int *out_gx, int *out_gz, int *out_group)
{
    if (!g_mappos_tried) resolve_player_map_pos_statics();
    if (!g_wcm_tried) resolve_world_chr_man();
    if (!g_mapid_slot || !g_mappos_mgr_slot || !g_wcm_static) return false;
    MapPosProbe pr{};
    probe_map_pos_seh(g_mapid_slot, g_mappos_mgr_slot, &pr); // area/tile (+ stale geom local)
    PlayerProbe pp{};
    probe_player_seh(g_wcm_static, &pp);                     // live ChrIns world pos
    if (!pr.ok || !pp.ok) return false;
    // Player LOCAL = LocalPlayer+0x6C0/+0x6C8 — RUNTIME-CONFIRMED to be the EXACT
    // WorldMapPointParam posX/posZ frame on every page (RE windows_player_pos_RESOLVED §4:
    // standing on the Bois-des-Fidèles grace, +0x6C0/+0x6C8 = 1437.8/1519.0 == the grace's
    // baked posX/posZ). So feed it as posX/posZ and bridge + project EXACTLY like a marker.
    // The old bug: get_player_map_pos read the local from geomMgr+0x70/+0x74 (a physics-block
    // frame, off by the block origin underground) — geomLocal is NOT used now. ChrIns reads
    // (0,0) only during a load before the player is positioned → report no position then.
    if (pp.p[0] == 0.0f && pp.p[2] == 0.0f) return false;
    from::paramdef::WORLD_MAP_POINT_PARAM_ST tmp{};
    tmp.areaNo  = static_cast<uint8_t>(pr.area);
    tmp.gridXNo = static_cast<uint8_t>(pr.gx);
    tmp.gridZNo = static_cast<uint8_t>(pr.gz);
    tmp.posX = pp.p[0]; // LocalPlayer+0x6C0 (param posX frame)
    tmp.posZ = pp.p[2]; // LocalPlayer+0x6C8 (param posZ frame)
    // Project dungeons/underground onto the overworld map-space EXACTLY like markers
    // (conv_underground=true unifies base underground area 12 too) → matches every page.
    if (project_dungeon_row_to_overworld(&tmp, nullptr, nullptr, /*conv_underground=*/true))
    {
        out_area = tmp.areaNo;
        world_x = tmp.gridXNo * 256.0f + tmp.posX;
        world_z = tmp.gridZNo * 256.0f + tmp.posZ;
        if (out_gx) *out_gx = tmp.gridXNo;
        if (out_gz) *out_gz = tmp.gridZNo;
        if (out_group) *out_group = goblin::marker_group_from((uint8_t)pr.area, tmp.areaNo);
    }
    else
    {
        out_area = pr.area;
        world_x = pr.gx * 256.0f + pp.p[0];
        world_z = pr.gz * 256.0f + pp.p[2];
        if (out_gx) *out_gx = pr.gx;
        if (out_gz) *out_gz = pr.gz;
        if (out_group) *out_group = goblin::marker_group_from((uint8_t)pr.area, pr.area);
    }
    return true;
}

// Player position in the RAW per-area frame (NO projection): out_area = the real MapId
// area (60 overworld, 12 base underground, 61/40-43 DLC, …); wx/wz = gridX*256 + the live
// ChrIns local. Unlike get_player_map_pos (which projects underground into the OVERLAPPING
// unified overworld map-space), this keeps each area in its own frame — so distance-adaptive
// can measure player↔grace distance correctly underground (gate on same raw area). Returns
// false during a load (ChrIns 0) or when the statics/probe fail.
bool goblin::get_player_raw_pos(int &out_area, float &wx, float &wz)
{
    if (!g_mappos_tried) resolve_player_map_pos_statics();
    if (!g_wcm_tried) resolve_world_chr_man();
    if (!g_mapid_slot || !g_wcm_static) return false;
    MapPosProbe pr{};
    probe_map_pos_seh(g_mapid_slot, g_mappos_mgr_slot, &pr);
    PlayerProbe pp{};
    probe_player_seh(g_wcm_static, &pp);
    if (!pr.ok || !pp.ok) return false;
    if (pp.p[0] == 0.0f && pp.p[2] == 0.0f) return false;
    out_area = pr.area;
    wx = pr.gx * 256.0f + pp.p[0];
    wz = pr.gz * 256.0f + pp.p[2];
    return true;
}

// Grace anchor in its RAW per-area frame (NO projection) — for same-area distance vs
// get_player_raw_pos. area = GRACE_ANCHORS[key].area; wx/wz = gridX*256 + posX/posZ.
bool goblin::grace_anchor_raw(int key, int &out_area, float &wx, float &wz)
{
    const auto &A = goblin::grace_anchors();
    if (key < 0 || static_cast<size_t>(key) >= A.size())
        return false;
    const auto &a = A[key];
    out_area = a.area;
    wx = a.gridX * 256.0f + a.posX;
    wz = a.gridZ * 256.0f + a.posZ;
    return true;
}


// Region gating helpers for the overlay (mirror the game's native areaNo+tab gating).
int goblin::grace_tab_id(uint8_t src_area, float raw_wx, float raw_wz)
{
    int idx = -1, pname = -1, tab = -1;
    if (find_nearest_grace(src_area, raw_wx, raw_wz, &idx, &pname, &tab))
        return tab;
    return -1;
}

bool goblin::player_in_dlc()
{
    if (!g_mappos_tried) resolve_player_map_pos_statics();
    if (!g_mapid_slot || !g_mappos_mgr_slot) return false;
    MapPosProbe pr{};
    probe_map_pos_seh(g_mapid_slot, g_mappos_mgr_slot, &pr);
    if (!pr.ok) return false;
    if (pr.area >= 40 && pr.area <= 43) return true;       // DLC underground (native)
    int tab = tab_for_tile(pr.area, pr.gx, pr.gz);          // DLC overworld/under → 6800-6999
    return tab >= 6800 && tab <= 6999;
}

// Unified overworld marker-space coord for an arbitrary baked marker (overlay-
// rendered markers need this: a row's native (areaNo, grid, pos) is page-local,
// but the overworld view projects everything into area-60 space). Mirrors the
// player path above: project legacy dungeons (area 10/11/30-39…) onto the
// overworld via LEGACY_CONV, then world = grid*256 + local. Rows already on the
// overworld (area 60/61) or on their own underground page are returned as-is.
static std::vector<goblin::LiveGrace> g_live_graces;

void goblin::capture_live_graces()
{
    g_live_graces.clear();
    // Read graces LIVE from the engine's own BonfireWarpParam — no MASSEDIT, no baked
    // MAP_ENTRIES, no per-update drift. posX/Y/Z + areaNo/grid ARE the grace world position
    // (the param carries them; no MSB resolve needed). The map icon is per-grace: iconId 1 =
    // normal bonfire, iconId 44 = ERR cave/underground grace, iconId 48 = unique. Filters
    // mirror the old offline bake (generate_graces.py): a real reachable grace needs a
    // discovery flag + a place-name, and is not an ERR intentional hide (all dispMask = 0).
    int hidden = 0, ug = 0;
    // textId1 field offset, READ LIVE from the game's own text getter `mov reg,[base+0x30]` — a
    // GENERIC switch dispatcher reused across several param types (4 matching sites, all disp 0x30,
    // resolved by consensus). Read textId1 via this offset instead of the struct field so no offset
    // constant survives in the hot path. Pinned 0x30 = logged fallback if the AOB breaks.
    static const ptrdiff_t s_textid1_off = [] {
        auto r = modutils::resolve_field_offset({.aob = goblin::sig::BONFIRE_TEXTID1_ACCESS,
                                                 .disp_pos = 3, .disp_size = 1, .consensus = true});
        if (r)
        {
            spdlog::info("[FIELDOFF] BonfireWarpParam.textId1 = +0x{:x} (live from exe, consensus)", *r);
            return *r;
        }
        spdlog::warn("[FIELDOFF] BonfireWarp textId1 AOB unresolved — falling back to pinned +0x30");
        return static_cast<ptrdiff_t>(0x30);
    }();
    auto textid1_of = [](const from::paramdef::BONFIRE_WARP_PARAM_ST &rw) -> int32_t {
        return *reinterpret_cast<const int32_t *>(reinterpret_cast<const uint8_t *>(&rw) + s_textid1_off);
    };
    try
    {
        for (auto [rowId, row] :
             from::params::get_param<from::paramdef::BONFIRE_WARP_PARAM_ST>(L"BonfireWarpParam"))
        {
            if (row.areaNo == 0) continue;                 // not a placed grace
            if ((int)row.eventflagId <= 0 || textid1_of(row) <= 0) continue; // no flag / no name
            // dispMask00/01/02 are ALL bits 0/1/2 of the single byte 0x1e (verified in-memory:
            // dispMask00→0x01, dispMask01→0x02, dispMask02→0x04; byte 0x1f is pad). A grace shown
            // on NO map layer (all three 0) is an ERR intentional hide (spoiler graces). The
            // earlier (&0x3) read missed dispMask02 → wrongly hid every DLC grace.
            if ((row.dispMask0 & 0x7) == 0) { ++hidden; continue; }
            const bool underground = (row.iconId == 44);
            if (underground) ++ug;
            g_live_graces.push_back({ row.areaNo, row.gridXNo, row.gridZNo,
                                      row.posX, row.posY, row.posZ, textid1_of(row), rowId,
                                      (int)row.eventflagId, underground,
                                      row.bonfireSubCategoryId });
        }
    }
    catch (...) {}

    spdlog::info("[LIVE-GRACE] {} grace rows from live BonfireWarpParam ({} underground/cave, "
                 "{} ERR-hidden skipped)", g_live_graces.size(), ug, hidden);
}

const std::vector<goblin::LiveGrace> &goblin::live_graces() { return g_live_graces; }

// (Removed: marker_fogged + WorldMapPieceParam build_fog_pieces. That "fog" was the map-FRAGMENT
// REGION reveal (openEventFlagId = 62xxx), redundant with the require_map_fragments item gate.
// A per-tile walk-fog gate was prototyped and dropped: it's a non-issue in normal play (the fog
// clears as you acquire map fragments while exploring). RE kept in docs/re/windows_worldmap_tile_fog_re_findings.md.)

// Map-fragment discovery flag for a marker, computed on the SAME tile the native
// injection gates on. Legacy GetMapFragment runs AFTER inject_map_entries projected the
// row, so it looks the fragment up by the PROJECTED tile: overworld dungeons (Stormveil
// m10, catacombs m30…) become area-60 overworld tiles, while underground (m12 / DLC
// 40-43) stays native (MapList keys those separately) — exactly conv_underground=false.
// The first overlay port looked up the ORIGINAL baked tile instead, so a dungeon whose
// interior grid cell isn't enumerated in MapList (deep Stormveil, etc.) returned 0 =
// "no fragment" = always shown → the islands that leaked into the fog with zero
// fragments. Projecting first puts them on a MapList-covered overworld tile and gates
// them like the native map. (ExceptionList per-row overrides still not applied — no
// rowId here; the tile table covers the vast majority.)
int goblin::marker_fragment_flag(uint8_t areaNo, uint8_t gx, uint8_t gz, float px, float pz)
{
    from::paramdef::WORLD_MAP_POINT_PARAM_ST tmp{};
    tmp.areaNo = areaNo;
    tmp.gridXNo = gx;
    tmp.gridZNo = gz;
    tmp.posX = px;
    tmp.posZ = pz;
    project_dungeon_row_to_overworld(&tmp, nullptr, nullptr, /*conv_underground=*/false);
    return goblin::map_fragment_flag(tmp.areaNo, tmp.gridXNo, tmp.gridZNo);
}

bool goblin::marker_world_pos(uint8_t areaNo, uint8_t gx, uint8_t gz, float px, float pz,
                              int &out_area, float &world_x, float &world_z,
                              bool conv_underground)
{
    from::paramdef::WORLD_MAP_POINT_PARAM_ST tmp{};
    tmp.areaNo = areaNo;
    tmp.gridXNo = gx;
    tmp.gridZNo = gz;
    tmp.posX = px;
    tmp.posZ = pz;
    // in-place; no-op if already overworld / unmappable. conv_underground=true also unifies
    // base underground (area 12) into overworld map-space (overlay UG layer).
    project_dungeon_row_to_overworld(&tmp, nullptr, nullptr, conv_underground);
    out_area = tmp.areaNo;
    world_x = tmp.gridXNo * 256.0f + tmp.posX;
    world_z = tmp.gridZNo * 256.0f + tmp.posZ;
    return true;
}
