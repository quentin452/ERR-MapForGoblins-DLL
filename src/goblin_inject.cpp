#include "goblin_inject.hpp"
#include "goblin_inject_shared.hpp"
#include "goblin_collected.hpp"
#include "goblin_kindling.hpp"
#include "goblin_config.hpp"
#include "goblin_messages.hpp"
#include "modutils.hpp"
#include "re_signatures.hpp"
#include "goblin_map_data.hpp"
#include "goblin_category_exceptions.hpp"
#include "goblin_region_anchors.hpp"
#include "goblin_major_regions.hpp"
#include "goblin_name_regions.hpp"
#include "goblin_tile_tabs.hpp"
#include "goblin_legacy_conv.hpp"
#include "goblin_legacy_fold.hpp"
#include "goblin_logic.hpp"
#include "goblin_markers.hpp"
#include "goblin_bench.hpp"
#include "from/params.hpp"
#include "from/paramdef/WORLD_MAP_POINT_PARAM_ST.hpp"
#include "from/paramdef/WORLD_MAP_PIECE_PARAM_ST.hpp"
#include "from/paramdef/BONFIRE_WARP_PARAM_ST.hpp"
#include "from/paramdef/BONFIRE_WARP_SUB_CATEGORY_PARAM_ST.hpp"
#include "goblin_quest_steps.hpp"
#include "goblin_logic.hpp"
#include "worldmap/loot_disk.hpp"  // read_game_file_decompressed (no-bake item-icon layout source)

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>
#include <deque>
#include <cctype>
#include <chrono>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <spdlog/spdlog.h>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <Xinput.h>
#pragma comment(lib, "Xinput.lib")

using ParamRowInfo = from::params::ParamRowInfo;
using ParamTable = from::params::ParamTable;
using ParamResCap = from::params::ParamResCap;
using Category = goblin::generated::Category;

static void *allocation = nullptr;

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


// Data pointers of MFG-injected WorldMapPointParam rows in the expanded table.
// Used by sanitize_injected_textids() (run after the FMG is built) to strip
// textIds that don't resolve to a real string.
static std::vector<uint8_t *> g_injected_row_ptrs;

static std::set<uint8_t *> g_lot_backed_set;


// ─── Per-section runtime visibility (in-game family-group toggle) ─────
//
// The 7 INI display groups (mirrors goblin_config_schema's sections). A row's
// family flag (show_*) decides whether it is injected at all; the SECTION gate
// decides runtime visibility of the rows that DID get injected. The gate is
// applied purely by flipping the row's areaNo to 99 (the same eviction trick
// pieces use) on the live expanded blob — no param rebuild, no pointer swap.
enum class Section : uint8_t
{
    Equipment, KeyItems, Loot, Magic, Quest, Reforged, World, COUNT
};
static constexpr int SECTION_COUNT = static_cast<int>(Section::COUNT);

static const char *section_name(Section s)
{
    switch (s)
    {
    case Section::Equipment: return "Equipment";
    case Section::KeyItems:  return "Key Items";
    case Section::Loot:      return "Loot";
    case Section::Magic:     return "Magic";
    case Section::Quest:     return "Quest";
    case Section::Reforged:  return "Reforged";
    case Section::World:     return "World";
    default:                 return "?";
    }
}

// Category -> display section. Mirrors the [section] grouping in
// goblin_config_schema.cpp::build_schema() exactly (note: HostileNPC / QuestNPC
// live under [World] in the schema, not [Quest]). Every Category is covered.
static Section section_of(Category c)
{
    switch (c)
    {
    case Category::EquipArmaments:
    case Category::EquipArmour:
    case Category::EquipAshesOfWar:
    case Category::EquipSpirits:
    case Category::EquipTalismans:
        return Section::Equipment;
    case Category::KeyCelestialDew:
    case Category::KeyCookbooks:
    case Category::KeyCrystalTears:
    case Category::KeyImbuedSwordKeys:
    case Category::KeyLarvalTears:
    case Category::KeyScadutreeFragments:
    case Category::KeyGreatRunes:
    case Category::KeyLostAshes:
    case Category::KeyPotsNPerfumes:
    case Category::KeySeedsTears:
    case Category::KeyWhetblades:
        return Section::KeyItems;
    case Category::LootAmmo:
    case Category::LootBellBearings:
    case Category::LootConsumables:
    case Category::LootCraftingMaterials:
    case Category::LootMPFingers:
    case Category::LootMaterialNodes:
    case Category::LootMerchantBellBearings:
    case Category::LootReusables:
    case Category::LootSmithingStones:
    case Category::LootSmithingStonesLow:
    case Category::LootSmithingStonesRare:
    case Category::LootGoldenRunes:
    case Category::LootGoldenRunesLow:
    case Category::LootStoneswordKeys:
    case Category::LootThrowables:
    case Category::LootPrattlingPates:
    case Category::LootRuneArcs:
    case Category::LootDragonHearts:
    case Category::LootGloveworts:
    case Category::LootGreatGloveworts:
    case Category::LootRadaFruit:
    case Category::LootGestures:
    case Category::LootGreases:
    case Category::LootUtilities:
    case Category::LootStatBoosts:
        return Section::Loot;
    case Category::MagicIncantations:
    case Category::MagicMemoryStones:
    case Category::MagicPrayerbooks:
    case Category::MagicSorceries:
        return Section::Magic;
    case Category::QuestDeathroot:
    case Category::QuestProgression:
    case Category::QuestSeedbedCurses:
        return Section::Quest;
    case Category::ReforgedFortunes:
    case Category::ReforgedEmberPieces:
    case Category::ReforgedItemsAndChanges:
    case Category::ReforgedRunePieces:
        return Section::Reforged;
    case Category::WorldHostileNPC:
    case Category::WorldQuestNPC:
    case Category::WorldBosses:
    case Category::WorldGraces:
    case Category::WorldImpStatues:
    case Category::WorldMaps:
    case Category::WorldPaintings:
    case Category::WorldSpiritSprings:
    case Category::WorldSpiritspringHawks:
    case Category::WorldStakesOfMarika:
    case Category::WorldSummoningPools:
    case Category::WorldKindlingSpirits:
    case Category::WorldInteractables:
        return Section::World;
    }
    return Section::World;  // unreachable; keeps the compiler happy
}

// Runtime gate per section. Seeded from config at inject time; flipped live by
// the section hotkey. Atomic: written by the hotkey thread, read here.
static std::atomic<bool> g_section_visible[SECTION_COUNT];

// Overlay-menu → watcher-thread inboxes. The menu only records intent;
// the watcher (menu_auto_toggle_loop, sole owner of game-state mutation) applies
// the areaNo flips, persists the ini, and fires the toast — mirroring how the
// master F10 toggle defers its banner to that thread.
static std::atomic<int> g_section_apply_req{-1};  // section idx to (re)apply, -1 = none

// Toast request QUEUE. Replaces the old single-slot request so several toasts
// (e.g. a burst of coverage-gap hits) can be shown in sequence instead of
// overwriting each other. The watcher (menu_auto_toggle_loop) drains it one at
// a time, spaced TOAST_SPACING_MS apart so each stays on screen.
static std::mutex g_toast_mtx;
static std::deque<int> g_toast_queue;        // tutorial ids waiting to fire
static int64_t g_next_toast_ms = 0;          // earliest time the next may fire
static constexpr size_t TOAST_QUEUE_CAP = 32;
static constexpr int64_t TOAST_SPACING_MS = 2500;

// One injected, section-toggleable row in the expanded blob. orig_area is the
// row's FINAL areaNo (post dungeon-reprojection) so a show restores the right
// page. Piece/kindling rows may be independently 99-hidden when collected — a
// section "show" must not resurrect those.

// Number of marker categories (enum has no COUNT sentinel; keep in sync).
static constexpr int NUM_CATEGORIES = static_cast<int>(Category::WorldInteractables) + 1;
static std::atomic<bool> g_category_visible[NUM_CATEGORIES];
static std::atomic<bool> g_category_dirty[NUM_CATEGORIES];  // set by menu, applied by watcher
// Per-category cluster opt-in (true = this category folds into clusters). Seeded
// from config::clusterExclude at init; toggled live by the menu but only takes
// effect after Save + restart, since clustering is planned once at inject.
static std::atomic<bool> g_category_cluster[NUM_CATEGORIES];
// Per-category cluster threshold for menu display (effective value: override or
// the global default). Seeded at init; edits persist to clusterThresholdOverrides
// on Save and take effect after restart.
static std::atomic<int> g_category_threshold[NUM_CATEGORIES];
// Per-category uncollected census (refresh_category_census fills these on the
// watcher thread; the overlay reads them). g_cat_total = collectible rows in the
// category; g_cat_remaining = uncollected, or -1 = no collectible rows (no badge).
// g_menu_visible_ns = steady_clock nanoseconds of the last overlay-panel frame;
// the census skips its flag sweep unless this is within the last 2s.
static std::atomic<int> g_cat_total[NUM_CATEGORIES];
static std::atomic<int> g_cat_remaining[NUM_CATEGORIES];
static std::atomic<long long> g_menu_visible_ns{0};

// Coordination for the multiple areaNo owners. Section visibility, fragment-
// eviction and collected-eviction all write WORLD_MAP_POINT_PARAM_ST.areaNo
// (offset 0x20) independently. Without coordination, a restore-to-orig path
// (e.g. fragment-eviction's cold-API safety, or collected's restore-all)
// un-hides rows the user hid via a section toggle. This set holds the data
// pointers a section currently hides; every restore path must keep such a row
// at 99. Guarded by a mutex (apply runs on the watcher thread, restores on the
// refresh-loop thread).
static std::mutex g_section_hidden_mtx;
static std::set<uint8_t *> g_section_hidden_ptrs;    // hidden by a section toggle
static std::set<uint8_t *> g_category_hidden_ptrs;   // hidden by a category toggle
static std::atomic<bool> g_master_off{false};        // master "Show icons" off (hides ALL)

// Cluster expand/collapse state (false = collapsed = clusters shown, members
// parked). Declared here (ahead of its sibling cluster statics) so the eviction
// coordination in is_section_hidden_ptr can read it.
static std::atomic<bool> g_clusters_expanded{false};
// Cluster-member row pointers (built once at registration, read-only after). When
// the view is COLLAPSED these are parked under their cluster icon; the other areaNo
// owners (fragment/collected/royal eviction) must treat them as hidden so their
// restore paths don't un-park a clustered member (the bug that showed clusters AND
// their members at once). Built-once → no lock needed for the read.
static std::set<uint8_t *> g_cluster_member_ptrs;

// Spare pool of cluster rows reserved at inject (the param table can't realloc at
// runtime). replan_clusters() fills/empties them from the LIVE rows, so enable /
// soft-hard / threshold / exclude all apply with NO restart (the map rebuild shows
// it on next open). g_cluster_member_ptrs + the pool are mutated by replan under
// g_section_hidden_mtx; is_section_hidden_ptr reads the member set under the lock.
static constexpr size_t CLUSTER_POOL_SIZE = 1024;
static constexpr int CLUSTER_MAX_COUNT = 1999;  // pre-injected number strings 1..this
static std::vector<uint8_t *> g_cluster_pool;
static std::atomic<bool> g_cluster_replan_dirty{false};

bool goblin::is_section_hidden_ptr(const void *param_data)
{
    if (g_master_off.load()) return true;  // master off hides every injected row
    auto *p = reinterpret_cast<uint8_t *>(const_cast<void *>(param_data));
    std::lock_guard<std::mutex> lk(g_section_hidden_mtx);
    // Collapsed cluster member → kept parked under its cluster, regardless of any
    // other owner's restore.
    if (!g_clusters_expanded.load() && g_cluster_member_ptrs.count(p) != 0)
        return true;
    return g_section_hidden_ptrs.count(p) != 0 || g_category_hidden_ptrs.count(p) != 0;
}

// Show/hide every injected row of one section in place. Hide = areaNo 99;
// show = restore orig_area unless the row is an already-collected piece/kindling
// (those stay evicted, owned by collected::/kindling::).

// g_clusters_expanded is declared earlier (near is_section_hidden_ptr).
// Cluster bubbles ON by default: the checkbox reads this value directly while the
// MAP state is only synced at plan-time (replan reads this) / on toggle — so the
// default MUST match what plan-time publishes, else the checkbox and the map
// disagree until the user toggles. true => bubble shown WITH its count (no phantom).
static std::atomic<bool> g_cluster_debug{true};       // on-map cluster bubbles + count shown by default; uncheck in the menu (off = counts only in the overlay census)
// Hotkey thread sets these; the watcher applies the areaNo/textId flips (single
// owner of game-state mutation), mirroring the section + master toggles.
static std::atomic<bool> g_cluster_expand_dirty{false};
static std::atomic<bool> g_cluster_debug_dirty{false};

// A cluster icon (and its parked members) belong to ONE category since buckets
// are per-category. Hide them when that category OR its section is toggled off,
// so a disabled category doesn't leave its cluster glyphs on the map.




// ─── Marker clustering (v1, density-triggered, static) ───────────────
//
// Dense piles of markers (e.g. Leyndell's ~450) are collapsed into one cluster
// icon to cut the per-page map-open cost (which scales with rows on the page).
// Membership is decided ONCE at inject (static), so each cluster's count is
// known → we can show it as a label. Collapse/expand + count/icon-only are live
// areaNo-99 / textId flips on the already-built blob — no rebuild.

// World-unit size of a clustering cell. Markers within the same cell (and area)
// merge once a cell exceeds the threshold. Tunable here for v1 (not in the ini).
static constexpr float CLUSTER_CELL = 60.0f;
// Cluster label PlaceName id base (one static "<count>" string per cluster,
// injected by setup_messages). Above the 950M BloodMsg band, clear of item
// (50-600M), location (<50M) and npc (700M+) id spaces.
static constexpr int CLUSTER_TEXTID_BASE = 952000000;          // + count → "<n>"
static constexpr int CLUSTER_CATNAME_TEXTID_BASE = 952010000;  // + category → its name

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

static void probe_player_seh(void **wcm_static, PlayerProbe *pr)
{
    pr->wcm = pr->lp = nullptr;
    pr->ok = false;
    __try
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
    __except (EXCEPTION_EXECUTE_HANDLER) {}
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
static void probe_map_pos_seh(uintptr_t mapid_slot, uintptr_t mgr_slot, MapPosProbe *pr)
{
    pr->ok = false;
    __try
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
    __except (EXCEPTION_EXECUTE_HANDLER) {}
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

// Master-off intent set by the toggle hotkey. When true the user has
// explicitly hidden the icons, so the auto-toggle must keep the table vanilla
// even while the world map is open. Shared between the hotkey and watcher
// threads; a lone bool flag is fine, but use atomic for correctness.
static std::atomic<bool> g_icons_user_disabled{false};

// EXPERIMENT: toast-method cycler. F10 fires a toast with the current method;
// F11 cycles the method. Lets us A/B every text-injectable notification path
// in-game. Remove once the final style is chosen.
// Default = method 1 (trampoline). User confirmed this is the codex-style
// upper-left plaque we want. Methods 0/2/3 retained for A/B testing via F11.
static std::atomic<int> g_toast_method{1};
static const char *const TOAST_METHOD_NAMES[] = {
    "0 Summon p=1 fp=1 u=1 (narrow plaque just below center, ~5s)",
    "1 ShowTutorialPopup trampoline (AOB-resolved; codex upper-left, default)",
};
// TutorialParam row ids exposed in goblin_inject.hpp. These are NEW rows
// injected by inject_tutorial_popup_rows() with textId pointing at
// TutorialBody.fmg entries injected by goblin_messages — so the upper-left
// codex toast renders our text without modifying any vanilla/ERR data.
static constexpr int TOAST_METHOD_COUNT =
    (int)(sizeof(TOAST_METHOD_NAMES) / sizeof(TOAST_METHOD_NAMES[0]));

struct WrapperRowLocator
{
    int32_t row;
    int32_t index;
};

static ParamResCap *find_world_map_point_param_res_cap()
{
    auto param_list = *from::params::param_list_address;
    if (!param_list) return nullptr;
    for (int i = 0; i < 186; i++)
    {
        auto prc = param_list->entries[i].param_res_cap;
        if (!prc) continue;
        std::wstring_view name = from::params::dlw_c_str(&prc->param_name);
        if (name == L"WorldMapPointParam") return prc;
    }
    return nullptr;
}

// Readiness probe for the robust init wait: true once the WorldMapPointParam table
// is registered (regulation loaded). Polling THIS instead of a fixed sleep makes
// init slow-PC-safe (the param can take >5s to load). SEH-guarded — the param list
// is walked during volatile game init, so a mid-load fault just reads "not ready".
bool goblin::world_map_param_ready()
{
    // Not just "registered" — the ResCap appears almost instantly (the probe logged
    // "ready after 0 ms") while the regulation FILE + rows load later. Walk to the
    // param table and require num_rows > 0, else inject runs on an empty/half-loaded
    // table and the map fails to load on the slow launches. Same chain inject uses.
    __try
    {
        auto *prc = find_world_map_point_param_res_cap();
        if (!prc)
            return false;
        auto *rescap = reinterpret_cast<uint8_t *>(prc->param_header);
        if (!rescap)
            return false;
        auto *file_ptr = *reinterpret_cast<uint8_t **>(rescap + 0x80);
        if (!file_ptr)
            return false;
        auto *table = reinterpret_cast<ParamTable *>(file_ptr);
        return table->num_rows > 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Lowercase + keep only alphanumerics, so "Loot - Smithing Stones (Low)" and a
// user-typed "LootSmithingStonesLow" / "smithing stones low" all normalize alike.
static std::string norm_alnum(const std::string &s)
{
    std::string o;
    for (unsigned char c : s)
        if (std::isalnum(c))
            o += static_cast<char>(std::tolower(c));
    return o;
}

// True if `cat`'s name matches a comma-separated token in `list` (loose substring
// match, alnum-normalised). Shared by show_all_except and cluster_exclude.
static bool category_in_list(const std::string &list, Category cat)
{
    if (list.empty())
        return false;
    std::string catn = norm_alnum(goblin::markers::category_name(cat));
    if (catn.empty())
        return false;
    for (size_t i = 0; i < list.size();)
    {
        size_t j = list.find(',', i);
        if (j == std::string::npos)
            j = list.size();
        std::string tok = norm_alnum(list.substr(i, j - i));
        if (!tok.empty() && catn.find(tok) != std::string::npos)
            return true;
        i = j + 1;
    }
    return false;
}

// True if `cat` matches a token in show_all_except (loose substring match).
static bool category_in_except(Category cat)
{
    return category_in_list(goblin::config::showAllExcept, cat);
}

// True if `cat` is allowed to fold into a cluster (i.e. NOT opted out via
// cluster_exclude). Read at inject time when the cluster plan is built.
static bool category_clustered_cfg(Category cat)
{
    return !category_in_list(goblin::config::clusterExclude, cat);
}

// Cluster threshold for `cat`: a per-category override from
// cluster_threshold_overrides ("Name:N,Name2:M", loose name match) if present,
// else the global clusterThreshold. Read at inject when the plan is built.
static int cluster_threshold_for_cfg(Category cat)
{
    int def = goblin::config::clusterThreshold;
    const std::string &ov = goblin::config::clusterThresholdOverrides;
    if (ov.empty()) return def;
    std::string catn = norm_alnum(goblin::markers::category_name(cat));
    if (catn.empty()) return def;
    for (size_t i = 0; i < ov.size();)
    {
        size_t j = ov.find(',', i);
        if (j == std::string::npos) j = ov.size();
        std::string tok = ov.substr(i, j - i);
        size_t c = tok.find(':');
        if (c != std::string::npos)
        {
            std::string nm = norm_alnum(tok.substr(0, c));
            int v = std::atoi(tok.substr(c + 1).c_str());
            // Exact name match (not substring) so "SmithingStones:4" does NOT bleed
            // into SmithingStonesLow/Rare; the overlay writes full category names.
            if (!nm.empty() && v >= 0 && catn == nm)
                return v;
        }
        i = j + 1;
    }
    return def;
}

// Runtime (re)planner: compute the ENTIRE cluster plan from the live resident
// rows (g_section_rows) into the reserved pool. Called once after inject and on
// any enable / soft-hard / threshold / exclude change — NO restart (the map shows
// it on next open). Counts are live: each pile points its label at a pre-injected
// number string (CLUSTER_TEXTID_BASE + count). Runs on the watcher thread only.
static bool *category_config_ptr(Category cat)
{
    return &goblin::config::showCategory[static_cast<int>(cat)];
}

static bool is_category_enabled(Category cat)
{
    if (goblin::config::showAll)
        return !category_in_except(cat);
    bool *p = category_config_ptr(cat);
    return p ? *p : true;
}

// Seed the runtime gate atomics from config: per-category visibility + cluster opt-in
// + threshold, the master on/off, and the cluster collapsed/expanded state. This is
// the pure config→state step (no native-row park / no cluster replan), so it can run
// in BOTH modes — crucially when native_map_injection is OFF and inject_map_entries()
// (which used to be the only seeder) is skipped, the ImGui overlay still needs these
// gates seeded so its per-category visibility + clustering work.
void goblin::seed_runtime_gates()
{
    const bool sec_cfg[SECTION_COUNT] = {
        goblin::config::sectionEquipment, goblin::config::sectionKeyItems,
        goblin::config::sectionLoot,      goblin::config::sectionMagic,
        goblin::config::sectionQuest,     goblin::config::sectionReforged,
        goblin::config::sectionWorld,
    };
    for (int s = 0; s < SECTION_COUNT; s++)
        g_section_visible[s].store(sec_cfg[s]);
    for (int c = 0; c < NUM_CATEGORIES; c++)
    {
        g_category_visible[c].store(is_category_enabled(static_cast<Category>(c)));
        g_category_cluster[c].store(category_clustered_cfg(static_cast<Category>(c)));
        g_category_threshold[c].store(cluster_threshold_for_cfg(static_cast<Category>(c)));
    }
    g_icons_user_disabled.store(goblin::config::iconsHidden);
    g_clusters_expanded.store(!goblin::config::enableClustering);
}

// ─── TutorialParam row injection ─────────────────────────────────────
//
// Adds two new rows for the F10 banner: one displays "Map icons: ON", the
// other "Map icons: OFF". Each row is copied from an existing codex row
// (4167000 — guaranteed to exist with menuType=0 / triggerType=0 / repeatType=1
// from ERR's codex data) and then patched so its textId points at our newly
// injected TutorialBody.fmg entries.
//
// Per ERR TutorialParam.xml paramdef (TUTORIAL_PARAM_ST):
//   offset 4  u8 menuType                (0 = upper-left toast widget)
//   offset 5  u8 triggerType
//   offset 6  u8 repeatType
//   offset 16 (0x10) s32 textId          ← FMG id we point at our entries
//   offset 12 u32 unlockEventFlagId      ← cleared, no gate
//   offset 20 (0x14) f32 dispMinTime
//   offset 24 (0x18) f32 dispTime

// (Kept for reference but unused now — see hijack_tutorial_param_textids()
// below for the simpler in-place approach we ship.)
static constexpr int TUTORIAL_TEMPLATE_ROW_ID = 4167000;
static constexpr int TUTORIAL_NEW_ROW_ID_ON        = goblin::TUTORIAL_FMG_ID_ON;
static constexpr int TUTORIAL_NEW_ROW_ID_OFF       = goblin::TUTORIAL_FMG_ID_OFF;
static constexpr int TUTORIAL_NEW_ROW_ID_DUMP_OK   = goblin::TUTORIAL_FMG_ID_DUMP_OK;
static constexpr int TUTORIAL_NEW_ROW_ID_DUMP_FAIL = goblin::TUTORIAL_FMG_ID_DUMP_FAIL;

static ParamResCap *find_param_res_cap_by_name(const wchar_t *target)
{
    auto param_list = *from::params::param_list_address;
    if (!param_list) return nullptr;
    for (int i = 0; i < 186; i++)
    {
        auto prc = param_list->entries[i].param_res_cap;
        if (!prc) continue;
        std::wstring_view name = from::params::dlw_c_str(&prc->param_name);
        if (name == target) return prc;
    }
    return nullptr;
}

bool goblin::inject_tutorial_popup_rows()
{
    auto prc = find_param_res_cap_by_name(L"TutorialParam");
    if (!prc)
    {
        spdlog::warn("[TOAST] TutorialParam not found — F10 banner falls back to Summon");
        return false;
    }
    auto *rescap = reinterpret_cast<uint8_t *>(prc->param_header);
    auto *&file_ptr = *reinterpret_cast<uint8_t **>(rescap + 0x80);
    auto &file_size = *reinterpret_cast<int64_t *>(rescap + 0x78);

    auto *old_file = file_ptr;
    auto *old_table = reinterpret_cast<ParamTable *>(old_file);
    uint16_t orig_rows = old_table->num_rows;
    if (orig_rows < 2)
    {
        spdlog::warn("[TOAST] TutorialParam has only {} rows", orig_rows);
        return false;
    }

    // Row data size from TUTORIAL_PARAM_ST paramdef: 1+3 reserve, menuType,
    // triggerType, repeatType, pad1, imageId(u16), pad2(2), unlockEventFlagId
    // (u32), textId(s32), displayMinTime(f32), displayTime(f32), pad3(4) = 32B.
    constexpr int64_t TUTORIAL_ROW_DATA_SIZE = 32;
    int64_t row_data_size = TUTORIAL_ROW_DATA_SIZE;

    // Sanity: the in-memory stride between rows must match the paramdef size.
    int64_t derived_stride = (int64_t)old_table->rows[1].param_offset -
                             (int64_t)old_table->rows[0].param_offset;
    if (derived_stride != row_data_size)
    {
        spdlog::warn("[TOAST] TutorialParam stride {} != paramdef {} — re-laying contiguously",
                     derived_stride, row_data_size);
    }

    // Find a template row. Preferred: ERR codex row 4167000 (menuType=0,
    // repeatType=1). Vanilla has no such row, so fall back to any row with
    // menuType==0 (vanilla ships 13 of those — the toast widget is a vanilla
    // mechanism), and as a last resort synthesize the 32-byte row locally.
    // Every field we depend on is patched explicitly below anyway.
    uint8_t synth_row[TUTORIAL_ROW_DATA_SIZE] = {};
    const uint8_t *template_data = nullptr;
    for (uint16_t i = 0; i < orig_rows; i++)
    {
        if ((int)old_table->rows[i].row_id == TUTORIAL_TEMPLATE_ROW_ID)
        {
            template_data = old_file + old_table->rows[i].param_offset;
            break;
        }
    }
    if (!template_data)
    {
        for (uint16_t i = 0; i < orig_rows; i++)
        {
            const uint8_t *row = old_file + old_table->rows[i].param_offset;
            if (row[4] == 0)  // menuType == 0 (toast)
            {
                template_data = row;
                spdlog::info("[TOAST] template row {} absent (vanilla?) — using row {} (menuType=0)",
                             TUTORIAL_TEMPLATE_ROW_ID, (int)old_table->rows[i].row_id);
                break;
            }
        }
    }
    if (!template_data)
    {
        // Synthesized toast row: menuType=0, triggerType=0, repeatType set
        // below, no image, dispMinTime=1s, dispTime=3s (vanilla toast values).
        *reinterpret_cast<float *>(synth_row + 0x14) = 1.0f;
        *reinterpret_cast<float *>(synth_row + 0x18) = 3.0f;
        template_data = synth_row;
        spdlog::info("[TOAST] no menuType=0 row found — synthesizing toast template");
    }

    constexpr size_t WRAPPER_HEADER = 0x10;
    constexpr size_t HEADER_SIZE = 0x40;
    constexpr size_t ROW_LOCATOR_SIZE = sizeof(ParamRowInfo);
    constexpr size_t WRAPPER_ROW_LOC_SIZE = sizeof(WrapperRowLocator);

    const char *type_str = reinterpret_cast<const char *>(old_file + old_table->param_type_offset);
    size_t type_str_len = strlen(type_str) + 1;

    // Must match EXACTLY the rows pushed into all_rows below, or the locator/
    // data/wrapper arrays are under-sized and the write loop overflows the
    // HeapAlloc'd buffer (heap corruption → ntdll AV at init). Rows:
    //   4 fixed (ON/OFF/DUMP_OK/DUMP_FAIL) + 1 coverage-gap + GAP_CAT_COUNT
    //   per-category gap toasts.
    uint32_t new_row_count = 4 + 1 + goblin::GAP_CAT_COUNT;
    uint32_t total_rows = orig_rows + new_row_count;

    size_t row_locators_start = HEADER_SIZE;
    size_t data_start = row_locators_start + total_rows * ROW_LOCATOR_SIZE;
    size_t data_end = data_start + total_rows * (size_t)row_data_size;
    size_t type_str_start = data_end;
    size_t after_type_str = type_str_start + type_str_len;
    // CRITICAL: align wrapper_row_loc to 16, NOT 4. The lookup-by-id engine
    // (LookupTutorialParam @ eldenring.exe+0xD51BA0, pre-2026-05-29 RVA) reads this offset from the
    // wrapper header and rounds it UP to 16 via `(x + 0xf) & ~0xf` before using
    // it as the wrapper_row_locator base for its binary search. If our actual
    // array sits at a merely-4-aligned offset, the engine reads 4-12 bytes
    // past it → garbage row ids → out-of-range index → OOB row-data read →
    // crash on save-load (which does an id lookup). WMP got away with 4-align
    // because it's only ever iterated, never id-looked-up.
    size_t wrapper_row_loc_start = (after_type_str + 0xf) & ~(size_t)0xf;
    size_t wrapper_row_loc_end = wrapper_row_loc_start + total_rows * WRAPPER_ROW_LOC_SIZE;
    size_t param_file_size = wrapper_row_loc_end;
    size_t total_alloc = WRAPPER_HEADER + param_file_size;

    auto *allocation = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total_alloc);
    if (!allocation)
    {
        spdlog::error("[TOAST] HeapAlloc failed ({} bytes) for TutorialParam expansion", total_alloc);
        return false;
    }

    auto *new_wrapper = reinterpret_cast<uint8_t *>(allocation);
    auto *new_file = new_wrapper + WRAPPER_HEADER;
    auto *new_table = reinterpret_cast<ParamTable *>(new_file);

    *reinterpret_cast<uint32_t *>(new_wrapper + 0x00) = (uint32_t)wrapper_row_loc_start;
    *reinterpret_cast<int32_t *>(new_wrapper + 0x04) = (int32_t)total_rows;

    memcpy(new_file, old_file, HEADER_SIZE);
    new_table->num_rows = (uint16_t)total_rows;
    new_table->param_type_offset = type_str_start;
    *reinterpret_cast<uint32_t *>(new_file + 0x00) = (uint32_t)type_str_start;
    // Offset 0x04 (ushortDataOffset) left as memcpy'd from original (0): the
    // new ER param format uses the u64 dataOffset @0x30 as canonical source.
    *reinterpret_cast<uint64_t *>(new_file + 0x30) = data_start;

    memcpy(new_file + type_str_start, type_str, type_str_len);

    struct RowSource
    {
        int32_t row_id;
        const uint8_t *data_ptr;
    };
    std::vector<RowSource> all_rows;
    all_rows.reserve(total_rows);
    for (uint16_t i = 0; i < orig_rows; i++)
    {
        auto *data = old_file + old_table->rows[i].param_offset;
        all_rows.push_back({(int32_t)old_table->rows[i].row_id, data});
    }
    all_rows.push_back({TUTORIAL_NEW_ROW_ID_ON,        template_data});
    all_rows.push_back({TUTORIAL_NEW_ROW_ID_OFF,       template_data});
    all_rows.push_back({TUTORIAL_NEW_ROW_ID_DUMP_OK,   template_data});
    all_rows.push_back({TUTORIAL_NEW_ROW_ID_DUMP_FAIL, template_data});
    all_rows.push_back({goblin::TUTORIAL_FMG_ID_COVERAGE_GAP, template_data});
    for (int c = 0; c < goblin::GAP_CAT_COUNT; c++)
        all_rows.push_back({goblin::gap_cat_toast_id(c), template_data});

    std::sort(all_rows.begin(), all_rows.end(),
              [](const RowSource &a, const RowSource &b) { return a.row_id < b.row_id; });

    auto *new_locators = reinterpret_cast<ParamRowInfo *>(new_file + row_locators_start);
    auto *new_wrapper_locs = reinterpret_cast<WrapperRowLocator *>(new_file + wrapper_row_loc_start);
    size_t file_end_marker = type_str_start + type_str_len;

    for (size_t i = 0; i < all_rows.size(); i++)
    {
        size_t data_offset = data_start + i * (size_t)row_data_size;
        new_locators[i].row_id = (uint64_t)all_rows[i].row_id;
        new_locators[i].param_offset = data_offset;
        new_locators[i].param_end_offset = file_end_marker;
        memcpy(new_file + data_offset, all_rows[i].data_ptr, (size_t)row_data_size);
        new_wrapper_locs[i].row = all_rows[i].row_id;
        new_wrapper_locs[i].index = (int32_t)i;

        // Patch our new rows: textId -> their own row id (so the FMG-lookup
        // side resolves to the entries we injected separately), clear
        // unlockEventFlagId so no gate prevents display. repeatType is set
        // to 1 explicitly: ERR's template carries 1, but vanilla menuType=0
        // rows ship repeatType=0 (show-once) — the toast must repeat.
        int32_t rid = all_rows[i].row_id;
        // All MFG toast rows: the fixed banners + coverage-gap + 14 section
        // banners + 6 per-category gap banners — a contiguous range above the
        // highest ERR codex id (9004250). Vanilla/ERR rows never fall in here.
        if (rid >= goblin::TUTORIAL_FMG_ID_ON &&
            rid <= goblin::gap_cat_toast_id(goblin::GAP_CAT_COUNT - 1))
        {
            auto *p = new_file + data_offset;
            *reinterpret_cast<uint8_t *>(p + 4)  = 0;      // menuType = 0 (toast)
            *reinterpret_cast<uint8_t *>(p + 6)  = 1;      // repeatType = 1 (repeatable)
            *reinterpret_cast<uint32_t *>(p + 12) = 0;     // unlockEventFlagId = 0
            *reinterpret_cast<int32_t *>(p + 16)  = rid;   // textId -> our row id
        }
    }

    file_ptr = new_file;
    file_size = (int64_t)param_file_size;

    spdlog::info("[TOAST] TutorialParam expanded: {} -> {} rows (ON={}, OFF={}, DUMP_OK={}, DUMP_FAIL={})",
                 orig_rows, total_rows, TUTORIAL_NEW_ROW_ID_ON, TUTORIAL_NEW_ROW_ID_OFF,
                 TUTORIAL_NEW_ROW_ID_DUMP_OK, TUTORIAL_NEW_ROW_ID_DUMP_FAIL);
    return true;
}



// (The old Summon-message path (post_summon) was removed: it depended on five
// hardcoded RVAs (0x763360/0x11A3E0/0x843860/0x844060/0x843910) that a game
// update invalidates, and the codex trampoline below is the toast style we
// actually ship. The F10/F9 banner uses the AOB-resolved trampoline only.)

// ShowTutorialPopup callers — codex/medal upper-left toast.
// Three entries pinned by static analysis (agent run, May 2026):
//   - inner   0x7EF5B0  `void(CSPopupMenu*, int id, bool, bool)` (286-byte fn)
//   - outer   0x7EE630  `void(CSPopupMenu*, int id, bool)` (4 direct call sites)
//   - tramp   0x80DA50  `void(int id)` — resolves singleton internally
// CSPopupMenu singleton ptr lives in .data at `CSFeMan_slot + 0x80`.
// AOB anchor for outer (24 bytes, unique across image):
//   48 8B C4 44 88 40 18 89 50 10 55 56 57 41 56 41 57 48 8D 68 A1 48 81 EC
// Patch-resilient anchor: LEA xref in real-.text to string
//   "CS::CSPopupMenu::_CanOpenTutorialParam" in .rdata.
//
// Note: eldenring.exe has TWO `.text` sections (VMProtect adds one). When
// pinning via pefile, scan the original MSVC `.text` at RVA 0x1000..0x29A3000,
// NOT the VMP-added one at 0x4C0E000+ — different content, will miss real fns.
// Resolve the trampoline by AOB (NOT a hardcoded RVA): a game update shifts
// every function's RVA (the May-2026 patch moved this one from 0x80DA50 to
// 0x80D960), so we pin it by a stable surrounding-byte signature that survives
// patches. modutils::scan returns the address of the AOB's first byte = the
// function entry. Resolved once and cached.
static void show_tutorial_popup_trampoline(uintptr_t /*er*/, int tutorial_id)
{
    static void (*fn)(int) = nullptr;
    static bool tried = false;
    if (!tried)
    {
        tried = true;
        fn = reinterpret_cast<void (*)(int)>(modutils::scan<void>({
            .aob = goblin::sig::WORLDMAP_POINT_FN,
        }));
        spdlog::info("[TOAST] trampoline ShowTutorialPopup @ {:p}", (void *)fn);
    }
    if (fn) fn(tutorial_id);
}

// SEH-guarded dispatch of one toast method. POD-only locals (no C++ unwinding).
static void seh_dispatch_toast(int method, uintptr_t er, void * /*mm*/, void *fe,
                               void ** /*csfeman_slot*/, bool icons_on,
                               const wchar_t *text)
{
    (void)method; (void)fe; (void)text;
    int tutorial_id = icons_on ? goblin::TUTORIAL_FMG_ID_ON : goblin::TUTORIAL_FMG_ID_OFF;
    __try
    {
        // Only the AOB-resolved codex trampoline remains (Summon path removed).
        show_tutorial_popup_trampoline(er, tutorial_id);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// Fire a toast using the currently-selected method (cycled by F11). Resolves
// the module base + singleton slots once.
bool goblin::world_map_open()
{
    static bool resolved = false;
    static void **menu_man_slot = nullptr;
    if (!resolved)
    {
        resolved = true;
        // Same CSMenuMan singleton AOB as the toast path below.
        menu_man_slot = reinterpret_cast<void **>(modutils::scan<void *>({
            .aob = goblin::sig::CSMENUMAN_SLOT,
            .relative_offsets = {{3, 7}},
        }));
        spdlog::info("[OVERLAY] world_map_open CSMenuMan_slot={:p}", (void *)menu_man_slot);
    }
    if (!menu_man_slot) return false;
    void *mm = *menu_man_slot;
    if (!mm) return false;
    // 0xCD = per-screen menu-state byte; 7 = the world-map screen is up (value
    // from a previous build — log every distinct value so we can confirm/correct
    // it on this build by opening the map and reading the log).
    uint8_t v = reinterpret_cast<uint8_t *>(mm)[0xCD];
    static int last = -1;
    if (v != last)
    {
        spdlog::info("[OVERLAY] CSMenuMan+0xCD changed -> {}", static_cast<int>(v));
        last = v;
    }
    return v == 7;
}


// ── Native discovered-grace pin suppression (RE e4b3f6a; config grace_suppress_native) ──
// Graces are WorldMapWarpPinData built by FUN_14088b7b0(this, out, WarpData* param_3) from a
// WarpData whose source entry (warpData+0x8) holds: state byte @+0x1E (bits 0/1/2 = registered/
// discovered/visible), iconId @+0x08. PHASE A (now): hook + LOG each build ([WARPPIN] state/iconId)
// to confirm we can identify discovered graces at build time. Suppression (skip/hide the discovered
// ones) is added once the log confirms identification. Read-only RPM in the detour; calls the orig.
namespace
{
using warp_pin_fn = void *(__fastcall *)(void *, void *, void *, void *);
warp_pin_fn g_warp_pin_orig = nullptr;

// Local duplicate of goblin_icon_harvest.cpp's icon_rpm_i32/icon_rpm_ptr (same
// per-file-copy convention as e.g. goblin_markers.cpp / goblin_kindling.cpp keeping
// their own AOB copies) — this detour is the one place in this file that still
// needs them after the icon-harvest block moved out (PR 1).
inline bool icon_rpm_i32(uintptr_t a, int &out)
{
    SIZE_T n = 0;
    return a && ReadProcessMemory(GetCurrentProcess(), (void *)a, &out, 4, &n) && n == 4;
}
inline bool icon_rpm_ptr(uintptr_t a, uintptr_t &out)
{
    SIZE_T n = 0;
    return a && ReadProcessMemory(GetCurrentProcess(), (void *)a, &out, 8, &n) && n == 8;
}

void *__fastcall warp_pin_detour(void *a1, void *a2, void *warpData, void *a4)
{
    void *ret = g_warp_pin_orig(a1, a2, warpData, a4);
    if (!goblin::config::graceSuppressNative) return ret;

    // Identify the just-built grace pin's draw state (source state byte @ warpData+0x8 +0x1E;
    // state != 0 = a drawn/registered grace, as confirmed by [WARPPIN]).
    uintptr_t src = 0; icon_rpm_ptr(reinterpret_cast<uintptr_t>(warpData) + 0x8, src);
    int state = 0, iconId = -1;
    if (src > 0x10000)
    {
        int sb = 0; icon_rpm_i32(src + 0x1C, sb);   // +0x1E = byte 2 of the dword at +0x1C
        state = (sb >> 16) & 0xff;
        icon_rpm_i32(src + 0x08, iconId);
    }

    // Suppression now happens at the vt[1] SetTo apply point (warp_setto_detour below) by hiding
    // the pin's "Icon_0" child — NOT here. Zeroing pin+0x60/+0xC suppressed the draw but also broke
    // fast-travel (the map cursor only snaps to a _visible widget; draw + click are coupled at
    // _visible — RE windows_grace_warppin_teleport_re_findings.md). This builder hook is kept
    // log-only (phase-A diagnostic: confirms discovered-grace identity at build time).
    static int logged = 0;
    if (logged < 40)
    {
        ++logged;
        spdlog::info("[WARPPIN] build src={:#x} stateByte(+0x1E)={:#x} (bits0-2={}) iconId={} pin={:#x}",
                     src, state & 0xff, state & 7, iconId, reinterpret_cast<uintptr_t>(ret));
    }
    return ret;
}

// ── Grace DRAW-ONLY suppression (RE windows_grace_warppin_teleport_re_findings.md §4) ──
// Hook vt[1] WorldMap(Warp|Point)PinData::SetTo = FUN_14087ae20: the per-refresh widget bind that
// sets widget._visible from pin+0xC. After the original runs, for a DISCOVERED WarpPinData, hide
// ONLY the "Icon_0" child of the pin's GFx widget — the outer widget stays _visible, so the map
// cursor still snaps to it and fast-travel works; only the native icon image is gone (the overlay
// draws our own). pin+0xC/+0x60 are left untouched (poking them is the layer-trap that re-fills
// every SetTo). Runs on the engine thread (in-context), so we call the GFx fns directly.
using setto_fn = void *(__fastcall *)(void *, void *, void *, void *);
setto_fn g_setto_orig = nullptr;
// GFx helpers (resolve at hook-install, cached): get a named child proxy / set a widget _visible /
// release the stack proxy. Signatures inferred from the RE pseudocode (doc §4).
void *g_warppin_vftable = nullptr;   // er + 0x2ad8228 (WorldMapWarpPinData::vftable) — grace filter

void *__fastcall warp_setto_detour(void *pin, void *widgetRoot, void *a3, void *a4)
{
    // Decide suppression BEFORE calling orig. SetTo binds the per-pin GFx row, reading pin+0xC to set
    // the row's _visible — then RELEASES the (stack) widget proxy at its end (FUN_140d7f850). So poking
    // the proxy AFTER orig is a no-op on a dead proxy (that was the bug). Instead force pin+0xC=0 around
    // orig so SetTo's OWN set_visible(widgetRoot,0) runs while the proxy is live → row hidden. Restore
    // pin+0xC afterward so the cursor's nearest-pin selection (FUN_1409cab60, reads pin+0xC + position,
    // NOT the row _visible) still treats the grace as selectable → fast-travel survives.
    bool suppress = false;
    uint8_t *pVis = nullptr;
    uint8_t savedVis = 0;
    if (goblin::config::graceSuppressNative && goblin::config::graceOverlay && pin && widgetRoot
        && *reinterpret_cast<void **>(pin) == g_warppin_vftable)
    {
        uint32_t state = *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(pin) + 0x60);
        if ((state & 7) != 0)   // discovered grace (undiscovered → engine draws nothing anyway)
        {
            suppress = true;
            pVis = reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(pin) + 0xC);
            savedVis = *pVis;
            *pVis = 0;   // SetTo will bind the row _visible = 0
        }
    }
    void *ret = g_setto_orig(pin, widgetRoot, a3, a4);
    if (suppress)
        *pVis = savedVis ? savedVis : 1;   // restore so selection vt[6] keeps the grace clickable
    return ret;
}
} // namespace

void goblin::install_grace_suppression_hook()
{
    if (!goblin::config::graceSuppressNative) return;
    uintptr_t er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!er) return;
    void *fn = reinterpret_cast<void *>(er + 0x88b7b0);   // FUN_14088b7b0 (RE e4b3f6a §1)
    try
    {
        modutils::hook(fn, reinterpret_cast<void *>(&warp_pin_detour),
                       reinterpret_cast<void **>(&g_warp_pin_orig));
        spdlog::info("[WARPPIN] WarpPinData builder hooked @ {} (phase A: log only)", fn);
    }
    catch (const std::exception &e)
    {
        spdlog::error("[WARPPIN] hook failed: {}", e.what());
        g_warp_pin_orig = nullptr;
    }

    // DRAW-ONLY suppression: hook vt[1] SetTo and force pin+0xC=0 around orig so SetTo's own
    // set_visible hides the per-pin GFx row while its proxy is live (poking after orig hit a released
    // proxy = no-op, the earlier bug). pin+0xC is restored after orig so the cursor's nearest-pin
    // selection (FUN_1409cab60, reads pin+0xC + position) keeps the grace clickable → teleport survives.
    // (RE windows_grace_warppin_cursor_re_findings.md.)
    constexpr bool kSetToHookEnabled = true;
    if (kSetToHookEnabled)
    {
        g_warppin_vftable = reinterpret_cast<void *>(er + 0x2ad8228); // WorldMapWarpPinData::vftable
        void *setto = reinterpret_cast<void *>(er + 0x87ae20);   // FUN_14087ae20 (vt[1] SetTo)
        try
        {
            modutils::hook(setto, reinterpret_cast<void *>(&warp_setto_detour),
                           reinterpret_cast<void **>(&g_setto_orig));
            spdlog::info("[WARPPIN] SetTo hooked @ {} (draw-only: hide row, keep teleport)", setto);
        }
        catch (const std::exception &e)
        {
            spdlog::error("[WARPPIN] SetTo hook failed: {}", e.what());
            g_setto_orig = nullptr;
        }
    }
    else
    {
        (void)&warp_setto_detour; // keep referenced while the hook is disabled
        spdlog::info("[WARPPIN] SetTo draw-only hook DISABLED (proxy ABI crashed; pending RE)");
    }
}

// ── Overlay control API (see goblin_inject.hpp) ──────────────────────────
int goblin::ui::section_count() { return SECTION_COUNT; }

const char *goblin::ui::section_label(int idx)
{
    if (idx < 0 || idx >= SECTION_COUNT) return "";
    return section_name(static_cast<Section>(idx));
}

bool goblin::ui::section_visible(int idx)
{
    if (idx < 0 || idx >= SECTION_COUNT) return false;
    return g_section_visible[idx].load();
}

void goblin::ui::set_section_visible(int idx, bool visible)
{
    if (idx < 0 || idx >= SECTION_COUNT) return;
    // Same intent the F7/F8 hotkey posts: flip state, request apply + toast.
    // The watcher (menu_auto_toggle_loop) applies the areaNo flips and persists.
    g_section_visible[idx].store(visible);
    g_section_apply_req.store(idx);
    // No toast: the overlay menu checkbox IS the feedback. The old per-section
    // "shown/hidden" banners were the icon-toggle toasts; the toast channel is
    // now used for the live coverage-gap notice instead.
}

bool goblin::ui::icons_enabled() { return !g_icons_user_disabled.load(); }
void goblin::ui::set_icons_enabled(bool on) { g_icons_user_disabled.store(!on); }

int goblin::ui::category_count() { return NUM_CATEGORIES; }

const char *goblin::ui::category_label(int idx)
{
    if (idx < 0 || idx >= NUM_CATEGORIES) return "";
    return goblin::markers::category_name(static_cast<Category>(idx));
}

int goblin::ui::category_section(int idx)
{
    if (idx < 0 || idx >= NUM_CATEGORIES) return -1;
    return static_cast<int>(section_of(static_cast<Category>(idx)));
}

bool goblin::ui::category_visible(int idx)
{
    if (idx < 0 || idx >= NUM_CATEGORIES) return false;
    return g_category_visible[idx].load();
}

void goblin::ui::set_category_visible(int idx, bool visible)
{
    if (idx < 0 || idx >= NUM_CATEGORIES) return;
    g_category_visible[idx].store(visible);
    g_category_dirty[idx].store(true);  // watcher applies the areaNo park/restore
}

// ERR integration convenience: ERR already marks bosses on the world map, so this
// hides MapForGoblins' own boss markers (the WorldBosses category) to avoid the
// duplicate. Reuses the per-category visibility flag (persists as show_bosses).
bool goblin::ui::err_hide_bosses()
{
    return !g_category_visible[static_cast<int>(Category::WorldBosses)].load();
}
void goblin::ui::set_err_hide_bosses(bool hide)
{
    goblin::ui::set_category_visible(static_cast<int>(Category::WorldBosses), !hide);
}

bool goblin::ui::category_clustered(int idx)
{
    if (idx < 0 || idx >= NUM_CATEGORIES) return true;
    return g_category_cluster[idx].load();
}

void goblin::ui::set_category_clustered(int idx, bool clustered)
{
    if (idx < 0 || idx >= NUM_CATEGORIES) return;
    g_category_cluster[idx].store(clustered);
    // LIVE: replan reads g_category_cluster directly, so checked ⇔ this category
    // joins the location pile / unchecked ⇔ shown normally — applied on next map
    // open (no restart). Persisted into clusterExclude by the Save path.
    g_cluster_replan_dirty.store(true);
}

int goblin::ui::category_threshold(int idx)
{
    if (idx < 0 || idx >= NUM_CATEGORIES) return goblin::config::clusterThreshold;
    return g_category_threshold[idx].load();
}

void goblin::ui::set_category_threshold(int idx, int threshold)
{
    if (idx < 0 || idx >= NUM_CATEGORIES) return;
    if (threshold < 1) threshold = 1;  // 0 = "cluster piles of 0" → every cell, pool blowout
    if (threshold > 255) threshold = 255;
    g_category_threshold[idx].store(threshold);  // persisted on Save (overrides), restart to apply
}

int goblin::ui::global_threshold() { return goblin::config::clusterThreshold; }
void goblin::ui::set_global_threshold(int t)
{
    if (t < 1) t = 1; if (t > 255) t = 255;  // 0 → every cell clusters → pool blowout/crash
    int old = goblin::config::clusterThreshold;
    goblin::config::clusterThreshold = static_cast<uint8_t>(t);
    // Categories still tracking the old default follow the new default, so they
    // aren't written as spurious per-category overrides on Save.
    for (int c = 0; c < NUM_CATEGORIES; c++)
        if (g_category_threshold[c].load() == old) g_category_threshold[c].store(t);
    g_cluster_replan_dirty.store(true);  // re-plan live (no restart)
}

// Hard (mixed-category) vs Soft (per-category) clustering — live, re-plans.
bool goblin::ui::cluster_hard() { return goblin::config::clusterHard; }
void goblin::ui::set_cluster_hard(bool on)
{
    goblin::config::clusterHard = on;
    g_cluster_replan_dirty.store(true);
}

// Re-plan request for settings the overlay writes to config::* directly (the
// distance-adaptive knobs + presets). Applied on the next map open.
void goblin::ui::request_cluster_replan()
{
    g_cluster_replan_dirty.store(true);
}

// Save request posted by the menu; the watcher does the file I/O.
static std::atomic<bool> g_save_req{false};
void goblin::ui::request_save() { g_save_req.store(true); }

// Danger zone. Clearing quest progress is just a string reset (render-thread
// safe; the browser reparses config::questProgress each frame). Reset-to-defaults
// re-seeds + writes the ini, so it's posted to the watcher to keep file I/O off
// the render thread.
static std::atomic<bool> g_reset_defaults_req{false};
void goblin::ui::reset_quest_progress() { goblin::config::questProgress.clear(); }
void goblin::ui::reset_to_defaults() { g_reset_defaults_req.store(true); }

// Sync the live section/category visibility into the config vars, then write the
// ini. The menu is now the category authority, so drop the showAll shortcut
// (else it would force every category back on at next load).
static void persist_settings()
{
    goblin::config::sectionEquipment = g_section_visible[0].load();
    goblin::config::sectionKeyItems  = g_section_visible[1].load();
    goblin::config::sectionLoot      = g_section_visible[2].load();
    goblin::config::sectionMagic     = g_section_visible[3].load();
    goblin::config::sectionQuest     = g_section_visible[4].load();
    goblin::config::sectionReforged  = g_section_visible[5].load();
    goblin::config::sectionWorld     = g_section_visible[6].load();

    goblin::config::showAll = false;
    for (int c = 0; c < NUM_CATEGORIES; c++)
        if (bool *p = category_config_ptr(static_cast<Category>(c)))
            *p = g_category_visible[c].load();

    // Persist the master on/off so it survives a restart.
    goblin::config::iconsHidden = g_icons_user_disabled.load();

    // Serialise per-category cluster opt-outs into clusterExclude (comma list of
    // the unchecked categories' names) so the next launch rebuilds the plan
    // accordingly. Uses the canonical category name = what category_in_list matches.
    {
        std::string ex;
        for (int c = 0; c < NUM_CATEGORIES; c++)
            if (!g_category_cluster[c].load())
            {
                if (!ex.empty()) ex += ',';
                ex += goblin::markers::category_name(static_cast<Category>(c));
            }
        goblin::config::clusterExclude = std::move(ex);
    }

    // Serialise per-category threshold overrides: "Name:N" for every category
    // whose effective threshold differs from the global default. Empty if none.
    {
        int def = goblin::config::clusterThreshold;
        std::string ov;
        for (int c = 0; c < NUM_CATEGORIES; c++)
        {
            int t = g_category_threshold[c].load();
            if (t != def)
            {
                if (!ov.empty()) ov += ',';
                ov += std::string(goblin::markers::category_name(static_cast<Category>(c)))
                      + ':' + std::to_string(t);
            }
        }
        goblin::config::clusterThresholdOverrides = std::move(ov);
    }

    goblin::save_all_bool_settings(goblin::config_ini_path());
}

bool goblin::ui::clustering_enabled() { return goblin::config::enableClustering; }
void goblin::ui::set_clustering_enabled(bool on)
{
    goblin::config::enableClustering = on;
    g_clusters_expanded.store(!on);       // enabled ⇔ collapsed (piles shown)
    g_cluster_replan_dirty.store(true);   // re-plan live: off tears down, on rebuilds
}

bool goblin::ui::clusters_expanded() { return g_clusters_expanded.load(); }
void goblin::ui::set_clusters_expanded(bool expanded)
{
    g_clusters_expanded.store(expanded);
    // Persist the live on/off intent: collapsed (clustered) ⇔ enableClustering.
    goblin::config::enableClustering = !expanded;
    // enable/disable changes whether the plan exists, so re-plan (it rebuilds when
    // enabled, clears when disabled, and applies the collapsed/expanded view).
    g_cluster_replan_dirty.store(true);
}

bool goblin::ui::cluster_debug() { return g_cluster_debug.load(); }
void goblin::ui::set_cluster_debug(bool on)
{
    g_cluster_debug.store(on);
    g_cluster_debug_dirty.store(true);
}

static void show_toggle_banner(bool icons_on)
{
    static bool resolved = false;
    static uintptr_t er = 0;
    static void **menu_man_slot = nullptr;
    static void **fe_man_slot = nullptr;
    if (!resolved)
    {
        resolved = true;
        er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
        menu_man_slot = reinterpret_cast<void **>(modutils::scan<void *>({
            .aob = goblin::sig::CSMENUMAN_SLOT,
            .relative_offsets = {{3, 7}},
        }));
        fe_man_slot = reinterpret_cast<void **>(modutils::scan<void *>({
            .aob = goblin::sig::EVENT_FLAG_MAN_SLOT_ALT,
            .relative_offsets = {{3, 7}},
        }));
        spdlog::info("[TOAST] resolve er=0x{:X} CSMenuMan_slot={:p} CSFeMan_slot={:p}",
                     er, (void *)menu_man_slot, (void *)fe_man_slot);
    }
    if (!er || !menu_man_slot || !fe_man_slot) return;
    void *mm = *menu_man_slot, *fe = *fe_man_slot;
    if (!mm || !fe) return;

    int method = g_toast_method.load();
    if (method < 0 || method >= TOAST_METHOD_COUNT) method = 0;
    const wchar_t *text = icons_on ? L"Map icons: ON" : L"Map icons: OFF";
    spdlog::info("[TOAST] fire method [{}] (icons {})", TOAST_METHOD_NAMES[method],
                 icons_on ? "ON" : "OFF");
    seh_dispatch_toast(method, er, mm, fe, fe_man_slot, icons_on, text);
}

// SEH-guarded trampoline fire (POD-only locals — no C++ unwinding).
static void seh_fire_trampoline(uintptr_t er, int tutorial_id)
{
    __try { show_tutorial_popup_trampoline(er, tutorial_id); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// Fire an upper-left codex toast for one of the injected TutorialParam rows
// (a TUTORIAL_FMG_ID_* id). Static text via the same trampoline path as the
// F10 banner — no FMG rewrite. Used by the F9 marker-dump banner.
void goblin::show_codex_toast(int tutorial_id)
{
    static uintptr_t er = 0;
    if (!er) er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!er) return;
    seh_fire_trampoline(er, tutorial_id);
}

// Queue a codex toast to be fired (spaced) by the watcher thread. Thread-safe;
// callable from any thread (e.g. the debug-events drain). Drops silently if the
// queue is full so a flood of gaps can't grow it without bound.
void goblin::enqueue_toast(int tutorial_id)
{
    if (tutorial_id <= 0) return;
    std::lock_guard<std::mutex> lk(g_toast_mtx);
    if (g_toast_queue.size() < TOAST_QUEUE_CAP)
        g_toast_queue.push_back(tutorial_id);
}


// WorldMapPointParam state owner. Since the 16-align fix in inject_map_entries
// (see docs/ersc_hosting_and_map_autohide.md), the expanded table is safe during
// ERSC hosting — the old "expand only while the map is open" auto-hide is no
// longer needed and has been removed. The table now stays EXPANDED always; the
// hotkey is a pure personal show/hide toggle.
//
// Desired table state:
//   userDisabled (F10/gamepad master-off) -> VANILLA  (user hid the icons)
//   else                                  -> EXPANDED  (icons everywhere)
//
// (The retired map-state auto-hide read CSMenuMan+0xCD with inverse logic;
// it's fully documented in docs/ersc_hosting_and_map_autohide.md should a
// future patch ever need it back.)
const std::vector<uint8_t *> &goblin::injected_row_ptrs()
{
    return g_injected_row_ptrs;
}

// ── Either-flag (OR) kill indicators ─────────────────────────────────
// Some quest fights have two mutually-exclusive completion flags (one per
// story branch) and no single "battle over" flag. Example: the academy
// battle — 7608 = Sellen's battle body defeated (sided with Jerren),
// 7609 = Jerren defeated (sided with Sellen); after either one, BOTH NPCs
// stop being attackable, so both markers should show the checkmark.
// Such rows are baked with the PRIMARY flag; once the ALT flag turns on
// this rewrites the matching fields so the checkmark/hide reacts within
// the running session. Pairs mirror data/quest_invader_overrides.json.
//
// Event-flag query — same AOBs as goblin_markers.cpp / goblin_kindling.cpp
// (each keeps its own local copy by established convention there).
using OrPairIsFlagFn = bool (*)(void *, uint32_t *);
static OrPairIsFlagFn g_orp_is_flag = nullptr;
static void **g_orp_event_man_slot = nullptr;
static bool g_orp_resolve_tried = false;

bool orp_flag_set(uint32_t flag_id)
{
    if (!g_orp_resolve_tried)
    {
        g_orp_resolve_tried = true;
        try
        {
            g_orp_is_flag = modutils::scan<bool(void *, uint32_t *)>(
                { .aob = goblin::sig::IS_EVENT_FLAG });
            g_orp_event_man_slot = reinterpret_cast<void **>(modutils::scan<void *>(
                { .aob = goblin::sig::EVENT_FLAG_MAN_SLOT,
                  .relative_offsets = { {3, 7} } }));
        }
        catch (...) { g_orp_is_flag = nullptr; g_orp_event_man_slot = nullptr; }
    }
    if (!g_orp_is_flag || !g_orp_event_man_slot) return false;
    void *event_man = *g_orp_event_man_slot;
    if (!event_man) return false;
    uint32_t id = flag_id;
    return g_orp_is_flag(event_man, &id);
}

// Accessors for goblin_loot_resolve.cpp's live-persistence classifier — see
// goblin_inject_shared.hpp. This resolve is genuinely shared with other sections
// in THIS file (quest-finishable cache, cluster depletion, apply_flag_or_pairs,
// goblin::ui::read_event_flag), so it stays owned here, not moved.
bool orp_manager_resolve_tried() { return g_orp_resolve_tried; }
void **orp_event_man_slot_ptr() { return g_orp_event_man_slot; }

// Part 2: per-questline "unfinishable" cache. One byte per QUEST_BROWSER entry,
// indexed by array order (same index the overlay passes). Written here on the
// watcher thread, read by ui::quest_unfinishable() on the render thread (a
// single-byte read; a benign cross-thread race at worst flips one frame late).
static std::vector<uint8_t> g_quest_unfinishable;

int goblin::refresh_quest_finishable()
{
    const size_t n = goblin::generated::QUEST_BROWSER_COUNT;
    if (g_quest_unfinishable.size() != n) g_quest_unfinishable.assign(n, 0);
    // Cold-API safety: if AlwaysOn (6001) can't read true, leave the cache as-is
    // rather than marking everything finishable on a not-yet-warm flag API.
    if (!orp_flag_set(6001)) return 0;
    int unfinishable = 0;
    for (size_t i = 0; i < n; i++)
    {
        uint32_t f = goblin::generated::QUEST_BROWSER[i].fail_flag;
        bool dead = (f != 0) && orp_flag_set(f);
        g_quest_unfinishable[i] = dead ? 1 : 0;
        if (dead) unfinishable++;
    }
    return unfinishable;
}

bool goblin::ui::quest_unfinishable(size_t i)
{
    return i < g_quest_unfinishable.size() && g_quest_unfinishable[i] != 0;
}

// Live event-flag reader exposed for the overlay's flag-capture finalize step
// (re-check captured flags so only PERSISTED ones are logged). Plain free
// function so it matches the bool(*)(uint32_t) callback the capture tool takes.
bool goblin::ui::read_event_flag(uint32_t id) { return orp_flag_set(id); }

// Per-category uncollected census — feeds the overlay's "<remaining>/<total>"
// badge next to each category. Gated to "menu on-screen" + throttled to 1s so the
// 9296-row flag sweep is free when the panel is closed. Collected detection mirrors
// cluster depletion: plain loot via textDisableFlagId1 + orp_flag_set, Reforged
// pieces/kindling via row-id tracking. Categories with no collectible rows
// (graces/NPCs/regions) cache remaining = -1 so the overlay draws no badge.
// Overlay-only census: implemented in the worldmap module (it owns the marker
// buckets). Forward-declared here so the refresh entry point can delegate to it.
namespace goblin::worldmap { void refresh_overlay_census(); }

int goblin::refresh_category_census()
{
    GOBLIN_BENCH("refresh.category_census");
    using clock = std::chrono::steady_clock;
    // Skip entirely unless the overlay panel was drawn within the last 2s.
    long long now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count();
    long long last_seen = g_menu_visible_ns.load();
    if (last_seen == 0 || now_ns - last_seen > 2'000'000'000LL) return 0;
    // Throttle: piles don't deplete fast, and the menu reads cached atomics.
    static clock::time_point last{};
    auto now = clock::now();
    if (now != clock::time_point{} && now - last < std::chrono::milliseconds(1000)) return 0;
    last = now;
    if (!orp_flag_set(6001)) return 0;  // cold flag API → don't publish bogus counts

    // Count from the OVERLAY's OWN marker layers (the exact markers
    // it draws + grays), so the F1 badge matches the map and can't diverge from a
    // parallel native-style recompute. refresh_overlay_census writes the census atomics
    // and logs [OVERLAY-CENSUS] (full dump once, then a line on each change).
    goblin::worldmap::refresh_overlay_census();
    return 0;
}

int  goblin::ui::category_total(int idx)
{
    if (idx < 0 || idx >= NUM_CATEGORIES) return 0;
    return g_cat_total[idx].load();
}
int  goblin::ui::category_remaining(int idx)
{
    if (idx < 0 || idx >= NUM_CATEGORIES) return -1;
    return g_cat_remaining[idx].load();
}
void goblin::ui::set_category_census(int idx, int total, int looted)
{
    if (idx < 0 || idx >= NUM_CATEGORIES) return;
    g_cat_total[idx].store(total);
    g_cat_remaining[idx].store(total > 0 ? (total - looted) : -1);
}
void goblin::ui::note_menu_visible()
{
    g_menu_visible_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct FlagOrPair { uint32_t primary; uint32_t alt; };
static constexpr FlagOrPair FLAG_OR_PAIRS[] = {
    {7608, 7609},  // Sellen/Jerren academy battle
};

void goblin::apply_flag_or_pairs()
{
    GOBLIN_BENCH("refresh.flag_or_pairs");
    for (const auto &pr : FLAG_OR_PAIRS)
    {
        if (!orp_flag_set(pr.alt))
            continue;
        for (uint8_t *ptr : g_injected_row_ptrs)
        {
            // Skip live-loot rows: their textDisableFlagId1 holds a lot pickup
            // flag (set by refresh_loot_from_itemlot), not a boss/quest flag —
            // don't let a value-collision rewrite it.
            if (g_lot_backed_set.count(ptr)) continue;
            auto *p = reinterpret_cast<from::paramdef::WORLD_MAP_POINT_PARAM_ST *>(ptr);
            if (p->clearedEventFlagId == pr.primary) p->clearedEventFlagId = pr.alt;
            if (p->textDisableFlagId1 == pr.primary) p->textDisableFlagId1 = pr.alt;
            if (p->textDisableFlagId2 == pr.primary) p->textDisableFlagId2 = pr.alt;
        }
    }
}

void goblin::menu_auto_toggle_loop()
{
    bool prev_user_disabled = g_icons_user_disabled.load();

    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Master-off banner. The overlay itself honours the toggle — goblin_overlay
        // skips render_markers when ui::icons_enabled() is false — so this thread only
        // fires the visual banner when the user flips master-off via the F10 hotkey.
        bool user_disabled_now = g_icons_user_disabled.load();
        if (user_disabled_now != prev_user_disabled)
        {
            show_toggle_banner(!user_disabled_now);
            prev_user_disabled = user_disabled_now;
        }

        // Per-section toggle: persist the choice to the ini (single owner of file I/O,
        // off the render thread). The overlay reads section_visible() live via the
        // ui:: getter, so no blob mutation is needed — just save.
        if (g_section_apply_req.exchange(-1) >= 0)
            goblin::save_section_states(goblin::config_ini_path());

        // Toast queue: fire one at a time, spaced so consecutive toasts don't
        // overwrite each other on screen.
        {
            int fire_id = 0;
            int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
            {
                std::lock_guard<std::mutex> lk(g_toast_mtx);
                if (!g_toast_queue.empty() && now_ms >= g_next_toast_ms)
                {
                    fire_id = g_toast_queue.front();
                    g_toast_queue.pop_front();
                    g_next_toast_ms = now_ms + TOAST_SPACING_MS;
                }
            }
            if (fire_id)
                goblin::show_codex_toast(fire_id);
        }

        // Menu "Save" → persist current visibility to the ini (file I/O here, off
        // the render thread).
        if (g_save_req.exchange(false))
            persist_settings();

        // Danger zone: re-seed config from defaults + write the ini (off the render
        // thread). Runtime visibility is unchanged until a restart.
        if (g_reset_defaults_req.exchange(false))
            goblin::reset_to_defaults_and_save(goblin::config_ini_path());
    }
}
