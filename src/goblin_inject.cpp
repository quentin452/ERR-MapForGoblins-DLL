#include "goblin_inject.hpp"
#include "goblin_collected.hpp"
#include "goblin_kindling.hpp"
#include "goblin_config.hpp"
#include "goblin_messages.hpp"
#include "modutils.hpp"
#include "re_signatures.hpp"
#include "goblin_map_data.hpp"
#include "goblin_item_icons.hpp"
#include "goblin_location_alt.hpp"
#include "goblin_grace_anchors.hpp"
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
#include "goblin_quest_gates.hpp"
#include "goblin_quest_steps.hpp"
#include "goblin_logic.hpp"

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

// Nearest Site-of-Grace anchor to a world point WITHIN THE SAME AREA. Graces are
// the authoritative named-location anchors (BonfireWarpParam); assigning each
// marker to its nearest grace gives a real, complete location grouping with no
// dependence on the marker's (often-missing) location textId. Returns the grace
// index + its region PlaceName id (for the label).
static bool find_nearest_grace(uint8_t area, float wx, float wz,
                               int *out_idx, int *out_pname, int *out_tab)
{
    int best = -1, best_named = -1;
    float bestd = 1e30f, bestnd = 1e30f;
    for (size_t i = 0; i < goblin::generated::GRACE_ANCHOR_COUNT; i++)
    {
        const auto &g = goblin::generated::GRACE_ANCHORS[i];
        if (g.area != area) continue;
        float dx = g.wx - wx, dz = g.wz - wz;
        float d = dx * dx + dz * dz;
        if (d < bestd) { bestd = d; best = static_cast<int>(i); }
        // Track the nearest grace that actually has a name, for a label fallback.
        if (g.placename_id > 0 && d < bestnd) { bestnd = d; best_named = static_cast<int>(i); }
    }
    if (best < 0) return false;
    const auto &b = goblin::generated::GRACE_ANCHORS[best];
    *out_idx = best;                      // grouping: physical-nearest grace
    *out_tab = b.tab_id;
    // Label: the grouped grace's name, else borrow the nearest NAMED grace's region
    // name (some underground/DLC graces store a tab id, not a PlaceName → no name).
    *out_pname = (b.placename_id > 0) ? b.placename_id
               : (best_named >= 0 ? goblin::generated::GRACE_ANCHORS[best_named].placename_id : 0);
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
    if (key < 0 || static_cast<size_t>(key) >= goblin::generated::GRACE_ANCHOR_COUNT)
        return false;
    const auto &a = goblin::generated::GRACE_ANCHORS[key];
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
    if (key < 0 || static_cast<size_t>(key) >= goblin::generated::GRACE_ANCHOR_COUNT)
        return -1;
    return goblin::generated::GRACE_ANCHORS[key].tab_id;
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
    if (key < 0 || static_cast<size_t>(key) >= goblin::generated::GRACE_ANCHOR_COUNT)
        return false;
    const auto &a = goblin::generated::GRACE_ANCHORS[key];
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
// flag-backed members; when ALL are set the pile is depleted → the refresh swaps
// the icon to CLUSTER_DONE_ICON_ID (green). Empty = no collectible members (a
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
    if (key < 0 || static_cast<size_t>(key) >= goblin::generated::GRACE_ANCHOR_COUNT)
        return false;
    const auto &a = goblin::generated::GRACE_ANCHORS[key];
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
    try
    {
        for (auto [rowId, row] :
             from::params::get_param<from::paramdef::BONFIRE_WARP_PARAM_ST>(L"BonfireWarpParam"))
        {
            if (row.areaNo == 0) continue;                 // not a placed grace
            if ((int)row.eventflagId <= 0 || row.textId1 <= 0) continue; // no flag / no name
            // dispMask00/01/02 are ALL bits 0/1/2 of the single byte 0x1e (verified in-memory:
            // dispMask00→0x01, dispMask01→0x02, dispMask02→0x04; byte 0x1f is pad). A grace shown
            // on NO map layer (all three 0) is an ERR intentional hide (spoiler graces). The
            // earlier (&0x3) read missed dispMask02 → wrongly hid every DLC grace.
            if ((row.dispMask0 & 0x7) == 0) { ++hidden; continue; }
            const bool underground = (row.iconId == 44);
            if (underground) ++ug;
            g_live_graces.push_back({ row.areaNo, row.gridXNo, row.gridZNo,
                                      row.posX, row.posZ, row.textId1, rowId,
                                      (int)row.eventflagId, underground });
        }
    }
    catch (...) {}

    spdlog::info("[LIVE-GRACE] {} grace rows from live BonfireWarpParam ({} underground/cave, "
                 "{} ERR-hidden skipped)", g_live_graces.size(), ug, hidden);
}

const std::vector<goblin::LiveGrace> &goblin::live_graces() { return g_live_graces; }

// ── Fog-of-war reveal gate (RE: docs/re/windows_fog_reveal_mask_re_findings.md) ──
// A WorldMapPieceParam piece covers the map-space rectangle openTravelArea L/R/T/B and is
// revealed ⇔ IsEventFlag(openEventFlagId) (flag 0 = always shown). A marker is fogged when
// the piece whose rect contains it has an unset reveal flag. This is the engine's TRUE fog
// state — replaces the coarse MapList/marker_fragment_flag approximation once calibrated.
namespace
{
struct FogPiece { int layer; float l, r, t, b; uint32_t flag; };
std::vector<FogPiece> g_fog_pieces;
bool g_fog_built = false;

void build_fog_pieces()
{
    if (g_fog_built) return;
    g_fog_built = true;
    int per_layer[16] = {0};
    float bbL[16], bbR[16], bbT[16], bbB[16];
    for (int i = 0; i < 16; ++i) { bbL[i] = 1e30f; bbR[i] = -1e30f; bbT[i] = 1e30f; bbB[i] = -1e30f; }
    try
    {
        for (auto [rowId, row] :
             from::params::get_param<from::paramdef::WORLD_MAP_PIECE_PARAM_ST>(L"WorldMapPieceParam"))
        {
            // rowId = areaIdx*100 + pieceIdx; areaIdx ∈ {0=overworld,1=underground,10=DLC}.
            int layer = static_cast<int>(rowId / 100);
            FogPiece p{layer, row.openTravelAreaLeft, row.openTravelAreaRight,
                       row.openTravelAreaTop, row.openTravelAreaBottom, row.openEventFlagId};
            g_fog_pieces.push_back(p);
            int li = (layer >= 0 && layer < 16) ? layer : 0;
            per_layer[li]++;
            bbL[li] = std::min(bbL[li], p.l); bbR[li] = std::max(bbR[li], p.r);
            bbT[li] = std::min(bbT[li], p.t); bbB[li] = std::max(bbB[li], p.b);
        }
    }
    catch (...) {}
    for (int i = 0; i < 16; ++i)
        if (per_layer[i])
            spdlog::info("[FOGCAL] layer {} : {} pieces, rect bbox=({:.1f},{:.1f})-({:.1f},{:.1f})",
                         i, per_layer[i], bbL[i], bbT[i], bbR[i], bbB[i]);
    spdlog::info("[FOGCAL] {} WorldMapPieceParam pieces loaded", g_fog_pieces.size());
}
} // namespace

bool goblin::marker_fogged(int areaIdx, float mx, float my)
{
    if (!g_fog_built) build_fog_pieces();
    for (const FogPiece &p : g_fog_pieces)
    {
        if (p.layer != areaIdx) continue;
        if (mx < p.l || mx > p.r || my < p.t || my > p.b) continue; // not this piece
        if (p.flag == 0) return false;                              // ungated piece
        return !goblin::ui::read_event_flag(p.flag);                // fogged if flag unset
    }
    return false; // covered by no piece ⇒ ungated (matches the engine)
}

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

namespace
{
// One ItemLotParam row, read by raw offset (ITEMLOT_PARAM_ST = 152 bytes,
// shared layout for _map and _enemy).
struct RawItemLotRow { uint8_t b[0x98]; };

// Reads ItemLotParam_map / _enemy from live memory once, then resolves rows by
// id. Shared by inject_map_entries (live icon/category) and
// refresh_loot_from_itemlot (live hide-flags / labels).
struct LotReader
{
    std::optional<from::params::ParamTableSequence<RawItemLotRow>> map_lots, enemy_lots;
    void init()
    {
        // ParamTableSequence has a const member (not copy-assignable) → emplace.
        try { map_lots.emplace(from::params::get_param<RawItemLotRow>(L"ItemLotParam_map")); } catch (...) {}
        try { enemy_lots.emplace(from::params::get_param<RawItemLotRow>(L"ItemLotParam_enemy")); } catch (...) {}
    }
    bool ok() const { return map_lots.has_value() || enemy_lots.has_value(); }
    RawItemLotRow *row(uint32_t lot_id, uint8_t lot_type)
    {
        auto &pref  = (lot_type == 2) ? enemy_lots : map_lots;
        auto &other = (lot_type == 2) ? map_lots : enemy_lots;
        // try_get = non-throwing, non-logging: a missing lot (ERR/randomizer removed it)
        // is an expected miss, not an error — just fall back to the other table / baked.
        if (pref)  { if (RawItemLotRow *r = pref->try_get(lot_id))  return r; }
        if (other) { if (RawItemLotRow *r = other->try_get(lot_id)) return r; }
        return nullptr;
    }
};

// Encode a live item (id + ItemLotParam category 1-5) into the offset-encoded
// key used by both marker textIds and the generated ITEM_ICONS table.
inline int32_t encode_live_item(int32_t item_id, int32_t cat)
{
    switch (cat)
    {
        case 1: return item_id + 500000000;                                       // goods
        case 2: return (item_id >= 50000000) ? item_id : item_id + 100000000;     // ammo / weapon
        case 3: return item_id + 200000000;                                       // protector
        case 4: return item_id + 300000000;                                       // accessory
        case 5: return item_id + 400000000;                                       // gem (ash of war)
        default: return 0;
    }
}

// Spoiler-free (config::anonymousLoot) constants. The generic label reuses the
// localized BloodMsg word "something" (id 32004) at the +950M encoding (copied
// into PlaceName by setup_messages). The icon is our gray "?" frame added to
// sprite 171 of the worldmap gfx (next free frame after the tinted variants).
constexpr int32_t ANON_LABEL_TEXTID = 950000000 + 32004;  // "something"
// gray "?" frame — generated per profile (goblin::generated::ANON_ICON_ID),
// 440 on a vanilla-base gfx, shifted by the icon-frame offset on Convergence.

// Binary-search the baked item-icon table (sorted by key).
const goblin::generated::ItemIcon *lookup_item_icon(int32_t key)
{
    const auto *begin = goblin::generated::ITEM_ICONS;
    const auto *end   = begin + goblin::generated::ITEM_ICON_COUNT;
    const auto *it = std::lower_bound(begin, end, key,
        [](const goblin::generated::ItemIcon &a, int32_t k) { return a.key < k; });
    return (it != end && it->key == key) ? it : nullptr;
}
} // namespace

// Public wrapper: marker/item key → real inventory iconId (or -1). Lets the overlay map
// renderer route lot/item markers through the native GPU icon harvest (ensure_item_icon_srv).
int goblin::item_icon_id(int32_t key)
{
    const goblin::generated::ItemIcon *p = lookup_item_icon(key);
    return p ? (int)p->iconId : -1;
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


// ── Icon-texture probe (config dump_icon_textures) ───────────────────────────────────
// Hooks CSScaleformImageCreator::CreateImage (FUN_140d6bbc0): builds each GFx image from a
// TPF import → a CS::CSTextureImage carrying the sprite RECT (+0x74/+0x7c/+0x3c/+0x40, dims
// +0x2c/+0x30) + backing GXTexture2D (+0x38 → ID3D12Resource +0x40). RE findings §2/§7. Logs
// the created image's rect/dims + backing texture + the import args so we can map iconIds →
// sprite sub-rects. Read-only (RPM); generous arg list forwarded (x64 caller-clean → extra
// args are harmless even if CreateImage takes fewer).
namespace
{
using create_image_fn = void *(__fastcall *)(void *, void *, void *, void *, void *, void *,
                                             void *, void *);
create_image_fn g_create_image_orig = nullptr;

// CreateImage context capture (RE §5g) — populated by create_image_detour, consumed by
// run_create_icon to replay the GFx per-image bind for an arbitrary symbol. a0=param_1 (creator this),
// a1=param_2, a2=param_3 (symbol desc: ASCII name @ (*a2 & ~3)+0xc, format "%hS").
std::atomic<void *> g_ci_p1{nullptr};
std::atomic<void *> g_ci_p2{nullptr};
std::atomic<int> g_ci_logn{0};
char g_ci_last[96] = {0};   // most-recent live symbol name (e.g. "img://KG_R1") for replay control test

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

// Bounded BFS over the CS object graph from `root` (follow er-base-vtable'd ptr fields)
// for the GXTexture2D / CSGxTexture vtable → the backing GPU texture. Returns its addr
// (+ out vtable/depth), 0 if not found. Used at load (usually fails — texture bound
// lazily) and on a map-open frame from root = *(img+0x10) (the now-bound Render::Texture).
uintptr_t icon_find_gpu_tex(uintptr_t root, uintptr_t base, uintptr_t &out_vt, int &out_depth)
{
    // ONLY GXTexture2D (0x2f05928) is the terminal — it carries the ID3D12Resource at +0x40.
    // CSGxTexture (0x2b761b0) is a WRAPPER whose +0x40 is a name string, so we expand THROUGH
    // it (and every other CS object) rather than stopping on it.
    const uintptr_t WANT_GX = base + 0x2f05928;
    std::vector<std::pair<uintptr_t, int>> q;
    std::vector<uintptr_t> seen;
    q.push_back({root, 0}); seen.push_back(root);
    for (size_t qi = 0; qi < q.size() && qi < 600; ++qi)
    {
        uintptr_t node = q[qi].first; int d = q[qi].second;
        for (int o = 0x00; o <= 0x120; o += 8)
        {
            uintptr_t p = 0;
            if (!icon_rpm_ptr(node + o, p) || p < 0x10000 || p >= 0x7fffffffffffULL) continue;
            uintptr_t vt = 0;
            if (!icon_rpm_ptr(p, vt)) continue;
            if (vt == WANT_GX) { out_vt = vt; out_depth = d + 1; return p; }
            if (d < 7 && vt > base && vt < base + 0x6000000 && q.size() < 600)
            {
                bool dup = false;
                for (uintptr_t s : seen) if (s == p) { dup = true; break; }
                if (!dup) { seen.push_back(p); q.push_back({p, d + 1}); }
            }
        }
    }
    out_vt = 0; out_depth = -1;
    return 0;
}

// Try to read a printable string at `addr` — narrow (char) first, then wide (char,0,char,0).
// Returns "" if no run of >=3 printable ASCII. Used to find the GFx import NAME the image was
// built from (findings §4: CreateImage builds from an import descriptor; the resolve path
// formats L"%s_ptl" from its wide name → label iconId by that name).
std::string icon_try_str(uintptr_t addr)
{
    if (!addr || addr < 0x10000 || addr >= 0x7fffffffffffULL) return {};
    unsigned char buf[96] = {0};
    SIZE_T n = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (void *)addr, buf, sizeof(buf) - 1, &n) || n < 4)
        return {};
    auto printable = [](unsigned char c) { return (c >= 0x20 && c < 0x7f); };
    // narrow
    int run = 0; for (int i = 0; i < (int)n && printable(buf[i]); ++i) run++;
    if (run >= 4) return std::string((char *)buf, run);
    // wide (every other byte 0)
    std::string w; for (int i = 0; i + 1 < (int)n; i += 2)
    {
        if (printable(buf[i]) && buf[i + 1] == 0) w.push_back((char)buf[i]); else break;
    }
    if (w.size() >= 4) return w;
    return {};
}

// Registry of created CSTextureImages (load-time) so the live dump can re-read each
// img+0x10 once the world map is open and the GPU texture is bound. Capped; sheet-sized
// images only (icon sheets are 512×512 / 2048×1024 — skip tiny/huge GFx images).
struct IconImg { uintptr_t img; int x0, y0, x1, y1, w, h; std::string name; };
std::vector<IconImg> g_icon_imgs;
std::vector<uintptr_t> g_icon_sheets; // unique ID3D12Resource per TPF sheet
bool g_icon_live_done = false;
bool g_icon_detail_done = false;

// One-shot full dump of the FIRST bound Render::Texture (img+0x10). The BFS proved
// non-deterministic (wanders into a shared global some runs, misses others), so dump the
// Scaleform Render::Texture's whole layout: every ptr field (tagged GXTexture2D /
// CSGxTexture / other CS-obj, + one-level child that points at GXTexture2D) and every
// small int (dims / the tex+0x3c size). Reveals the DETERMINISTIC renderTex→GPU-texture
// offset to replace the BFS, and whether all images share one atlas.
void icon_detail_dump(uintptr_t rtex, uintptr_t base)
{
    if (g_icon_detail_done || !rtex) return;
    g_icon_detail_done = true;
    const uintptr_t WANT_GX = base + 0x2f05928, WANT_CSGX = base + 0x2b761b0;
    spdlog::info("[ICONTEX-DET] renderTex={:#x} full layout dump:", rtex);
    for (int o = 0; o <= 0xB8; o += 8)
    {
        uintptr_t p = 0;
        if (!icon_rpm_ptr(rtex + o, p)) continue;
        if (p > 0x10000 && p < 0x7fffffffffffULL)
        {
            uintptr_t vt = 0; icon_rpm_ptr(p, vt);
            const char *tag = vt == WANT_GX ? " <GXTexture2D>"
                            : vt == WANT_CSGX ? " <CSGxTexture>"
                            : (vt > base && vt < base + 0x6000000) ? " <CSobj>" : "";
            spdlog::info("[ICONTEX-DET]  rtex+{:#x} = ptr {:#x} vtRVA={:#x}{}",
                         o, p, vt > base ? vt - base : 0, tag);
            // one level down: a child field pointing straight at the GXTexture2D
            if (vt > base && vt < base + 0x6000000)
                for (int o2 = 0; o2 <= 0x70; o2 += 8)
                {
                    uintptr_t c = 0; if (!icon_rpm_ptr(p + o2, c) || c < 0x10000) continue;
                    uintptr_t cv = 0; icon_rpm_ptr(c, cv);
                    if (cv == WANT_GX)
                        spdlog::info("[ICONTEX-DET]    -> [{:#x}+{:#x}] = GXTexture2D {:#x}", p, o2, c);
                }
        }
        else
        {
            int lo = (int)(p & 0xffffffff), hi = (int)(p >> 32);
            if ((lo > 0 && lo <= 8192) || (hi > 0 && hi <= 8192))
                spdlog::info("[ICONTEX-DET]  rtex+{:#x} = ints ({}, {})", o, lo, hi);
        }
    }
    // rtex+0x70 = the D3D texture HAL object (non-module vtable, repeated across the texture
    // list → a real D3D/driver type). Deep-dump it: its fields + ints, hunting the actual
    // ID3D12Resource (a ptr whose vtable is OUTSIDE the ER module = d3d12.dll / vkd3d) and the
    // texture dims/format. mod range = [base, base+0x6000000).
    const uintptr_t mod_lo = base, mod_hi = base + 0x6000000;
    uintptr_t hal = 0;
    if (icon_rpm_ptr(rtex + 0x70, hal) && hal > 0x10000 && hal < 0x7fffffffffffULL)
    {
        uintptr_t halvt = 0; icon_rpm_ptr(hal, halvt);
        spdlog::info("[ICONTEX-DET] -- HAL tex rtex+0x70 = {:#x} vt={:#x} fields:", hal, halvt);
        for (int o = 0; o <= 0xC0; o += 8)
        {
            uintptr_t p = 0;
            if (!icon_rpm_ptr(hal + o, p)) continue;
            if (p > 0x10000 && p < 0x7fffffffffffULL)
            {
                uintptr_t vt = 0; bool hasvt = icon_rpm_ptr(p, vt) && vt > 0x10000;
                bool in_mod = (vt >= mod_lo && vt < mod_hi);
                const char *tag = !hasvt ? " <data?>"
                                : in_mod ? " <CSobj>" : " <NON-MODULE vt = D3D12/COM?>";
                spdlog::info("[ICONTEX-DET]    hal+{:#x} = ptr {:#x} vt={:#x}{}",
                             o, p, hasvt ? (in_mod ? vt - base : vt) : 0, tag);
            }
            else
            {
                int lo = (int)(p & 0xffffffff), hi = (int)(p >> 32);
                if ((lo > 0 && lo <= 8192) || (hi > 0 && hi <= 8192))
                    spdlog::info("[ICONTEX-DET]    hal+{:#x} = ints ({}, {})", o, lo, hi);
            }
        }
    }
}

// Live anchors: the numeric iconIds the menu actually DRAW (MENU_FL_<N>) as the user browses.
static std::vector<uint16_t> g_menu_iconids;

// Correlate the live-captured iconIds against the EquipParam tables: for each param, the offset
// whose u16 column CONTAINS the most captured iconIds = that param's iconId offset (every drawn
// item's iconId lives at the same offset). This is the anchored self-calibration that the abstract
// distinct/density heuristics couldn't do — the anchors are real, drawn items. Hover a few items of
// each type → [ICONFIND] converges. Dev-only (dump_icon_textures).
static void correlate_menu_iconids()
{
    std::set<uint16_t> ids(g_menu_iconids.begin(), g_menu_iconids.end());
    if (ids.empty())
        return;
    // sample a few captured ids in the log so we can value-scan manually if needed
    std::string sample;
    { int k = 0; for (uint16_t v : ids) { if (k++) sample += ","; sample += std::to_string(v); if (k >= 8) break; } }
    spdlog::info("[ICONFIND] correlating {} captured iconIds (e.g. {}) vs EquipParams:",
                 (int)ids.size(), sample);
    struct P { const wchar_t *name; const char *tag; };
    static const P params[] = {
        {L"EquipParamWeapon", "Weapon"}, {L"EquipParamProtector", "Protector"},
        {L"EquipParamAccessory", "Accessory"}, {L"EquipParamGoods", "Goods"},
        {L"EquipParamGem", "Gem"}, {L"Magic", "Magic"},
    };
    int totalBest = 0;
    for (const P &p : params)
    {
        try
        {
            std::vector<uintptr_t> bases;
            for (auto row : from::params::get_param<uint8_t>(p.name))
                bases.push_back(reinterpret_cast<uintptr_t>(&row.second));
            if (bases.size() < 2) { spdlog::info("[ICONFIND]   {} not loaded", p.tag); continue; }
            uintptr_t stride = bases[1] - bases[0];
            if (stride < 4 || stride > 0x2000) continue;
            int bestOff = -1, bestHits = 0;
            for (uintptr_t off = 0; off + 2 <= stride; ++off)
            {
                std::set<uint16_t> hit;
                for (uintptr_t b : bases)
                {
                    uint16_t v = *reinterpret_cast<uint16_t *>(b + off);
                    if (ids.count(v)) hit.insert(v);
                }
                if ((int)hit.size() > bestHits) { bestHits = (int)hit.size(); bestOff = (int)off; }
            }
            totalBest += bestHits;
            spdlog::info("[ICONFIND]   {} bestOff=0x{:x} matches {}/{}", p.tag, bestOff, bestHits, (int)ids.size());
        }
        catch (...) { spdlog::info("[ICONFIND]   {} not loaded", p.tag); }
    }
    if (totalBest == 0)
        spdlog::info("[ICONFIND] NO captured iconId appears in ANY EquipParam column → MENU_FL_<id> "
                     "is a TRANSFORMED id, not the raw EquipParam.iconId.");
}

void icon_log_image(uintptr_t img, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
    if (!goblin::config::dumpIconTextures || !img)
        return;
    static int logged = 0; // gates only the verbose [ICONTEX] line; [ICONMAP] is uncapped
    // Rect SOLVED: x0,y0,x1,y1 contiguous at +0x74/+0x78/+0x7c/+0x80; dims +0x84/+0x88.
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0, w = 0, h = 0;
    icon_rpm_i32(img + 0x74, x0); icon_rpm_i32(img + 0x78, y0);
    icon_rpm_i32(img + 0x7c, x1); icon_rpm_i32(img + 0x80, y1);
    icon_rpm_i32(img + 0x84, w);  icon_rpm_i32(img + 0x88, h);
    // Register sheet-sized images so the live dump can re-read img+0x10 once the map is
    // open (the GPU texture binds LAZILY on first render — findings §2; the BFS here at
    // load time finds nothing, which is expected). Icon sheets are 512×512 / 2048×1024.
    // The GFx import NAME (SOLVED): img+0x40 -> '<symbol>_ptl' string (the resolve fn formats
    // L"%s_ptl"). ERR map icons = 'SB_ERR_*'; 'KG_*' = controller button glyphs (skip).
    uintptr_t name_ptr = 0; icon_rpm_ptr(img + 0x40, name_ptr);
    std::string name = icon_try_str(name_ptr);
    // Register sheet-sized images so the live dump can re-read img+0x10 once the map is open
    // (the GPU texture binds LAZILY on first render). Icon sheets are 512×512 / 2048×1024.
    // Sheet-sized images (≥256), OR any SB_ERR_*/MENU_MAP_* (the grace/pin _ptl may report ICON size
    // at +0x84/+0x88, not the sheet — don't let the ≥256 gate drop the forced grace candidates).
    if (((w >= 256 && h >= 256) || name.rfind("SB_ERR_", 0) == 0 || name.rfind("MENU_MAP_", 0) == 0) &&
        g_icon_imgs.size() < 512)
        g_icon_imgs.push_back({img, x0, y0, x1, y1, w, h, name});
    // Accumulate the UNIQUE name→rect table (skip KG_ button glyphs). Uncapped (dedup set) so
    // it captures whatever menu is open — worldmap (SB_ERR_*) OR inventory/equipment item icons.
    if (!name.empty() && name.rfind("KG_", 0) != 0)
    {
        static std::set<std::string> seen;
        if (seen.insert(name).second)
        {
            spdlog::info("[ICONMAP] '{}' rect=({},{})-({},{}) sheet={}x{} ({}x{})",
                         name, x0, y0, x1, y1, w, h, x1 - x0, y1 - y0);
            // MENU_FL_<N> = a drawn item icon → N is a live iconId anchor. Accumulate + correlate
            // against the EquipParam tables to pin the iconId offset per param ([ICONFIND]).
            if (name.rfind("MENU_FL_", 0) == 0 && name.size() > 8 && name[8] >= '0' && name[8] <= '9')
            {
                uint16_t N = static_cast<uint16_t>(std::atoi(name.c_str() + 8));
                if (std::find(g_menu_iconids.begin(), g_menu_iconids.end(), N) == g_menu_iconids.end())
                {
                    g_menu_iconids.push_back(N);
                    correlate_menu_iconids();   // every new drawn-item iconId → re-correlate
                }
            }
        }
    }
    // Verbose per-image line, capped (avoid flooding when a busy menu creates hundreds).
    if (logged < 60)
    {
        logged++;
        spdlog::info("[ICONTEX] img={:#x} name='{}' rect=({},{})-({},{}) sheet={}x{}",
                     img, name, x0, y0, x1, y1, w, h);
    }
}

void *__fastcall create_image_detour(void *a0, void *a1, void *a2, void *a3, void *a4, void *a5,
                                     void *a6, void *a7)
{
    void *ret = g_create_image_orig(a0, a1, a2, a3, a4, a5, a6, a7);
    icon_log_image((uintptr_t)ret, (uintptr_t)a1, (uintptr_t)a2, (uintptr_t)a3);
    // §5g: capture the live creator context (a0/a1) + log the real symbol name (a2 = param_3 →
    // ASCII @ (*a2 & ~3)+0xc) so we can replay CreateImage for arbitrary item-icon symbols.
    if (g_ci_p1.load(std::memory_order_relaxed) == nullptr) { g_ci_p1.store(a0); g_ci_p2.store(a1); }
    int n = g_ci_logn.fetch_add(1, std::memory_order_relaxed);
    if (n < 60 && a2)
    {
        uintptr_t descv = 0;
        if (icon_rpm_ptr((uintptr_t)a2, descv))
        {
            char nm[96] = {0}; SIZE_T got = 0;
            ReadProcessMemory(GetCurrentProcess(), (void *)((descv & ~uintptr_t(3)) + 0xc), nm, 95, &got);
            if (nm[0]) strncpy(g_ci_last, nm, sizeof(g_ci_last) - 1);   // remember for replay control
            spdlog::info("[CREATEIMG] live a0={} a1={} *a2={:#x} name='{}' -> img={:#x}",
                         a0, a1, descv, nm, (uintptr_t)ret);
        }
    }
    return ret;
}

// ── Loaded-image ENUMERATE (sprite findings §1: FUN_140d69640 walks the loaded movie's image
// list [movie+0x40]+0x90, entries type +0x88==4) — the SAFE harvest path (only resident images,
// vs find-by-name which crashes on non-resident). Hook captures the movie ptr (arg0) on the engine
// thread; a one-time dump reverses the entry layout so we can then harvest name+rect+resource.
using enum_fn = void *(__fastcall *)(void *, void *, void *, void *);
enum_fn g_enum_orig = nullptr;
uintptr_t g_inv_movie = 0;

void harvest_resident_icons(uintptr_t movie);   // §8 walk-all (defined after find_detour/g_find_orig)
void harvest_repo_icons();                       // §8b repo-tree walk-all (RE-validated, see below)
void harvest_twin_map_icons(uintptr_t repo, uintptr_t er);  // §8c twin map repo+0xb0 (pin/grace sprites)
void cache_map_sprite_from_img(const char *nm, uintptr_t img);

// Map-point icon layout, resolved from the RESIDENT image repo (the RAM source — see the §8c twin
// walk / cache_map_sprite_from_img). MENU_MAP_<NN> → iconId NN (= WORLD_MAP_POINT_PARAM.iconId, RE
// windows_map_point_icon_layout_re_findings.md); MENU_MAP_ERR_*/Church/… stay name-keyed. rect =
// (x,y)+(w,h) on `sheet` (the ID3D12Resource); err = the SB_MapCursor_ERR sheet.
struct MapIconRect { int x, y, w, h; bool err; void *sheet; };
std::map<int, MapIconRect> g_map_icon_rects;          // iconId -> rect
std::map<std::string, MapIconRect> g_map_icon_named;  // full name -> rect
std::mutex g_map_icon_mtx;
void store_map_icon_rect(const char *nm, int x, int y, int w, int h, void *sheet)
{
    if (!nm || w <= 0 || h <= 0) return;
    MapIconRect r{x, y, w, h, std::strncmp(nm, "MENU_MAP_ERR", 12) == 0, sheet};
    std::lock_guard<std::mutex> lk(g_map_icon_mtx);
    g_map_icon_named[nm] = r;
    if (std::strncmp(nm, "MENU_MAP_", 9) == 0)
    {
        const char *p = nm + 9;                 // MENU_MAP_<NN> → iconId NN
        if (*p >= '0' && *p <= '9')
        {
            int iid = std::atoi(p);
            bool fresh = g_map_icon_rects.find(iid) == g_map_icon_rects.end();
            g_map_icon_rects[iid] = r;
            // Enumerate available map-point symbols so the category_gpu_iconId table can be
            // filled from real data (which iconId = which symbol). One line per new iconId.
            if (fresh && goblin::config::dumpIconTextures)
                spdlog::info("[MAPICON-AVAIL] iconId={} name={} rect=({},{} {}x{})", iid, nm, x, y, w, h);
        }
    }
}

void *__fastcall enum_detour(void *movie, void *a1, void *a2, void *a3)
{
    g_inv_movie = reinterpret_cast<uintptr_t>(movie);
    void *r = g_enum_orig(movie, a1, a2, a3);
    // Proactive harvest: walk THIS movie's resident image-list (sprite findings §8) instead of
    // waiting for the find hook to catch each icon as the player browses. enum_detour fires for
    // MANY movies (load screens etc.) — we must walk the CURRENT arg, not a stashed global, so the
    // inventory movie (many MENU_ItemIcon) actually gets processed. Engine thread, post-original.
    harvest_resident_icons(reinterpret_cast<uintptr_t>(movie));
    return r;
}

// ── HARVEST via the find hook (the SAFE path): the engine calls FUN_140d63c30(repo,&out,key) to
// resolve each LOADED image (during enumeration + menu draws) — so hooking it captures every
// RESIDENT item icon's name+rect+sheet, on the engine thread, no container-reversing, no risky
// self-initiated find (the engine only finds what's loaded → never the crashing non-resident path).
using find_fn2 = void *(__fastcall *)(void *, void **, const wchar_t *);
find_fn2 g_find_orig = nullptr;

// Harvested icon cache: iconId → {sheet ID3D12Resource, sub-rect, format}. Engine thread (find
// hook) writes; render thread (overlay) reads → mutex. The render thread CopyTextureRegions from
// these into our own SRV atlas.
std::mutex g_harvest_mtx;
std::unordered_map<int, goblin::ItemSprite> g_harvest;
uintptr_t g_icon_repo = 0;   // FUN_140d63c30 arg0 (repo) — stashed for the proactive §8 walk
// Discovered/lit grace sprite (RE e4b3f6a §6: SB_ERR_Grace_Morning_Color, world-map movie). Harvested
// by the SAME find hook when the open map draws a discovered grace → lets the overlay draw graces
// itself (discovered = this sprite, undiscovered = grey-tinted) and become the sole grace source.
goblin::ItemSprite g_grace_sprite{};
bool g_grace_locked = false;   // true once the canonical grace (MENU_MAP_01_Bonfire) is stored
std::vector<goblin::GraceCandidate> g_grace_cands;   // all grace candidates (dev F1 viewer)
// ERR dungeon-style grace (MENU_MAP_ERR_GraceUnderground). Valid iff ERR is installed (the sprite
// exists in the worldmap gfx). The renderer uses it for DUNGEON graces in place of the vanilla bonfire.
goblin::ItemSprite g_grace_dungeon_sprite{};

// Read a resolved CSTextureImage (`img` = the find fn's `out`) and cache its sub-rect + backing
// sheet resource + DXGI_FORMAT under the iconId parsed from a MENU_ItemIcon_<id> name. Shared by
// the find hook (browse-to-fill) and the §8 proactive walk. Read-only (RPM). Returns true if cached.
bool cache_icon_from_img(const char *nm, uintptr_t img)
{
    if (img < 0x10000 || std::strncmp(nm, "MENU_ItemIcon_", 14) != 0) return false;
    uintptr_t vt = 0; icon_rpm_ptr(img, vt);
    uintptr_t er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!(vt > er && vt - er == 0x2bb8910)) return false;   // CSTextureImage vtable guard
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    icon_rpm_i32(img + 0x74, x0); icon_rpm_i32(img + 0x78, y0);
    icon_rpm_i32(img + 0x7c, x1); icon_rpm_i32(img + 0x80, y1);
    uintptr_t rtex = 0, res = 0;
    icon_rpm_ptr(img + 0x10, rtex);
    if (rtex > 0x10000) icon_rpm_ptr(rtex + 0x70, res);
    if (res <= 0x10000) return false;
    int iconId = std::atoi(nm + 14);
    // Sheet dims/format live on the bound D3D12 resource, but the GPU texture binds LAZILY
    // (findings §2) — at find/walk time res+0x10/+0x30 may still be 0. Do NOT gate harvesting on
    // that (it dropped every not-yet-rendered icon → harvested=0). Cache the rect + sheet ptr now
    // (the ID3D12Resource* is persistent); the overlay reads the AUTHORITATIVE format/dims via
    // ID3D12Resource::GetDesc() at copy time (render thread, bound). RPM read here is best-effort
    // diagnostics only (correct once bound, else 0).
    int dim = 0, rw = 0, rh = 0, fmt = 0;
    icon_rpm_i32(res + 0x10, dim); icon_rpm_i32(res + 0x20, rw); icon_rpm_i32(res + 0x28, rh);
    if (dim == 3) icon_rpm_i32(res + 0x30, fmt);
    goblin::ItemSprite hs;
    hs.sheet = reinterpret_cast<void *>(res);
    hs.x0 = x0; hs.y0 = y0; hs.x1 = x1; hs.y1 = y1;
    hs.sheetW = static_cast<unsigned long long>(rw);
    hs.sheetH = static_cast<unsigned>(rh);
    hs.format = static_cast<unsigned>(fmt);
    hs.valid = true;
    std::lock_guard<std::mutex> lk(g_harvest_mtx);
    g_harvest[iconId] = hs;
    return true;
}

void *__fastcall find_detour(void *repo, void **out, const wchar_t *key)
{
    if (repo && !g_icon_repo) g_icon_repo = reinterpret_cast<uintptr_t>(repo);
    void *ret = g_find_orig(repo, out, key);
    if (goblin::config::dumpIconTextures && key)
    {
        char nm[48] = {0};
        for (int i = 0; i < 47; ++i) { wchar_t c = key[i]; if (!c) break; nm[i] = (c < 128) ? static_cast<char>(c) : '?'; }
        // DIAG: log EVERY resolved sprite key + rect (deduped) → reveals the map-point naming scheme.
        // If the key encodes the WorldMapPointParam iconId (numeric) we get iconId→rect for free; if
        // it's symbolic (MENU_MAP_01_Bonfire) the marker iconId→rect needs a pin-draw hook instead.
        {
            uintptr_t img2 = 0; icon_rpm_ptr(reinterpret_cast<uintptr_t>(out), img2);
            if (img2 < 0x10000) img2 = reinterpret_cast<uintptr_t>(ret);
            uintptr_t vt2 = 0; icon_rpm_ptr(img2, vt2);
            uintptr_t er2 = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
            static std::set<std::string> seen2;
            if (img2 > 0x10000 && vt2 > er2 && vt2 - er2 == 0x2bb8910 && nm[0] &&
                seen2.size() < 400 && seen2.insert(nm).second)
            {
                int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                icon_rpm_i32(img2 + 0x74, x0); icon_rpm_i32(img2 + 0x78, y0);
                icon_rpm_i32(img2 + 0x7c, x1); icon_rpm_i32(img2 + 0x80, y1);
                spdlog::info("[MAPICON] key='{}' rect=({},{})-({},{})", nm, x0, y0, x1, y1);
            }
        }
        if (nm[0] == 'M' && nm[1] == 'E' && nm[2] == 'N' && nm[3] == 'U')   // MENU_* item icons
        {
            uintptr_t img = 0; icon_rpm_ptr(reinterpret_cast<uintptr_t>(out), img);
            if (img < 0x10000) img = reinterpret_cast<uintptr_t>(ret);
            uintptr_t vt = 0; icon_rpm_ptr(img, vt);
            uintptr_t er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
            static std::set<std::string> seen;
            if (img > 0x10000 && vt > er && vt - er == 0x2bb8910 && seen.insert(nm).second)
            {
                int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                icon_rpm_i32(img + 0x74, x0); icon_rpm_i32(img + 0x78, y0);
                icon_rpm_i32(img + 0x7c, x1); icon_rpm_i32(img + 0x80, y1);
                uintptr_t rtex = 0, res = 0;
                icon_rpm_ptr(img + 0x10, rtex);
                if (rtex > 0x10000) icon_rpm_ptr(rtex + 0x70, res);
                spdlog::info("[ENUM2] '{}' img={:#x} rect=({},{})-({},{}) res={:#x}", nm, img, x0, y0, x1, y1, res);
                // Cache per-iconId for the overlay's CopyTextureRegion (MENU_ItemIcon_<id> only).
                cache_icon_from_img(nm, img);
                // P2b: one-time dump of the sheet resource's first 0x60 bytes (i32) to LOCATE the
                // DXGI_FORMAT field (a small int near W@0x20/H@0x28; e.g. 71=BC1,77=BC3,98=BC7,87=BGRA8).
                static bool fmt_dumped = false;
                if (!fmt_dumped && res > 0x10000)
                {
                    fmt_dumped = true;
                    for (int o = 0; o < 0x60; o += 4)
                    {
                        int fv = 0; icon_rpm_i32(res + o, fv);
                        spdlog::info("[ENUM2-FMT] res+0x{:02x} = {} (0x{:x})", o, fv, static_cast<unsigned>(fv));
                    }
                }
            }
        }
    }
    // §8b repo-tree harvest RELOCATED off this game-thread hook → goblin::background_harvest_tick()
    // on the worldmap_probe background poll (docs/rpm_walk_audit.md). find_detour still fires
    // constantly while a menu is open, so running the RPM walk here put Wine's per-RPM cost on the
    // engine thread; the walk is self-throttled + reads the repo from its static anchor, so the
    // background poll covers it without stalling the engine. We keep stashing g_icon_repo above as
    // the walk's fallback anchor + the dumpIconTextures diag's per-find cache.
    return ret;
}

// §8 proactive harvest (sprite findings §8, commit 6913ec4): walk the loaded movie's resident
// image-list in ONE pass and cache every MENU_ItemIcon_* (vs the find hook, which only catches the
// one icon the engine happens to resolve while the player browses). Layout (instruction-confirmed):
//   movie+0x40 -> res ; gate res+0x88 == 4 (image-list) ; res+0x90 -> list
//   list+0x78 u32 count ; list+0x80 -> arr (entry-ptr array, stride 8)
//   entry+0x18 = name DLWString (heap iff *(entry+0x30) >= 8, else inline) ; names are GFx symbols.
// Each name is RESIDENT by construction → re-resolving it via the find fn (g_find_orig, the original
// trampoline — does NOT re-enter find_detour) is SAFE (the non-resident crash constraint doesn't
// apply). Read-only walk; runs on the engine thread from enum_detour. Throttled (500ms).
void harvest_resident_icons(uintptr_t movie)
{
    if (!goblin::config::dumpIconTextures || movie < 0x10000 || !g_icon_repo || !g_find_orig)
        return;

    uintptr_t res = 0; icon_rpm_ptr(movie + 0x40, res);
    if (res < 0x10000) return;
    uintptr_t list = 0; icon_rpm_ptr(res + 0x90, list);
    if (list < 0x10000) return;
    int count = 0; icon_rpm_i32(list + 0x78, count);
    uintptr_t arr = 0; icon_rpm_ptr(list + 0x80, arr);
    if (count <= 0 || count > 100000 || arr < 0x10000) return;
    // NOTE: the static +0x88==4 type gate from the findings reads garbage live (0x44) → dropped;
    // we instead filter by entry NAME (only MENU_ItemIcon_* are cached) which is self-validating.

    // Process each distinct (movie,count) once — enum_detour fires every frame for many movies; this
    // dedup avoids re-walking + re-resolving an unchanged list. A grown list (new count) reprocesses.
    static std::set<uint64_t> s_done;
    uint64_t key = (static_cast<uint64_t>(movie) * 1000003u) ^ static_cast<uint64_t>(count);
    if (!s_done.insert(key).second) return;

    // Game-thread RPM walk (enum_detour) → keep it bench-visible so a Wine RPM-cost regression on
    // this dev-gated path shows in [BENCH] instead of being an invisible freeze (docs/rpm_walk_audit.md).
    GOBLIN_BENCH_QUIET("harvest.resident_walk");

    int loops = count < 4096 ? count : 4096;   // hard cap (a non-list movie could give a bogus count)
    int harvested = 0, names_menu = 0;
    for (int i = 0; i < loops; ++i)
    {
        uintptr_t entry = 0; icon_rpm_ptr(arr + static_cast<uintptr_t>(i) * 8, entry);
        if (entry < 0x10000) continue;
        uintptr_t len = 0; icon_rpm_ptr(entry + 0x30, len);          // name length (wchars)
        uintptr_t namep = (len >= 8) ? 0 : entry + 0x18;             // SSO inline vs heap
        if (len >= 8) icon_rpm_ptr(entry + 0x18, namep);
        if (namep < 0x1000) continue;

        wchar_t wbuf[80] = {0}; SIZE_T got = 0;                       // one RPM for the whole name
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(namep), wbuf,
                          sizeof(wbuf) - sizeof(wchar_t), &got);
        char nm[80]; int k = 0;
        for (; k < 79 && wbuf[k]; ++k) nm[k] = (wbuf[k] < 128) ? static_cast<char>(wbuf[k]) : '?';
        nm[k] = 0;
        if (std::strncmp(nm, "MENU_ItemIcon_", 14) != 0) continue;
        ++names_menu;
        int iconId = std::atoi(nm + 14);
        { std::lock_guard<std::mutex> lk(g_harvest_mtx); if (g_harvest.count(iconId)) continue; }

        // Resolve the resident image by name (safe; trampoline, not the detour) → read+cache.
        // The fn writes the image into *out; some paths return it instead.
        void *out = nullptr;
        void *ret = g_find_orig(reinterpret_cast<void *>(g_icon_repo), &out, wbuf);
        uintptr_t img = reinterpret_cast<uintptr_t>(out);
        if (img < 0x10000) img = reinterpret_cast<uintptr_t>(ret);
        if (cache_icon_from_img(nm, img)) ++harvested;
    }
    if (names_menu || harvested)
        spdlog::info("[ENUM-WALK] §8 movie={:#x} count={} MENU_ItemIcon_names={} harvested_new={}",
                     movie, count, names_menu, harvested);
}

// §8b PROACTIVE repo-walk harvest — SOLVED + validated live (163 icons / 938 images, 2026-06-22).
// See docs/re/windows_resident_icon_enumeration_re_findings.md. Walk the FD4 image-repo's by-name
// std::map (a red-black tree) directly and cache EVERY resident MENU_ItemIcon_* in one pass —
// movie-independent (the §8 movie-walk only saw load-screen movies on this build), reading the
// CSTextureImage* straight from node+0x50 (NO find-by-name re-resolve, so the §5 non-resident crash
// can't occur). Read-only RPM, runs on the engine thread from find_detour; reprocesses only when the
// resident image count (_Mysize) grows, so the steady-state cost is two RPM reads + a compare.
//   repo    = *(er+0x3d82510)  (DAT_143d82510; fallback to g_icon_repo stashed by find_detour)
//   _Myhead = *(repo+0x88) ; root = *(_Myhead+0x08) ; _Mysize = *(repo+0x90)
//   node: _Left+0x00 _Parent+0x08 _Right+0x10 _Isnil(u8)+0x19 ; key DLWString+0x28 (len+0x38,cap+0x40)
//         value = CS::CSTextureImage* @ node+0x50
void harvest_repo_icons()
{
    // Run whenever native icons are wanted — graces (grace_overlay) and item icons
    // (native_item_icons) BOTH consume the harvested rects/sprites this walk caches, plus the dev
    // dump flag. Previously gated dump-only, which forced users onto the laggy debug flag to get
    // GPU graces. The walk below bulk-reads each node (1 RPM) + is rate-throttled so Wine's
    // per-RPM cost doesn't stall on icon churn (the old per-field reads were the Linux freeze).
    if (!goblin::config::dumpIconTextures && !goblin::config::graceOverlay &&
        !goblin::config::nativeItemIcons)
        return;
    uintptr_t er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!er) return;

    uintptr_t repo = 0; icon_rpm_ptr(er + 0x3d82510, repo);  // the canonical static anchor
    if (repo < 0x10000) repo = g_icon_repo;                  // fallback: arg0 stashed by find_detour
    if (repo < 0x10000) return;

    uintptr_t head = 0; icon_rpm_ptr(repo + 0x88, head);     // _Myhead (nil sentinel == end())
    if (head < 0x10000) return;
    int size = 0; icon_rpm_i32(repo + 0x90, size);           // _Mysize (count of ALL resident images)

    // Re-walk on ANY change to the resident count, not just growth. _Mysize OSCILLATES as the engine
    // evicts+loads icons while navigating menus (live monitor: 1074<->1107 churn), so the old
    // "walk only when it grows past the peak" gate stopped re-walking after the first peak and missed
    // every icon that loaded below it. Walking on each change + the per-iconId dedup (g_harvest.count
    // below) accumulates the UNION of everything that ever loads into our permanent cache → far wider
    // browse-to-fill coverage. Cheap: only fires when the count actually changed, not every frame.
    static int s_last_size = -1;
    static DWORD s_last_walk = 0;
    if (size == s_last_size) return;
    // Wine RPM is ~expensive: in production (no dev dump) cap the re-walk rate so the _Mysize churn
    // (icons evict/load while navigating, e.g. 1074<->1107) can't trigger a walk-storm freeze. The
    // map symbols/graces we need load once and persist in our cache, so an occasional walk suffices.
    // (Skip without updating s_last_size so the next post-throttle change still triggers a walk.)
    if (!goblin::config::dumpIconTextures && (GetTickCount() - s_last_walk) < 400)
        return;
    s_last_size = size;
    s_last_walk = GetTickCount();
    // BENCH (quiet = aggregate only, no per-call log spam): this runs on the GAME thread (find_detour),
    // which the per-frame render bench never sees — so a future Linux RPM-walk regression shows up in
    // the [BENCH] report instead of being an invisible freeze. See [[overlay-rendered-markers]] #11.
    GOBLIN_BENCH_QUIET("harvest.repo_walk");

    uintptr_t root = 0; icon_rpm_ptr(head + 0x08, root);     // _Myhead._Parent
    if (root < 0x10000) return;

    // Stack DFS over _Left/_Right, skipping nil sentinels (_Isnil@+0x19 != 0). Tree height is
    // O(log n) so the frontier stays small; the vector grows if a build ever holds a huge repo.
    std::vector<uintptr_t> stack; stack.reserve(64); stack.push_back(root);
    int walked = 0, harvested = 0;
    while (!stack.empty())
    {
        if (walked > 200000) break;                          // runaway guard
        uintptr_t n = stack.back(); stack.pop_back();
        if (n < 0x10000) continue;
        // BULK-READ the node header in ONE RPM (was ~6 per-field reads/node = thousands of RPM/walk
        // = the Wine freeze). Layout: _Left@0x00 _Right@0x10 _Isnil@0x19 key-DLWString@0x28
        // (inline chars, or heap ptr iff cap>=8) cap@0x40 value(CSTextureImage*)@0x50.
        uint8_t nb[0x58]; SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(n), nb, sizeof(nb), &got) ||
            got != sizeof(nb) || nb[0x19])
            continue;                                        // read fail or nil sentinel
        ++walked;
        const uintptr_t l = *reinterpret_cast<uintptr_t *>(nb + 0x00);
        const uintptr_t r = *reinterpret_cast<uintptr_t *>(nb + 0x10);
        const uint64_t cap = *reinterpret_cast<uint64_t *>(nb + 0x40);
        const uintptr_t img = *reinterpret_cast<uintptr_t *>(nb + 0x50);

        char nm[80] = {0};
        if (cap < 8)                                         // inline name (rare for MENU_* — they're long)
        {
            const wchar_t *w = reinterpret_cast<const wchar_t *>(nb + 0x28);
            for (int k = 0; k < 7 && w[k]; ++k) nm[k] = (w[k] < 128) ? static_cast<char>(w[k]) : '?';
        }
        else                                                 // heap name → one extra RPM
        {
            uintptr_t namep = *reinterpret_cast<uintptr_t *>(nb + 0x28);
            if (namep >= 0x1000)
            {
                wchar_t wbuf[80] = {0}; SIZE_T g2 = 0;
                ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(namep), wbuf,
                                  sizeof(wbuf) - sizeof(wchar_t), &g2);
                for (int k = 0; k < 79 && wbuf[k]; ++k)
                    nm[k] = (wbuf[k] < 128) ? static_cast<char>(wbuf[k]) : '?';
            }
        }
        if (nm[0])
        {
            if (std::strncmp(nm, "MENU_ItemIcon_", 14) == 0)
            {
                int iconId = std::atoi(nm + 14);
                bool have;
                { std::lock_guard<std::mutex> lk(g_harvest_mtx); have = g_harvest.count(iconId) != 0; }
                if (!have && cache_icon_from_img(nm, img)) ++harvested;   // value = CSTextureImage*
            }
            // Map-point icons (MENU_MAP_<NN>/_ERR_*) live in THIS by-name tree (repo+0x80), not the
            // +0xb0 twin (which holds MENU_MapTile_*). Read their rect @img+0x74..0x80 + backing sheet
            // and cache iconId->rect for the overlay (RE windows_map_point_icon_layout_re_findings.md).
            else if (std::strncmp(nm, "MENU_MAP_", 9) == 0 && img >= 0x10000)
            {
                int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                icon_rpm_i32(img + 0x74, x0); icon_rpm_i32(img + 0x78, y0);
                icon_rpm_i32(img + 0x7c, x1); icon_rpm_i32(img + 0x80, y1);
                uintptr_t rtex = 0, res = 0;
                icon_rpm_ptr(img + 0x10, rtex);
                if (rtex >= 0x10000) icon_rpm_ptr(rtex + 0x70, res);
                if (x1 - x0 >= 2 && y1 - y0 >= 2 && res >= 0x10000)
                    store_map_icon_rect(nm, x0, y0, x1 - x0, y1 - y0, reinterpret_cast<void *>(res));
                // The overworld grace (MENU_MAP_01_Bonfire) + the ERR dungeon grace
                // (MENU_MAP_ERR_GraceUnderground) are the sprites the overlay draws. run_force_grace's
                // CreateImage lands them in THIS repo+0x80 tree, so route them to cache_map_sprite_from_img
                // (sets g_grace_sprite / g_grace_dungeon_sprite). NOT the SB_ERR_Grace_*_Color time-of-day
                // pin variant (that gave the wrong "morning grace"). Map symbols (boss etc) use the rect above.
                if (std::strcmp(nm, "MENU_MAP_01_Bonfire") == 0 ||
                    std::strcmp(nm, "MENU_MAP_ERR_GraceUnderground") == 0)
                    cache_map_sprite_from_img(nm, img);
            }
        }

        if (l >= 0x10000) stack.push_back(l);
        if (r >= 0x10000) stack.push_back(r);
    }
    if (harvested && goblin::config::dumpIconTextures)
        spdlog::info("[REPO-WALK] §8b images={} walked={} harvested_new={}", size, walked, harvested);

    harvest_twin_map_icons(repo, er);
}

// Build an ItemSprite from a CSTextureImage* (rect + backing sheet) and register it as a grace
// CANDIDATE — used by the twin-map walk for the WorldMapPoint pin sprites (MENU_MAP_*). Mirrors
// cache_icon_from_img but keyed by NAME (not iconId): rect @img+0x74.., sheet = rtex+0x70 (no DIM
// gate — GetDesc resolves it at copy time). If the name is the canonical grace, set g_grace_sprite.
void cache_map_sprite_from_img(const char *nm, uintptr_t img)
{
    if (img < 0x10000) return;
    uintptr_t vt = 0; icon_rpm_ptr(img, vt);
    uintptr_t er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!(vt > er && vt - er == 0x2bb8910)) return;     // CSTextureImage vtable guard
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    icon_rpm_i32(img + 0x74, x0); icon_rpm_i32(img + 0x78, y0);
    icon_rpm_i32(img + 0x7c, x1); icon_rpm_i32(img + 0x80, y1);
    uintptr_t rtex = 0, res = 0;
    icon_rpm_ptr(img + 0x10, rtex);
    if (rtex > 0x10000) icon_rpm_ptr(rtex + 0x70, res);
    if (res <= 0x10000) return;
    if (x1 - x0 < 2 || y1 - y0 < 2) return;             // need a real rect
    int rw = 0, rh = 0, fmt = 0;
    icon_rpm_i32(res + 0x20, rw); icon_rpm_i32(res + 0x28, rh); icon_rpm_i32(res + 0x30, fmt);
    goblin::ItemSprite hs;
    hs.sheet = reinterpret_cast<void *>(res);
    hs.x0 = x0; hs.y0 = y0; hs.x1 = x1; hs.y1 = y1;
    hs.sheetW = static_cast<unsigned long long>(rw);
    hs.sheetH = static_cast<unsigned>(rh);
    hs.format = static_cast<unsigned>(fmt);
    hs.valid = true;

    // Map-point icon layout from the RESIDENT repo (the RAM source — no sblytbnd decompress needed):
    // iconId/name -> rect (x0,y0,x1,y1 = left,top,right,bottom) + the backing sheet. Drives the overlay
    // MapPointProvider. RE windows_map_point_icon_layout_re_findings.md (§ repo walk).
    store_map_icon_rect(nm, x0, y0, x1 - x0, y1 - y0, reinterpret_cast<void *>(res));

    std::lock_guard<std::mutex> lk(g_harvest_mtx);
    bool dup = false;
    for (const auto &gc : g_grace_cands) if (gc.name == nm) { dup = true; break; }
    if (!dup && g_grace_cands.size() < 64)
    {
        goblin::GraceCandidate gc; gc.name = nm; gc.spr = hs;
        g_grace_cands.push_back(gc);
        spdlog::info("[TWIN-WALK] candidate '{}' rect=({},{})-({},{}) res={:#x} fmt={}",
                     nm, x0, y0, x1, y1, res, fmt);
    }
    // Overworld grace = the vanilla bonfire icon MENU_MAP_01_Bonfire (the mod's MENU_MAP_GOBLIN_Grace
    // doesn't exist → CreateImage returned 0). Dungeon grace = MENU_MAP_ERR_GraceUnderground. NOT the
    // SB_ERR_Grace_*_Color pin (time-of-day variant = the wrong "morning grace"); no generic fallback.
    if (std::strcmp(nm, "MENU_MAP_01_Bonfire") == 0)
    {
        g_grace_sprite = hs; g_grace_locked = true;
        spdlog::info("[GRACE-SPRITE] '{}' LOCKED overworld bonfire rect=({},{})-({},{}) res={:#x}",
                     nm, x0, y0, x1, y1, res);
    }
    else if (std::strcmp(nm, "MENU_MAP_ERR_GraceUnderground") == 0 && !g_grace_dungeon_sprite.valid)
    {
        g_grace_dungeon_sprite = hs;
        spdlog::info("[GRACE-SPRITE] '{}' dungeon rect=({},{})-({},{}) res={:#x}",
                     nm, x0, y0, x1, y1, res);
    }
}

// §8c TWIN-map walk — the WorldMapPoint PIN icons (the REAL grace) live in the repo's TWIN std::map
// at repo+0xb0 (head +0xb8, size +0xc0), keyed by the gfx sprite name (MENU_MAP_*), looked up by the
// icon widget FUN_14074bcc0 via FUN_140d63e50 — NOT in repo+0x80 (MENU_ItemIcon). We never walked it,
// which is why we only ever found the wrong SB_ERR_Grace native pin. Same RB-tree node shape as §8b.
void harvest_twin_map_icons(uintptr_t repo, uintptr_t er)
{
    (void)er;
    if (repo < 0x10000) return;
    uintptr_t head = 0; icon_rpm_ptr(repo + 0xb8, head);
    if (head < 0x10000) return;
    int size = 0; icon_rpm_i32(repo + 0xc0, size);
    static int s_last_twin = -1;
    static DWORD s_last_twin_walk = 0;
    if (size == s_last_twin) return;                    // re-walk only when the twin count changes
    if (!goblin::config::dumpIconTextures && (GetTickCount() - s_last_twin_walk) < 400)
        return;                                         // prod: cap re-walk rate (Wine RPM cost)
    s_last_twin = size;
    s_last_twin_walk = GetTickCount();
    GOBLIN_BENCH_QUIET("harvest.twin_walk");            // game-thread RPM walk → keep it bench-visible
    uintptr_t root = 0; icon_rpm_ptr(head + 0x08, root);
    if (root < 0x10000) return;

    std::vector<uintptr_t> stack; stack.reserve(64); stack.push_back(root);
    int walked = 0, found = 0;
    while (!stack.empty())
    {
        if (walked > 200000) break;
        uintptr_t n = stack.back(); stack.pop_back();
        if (n < 0x10000) continue;
        // BULK-READ the node header in ONE RPM (was ~6 per-field reads = the Wine freeze). Same RB-tree
        // node layout as the repo walk: _Left@0x00 _Right@0x10 _Isnil@0x19 key@0x28 cap@0x40 value@0x50.
        uint8_t nb[0x58]; SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(n), nb, sizeof(nb), &got) ||
            got != sizeof(nb) || nb[0x19])
            continue;
        ++walked;
        const uintptr_t l = *reinterpret_cast<uintptr_t *>(nb + 0x00);
        const uintptr_t r = *reinterpret_cast<uintptr_t *>(nb + 0x10);
        const uint64_t cap = *reinterpret_cast<uint64_t *>(nb + 0x40);
        const uintptr_t img = *reinterpret_cast<uintptr_t *>(nb + 0x50);
        char nm[96] = {0};
        if (cap < 8)
        {
            const wchar_t *w = reinterpret_cast<const wchar_t *>(nb + 0x28);
            for (int k = 0; k < 7 && w[k]; ++k) nm[k] = (w[k] < 128) ? static_cast<char>(w[k]) : '?';
        }
        else
        {
            uintptr_t namep = *reinterpret_cast<uintptr_t *>(nb + 0x28);
            if (namep >= 0x1000)
            {
                wchar_t wbuf[96] = {0}; SIZE_T g2 = 0;
                ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(namep), wbuf,
                                  sizeof(wbuf) - sizeof(wchar_t), &g2);
                for (int k = 0; k < 95 && wbuf[k]; ++k)
                    nm[k] = (wbuf[k] < 128) ? static_cast<char>(wbuf[k]) : '?';
            }
        }
        if (nm[0])
        {
            ++found;
            // Capture map-point sprites (MENU_MAP_*) and anything grace-ish into grace candidates.
            if (std::strncmp(nm, "MENU_MAP_", 9) == 0 || std::strstr(nm, "Grace"))
                cache_map_sprite_from_img(nm, img);
        }
        if (l >= 0x10000) stack.push_back(l);
        if (r >= 0x10000) stack.push_back(r);
    }
    if (goblin::config::dumpIconTextures)
        spdlog::info("[TWIN-WALK] repo+0xb0 size={} walked={} named={}", size, walked, found);
}
} // namespace

// Verify item↔iconId↔sprite: iterate the EquipParam* tables LIVE (get_param), read each row's
// iconId at its paramdef offset (u16), and log the rows whose iconId matches the sprites we
// captured from the inventory (MENU_FL_<iconId>). Proves the menu icon = EquipParam.iconId.
// iconId offsets (Ghidra+1, live-corrected): Weapon 0xC0, Protector iconIdM 0xA8,
// Accessory 0x28, Goods 0x32, Gem 0x06. row_id == the item-name FMG id.
void goblin::verify_equip_iconids()
{
    if (!goblin::config::dumpIconTextures)
        return;
    struct P { const wchar_t *name; const char *tag; int off; };
    // iconId offsets (u16) — SOLVED + cross-verified (2026-06-22): computed from the CURRENT
    // (SOTE) Paramdex (Nordgaren/Erd-Tools Defs-English) by tools/paramdef_iconid_offset.py AND
    // confirmed against the live [CALIB] high-distinct columns. The earlier 0xC0 "confirmation" was
    // DURABILITY (paramdef order: equipModelId@0xBC, iconId@0xBE, durability@0xC0; Dagger has both
    // iconId=100 AND durability=100 → coincidence). distinct=2 @ 0xC0 = durability default 100.
    //   Weapon iconId=0xBE (live distinct=599 range[20,45818]); Protector iconIdM=0xA6 / iconIdF=0xA8
    //   (distinct=760 range[1097,45808]); Accessory=0x26 (distinct=167/170); Goods=0x30
    //   (distinct=1573, contains the live-captured 40144); Gem=0x04 (distinct=609).
    static const P params[] = {
        {L"EquipParamWeapon", "Weapon", 0xBE},
        {L"EquipParamProtector", "Protector", 0xA6}, // iconIdM (iconIdF = 0xA8)
        {L"EquipParamAccessory", "Accessory", 0x26},
        {L"EquipParamGoods", "Goods", 0x30},
        {L"EquipParamGem", "Gem", 0x04},
    };
    static const uint16_t want[] = {40144, 40147, 40172}; // the 3 inventory-captured iconIds
    for (const P &p : params)
    {
        try
        {
            int n = 0, sample = 0; uint32_t mn = 0xffffffff, mx = 0; int hits = 0;
            uintptr_t prev_base = 0;
            for (auto row : from::params::get_param<uint8_t>(p.name))
            {
                uintptr_t base = reinterpret_cast<uintptr_t>(&row.second);
                uint16_t icon = *reinterpret_cast<uint16_t *>(base + p.off);
                ++n;
                if (icon < mn) mn = icon;
                if (icon > mx) mx = icon;
                if (sample < 8 && icon != 0)   // skip unused/system rows (iconId 0) so the sample
                                               // shows REAL iconIds at the new offset
                {
                    // [EQUIPADDR] = the absolute row-base + name + struct stride, for CE
                    // find-what-accesses: watch <base + iconId-offset> and trigger the menu
                    // icon draw to capture the compiled `movzx r,[reg+0xXX]` (XX = true offset).
                    std::string nm = goblin::lookup_text_utf8(static_cast<int>(row.first));
                    uintptr_t stride = prev_base ? (base - prev_base) : 0;
                    spdlog::info("[EQUIPADDR] {} row_id={} base={:#x} stride={:#x} u16@0x{:x}={} name='{}'",
                                 p.tag, row.first, base, stride, p.off, icon, nm);
                    ++sample;
                }
                prev_base = base;
                for (uint16_t w : want)
                    if (icon == w)
                    {
                        std::string nm = goblin::lookup_text_utf8(static_cast<int>(row.first));
                        spdlog::info("[EQUIPVERIFY] {} MATCH row_id={} iconId={} name='{}'", p.tag, row.first, icon, nm);
                        ++hits;
                    }
            }
            spdlog::info("[EQUIPVERIFY] {} rows={} iconId[min={},max={}] hits={}", p.tag, n, mn, mx, hits);
        }
        catch (...)
        {
            spdlog::info("[EQUIPVERIFY] {} not loaded", p.tag);
        }
    }
}

// Self-calibrating iconId-offset finder (RE §3, windows_live_paramdef_offset_re_findings).
// For each EquipParam, scan every u16 column across all rows and rank the offsets whose
// distribution looks like an iconId (many distinct, plausible range, ideally containing a
// known anchor). Zero offline / zero hardcoded offset / survives ER patches + mod swaps.
// Anchors: Weapon row 1000 (Dagger) = iconId 100; the live-captured inventory iconIds
// {40144,40147,40172} ([ICONMAP]) tag whichever param actually owns those items.
static void self_calibrate_iconid()
{
    if (!goblin::config::dumpIconTextures)
        return;
    struct P { const wchar_t *name; const char *tag; int known_off; };
    static const P params[] = {
        {L"EquipParamWeapon", "Weapon", 0xC0},
        {L"EquipParamProtector", "Protector", -1},
        {L"EquipParamAccessory", "Accessory", -1},
        {L"EquipParamGoods", "Goods", -1},
        {L"EquipParamGem", "Gem", -1},
    };
    static const uint16_t anchors[] = {40144, 40147, 40172};
    for (const P &p : params)
    {
        try
        {
            std::vector<uintptr_t> bases;
            for (auto row : from::params::get_param<uint8_t>(p.name))
                bases.push_back(reinterpret_cast<uintptr_t>(&row.second));
            if (bases.size() < 2)
            {
                spdlog::info("[CALIB] {} too few rows", p.tag);
                continue;
            }
            uintptr_t stride = bases[1] - bases[0];
            if (stride < 4 || stride > 0x2000)
            {
                spdlog::info("[CALIB] {} non-contiguous rows (stride={:#x}) — skip", p.tag, stride);
                continue;
            }
            int N = static_cast<int>(bases.size());
            // iconId is LOW-cardinality (icons reused across many items) but DENSE in a small
            // low band — unlike id/price columns (huge sparse range). Rank by density =
            // distinct/span; that's what separates iconId from the high-distinct id columns.
            struct Cand { int off, distinct, validPct, density; uint16_t mn, mx; bool anchor; };
            std::vector<Cand> cands;
            for (uintptr_t off = 0; off + 2 <= stride; ++off)
            {
                std::set<uint16_t> seen;
                int valid = 0; uint16_t mn = 0xffff, mx = 0; bool anc = false;
                for (uintptr_t b : bases)
                {
                    uint16_t v = *reinterpret_cast<uint16_t *>(b + off);
                    if (v >= 1 && v <= 65534) { ++valid; if (v < mn) mn = v; if (v > mx) mx = v; seen.insert(v); }
                    for (uint16_t a : anchors) if (v == a) anc = true;
                }
                int distinct = static_cast<int>(seen.size());
                int span = (mx >= mn) ? (mx - mn + 1) : 1;
                int density = distinct * 1000 / span;            // ×1000 to keep ints meaningful
                if (distinct < 20 || mx > 60000) continue;        // iconId: varied + bounded
                cands.push_back({(int)off, distinct, valid * 100 / N, density, mn, mx, anc});
            }
            std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) {
                if (a.anchor != b.anchor) return a.anchor;        // a captured iconId here = strong
                return a.density > b.density;                      // else densest low-band column
            });
            spdlog::info("[CALIB] {} N={} stride={:#x} knownOff=0x{:x} — ranked by density:",
                         p.tag, N, stride, p.known_off);
            for (int i = 0; i < (int)cands.size() && i < 6; ++i)
            {
                const Cand &c = cands[i];
                spdlog::info("[CALIB]   off=0x{:x} density={} distinct={} valid={}% range[{},{}]{}{}",
                             c.off, c.density, c.distinct, c.validPct, c.mn, c.mx,
                             c.anchor ? " <ANCHOR>" : "", c.off == p.known_off ? " <KNOWN>" : "");
            }
            // Also dump the KNOWN offset's stats verbatim (even if it didn't make top-6) so the
            // density heuristic can be calibrated against ground truth (Weapon 0xC0).
            if (p.known_off >= 0)
            {
                std::set<uint16_t> seen; uint16_t mn = 0xffff, mx = 0;
                for (uintptr_t b : bases)
                {
                    uint16_t v = *reinterpret_cast<uint16_t *>(b + p.known_off);
                    if (v >= 1 && v <= 65534) { if (v < mn) mn = v; if (v > mx) mx = v; seen.insert(v); }
                }
                int span = (mx >= mn) ? (mx - mn + 1) : 1;
                spdlog::info("[CALIB]   KNOWN off=0x{:x} density={} distinct={} range[{},{}]",
                             p.known_off, (int)seen.size() * 1000 / span, (int)seen.size(), mn, mx);
            }
        }
        catch (...) { spdlog::info("[CALIB] {} not loaded", p.tag); }
    }
}

goblin::ItemSprite goblin::resolve_item_sprite(int iconId)
{
    // Cache VALID resolves only (a miss may be "sheet not loaded yet" → retry later).
    static std::unordered_map<int, ItemSprite> cache;
    auto it = cache.find(iconId);
    if (it != cache.end()) return it->second;

    ItemSprite s;
    uintptr_t er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!er) return s;
    using FindFn = void *(__fastcall *)(void *, void **, const wchar_t *);  // FUN_140d63c30
    auto find = reinterpret_cast<FindFn>(er + 0xd63c30);
    void *repo = *reinterpret_cast<void **>(er + 0x3d82510);  // FD4 image repo singleton
    if (!repo) return s;                                       // menu not loaded → no sheets resident
    wchar_t key[40] = L"MENU_ItemIcon_00000";
    { int v = iconId; for (int i = 18; i >= 14; --i) { key[i] = static_cast<wchar_t>(L'0' + (v % 10)); v /= 10; } }
    void *out = nullptr;
    void *ret = find(repo, &out, key);
    uintptr_t img = reinterpret_cast<uintptr_t>(out ? out : ret);
    if (img < 0x10000) return s;
    uintptr_t vt = 0; icon_rpm_ptr(img, vt);
    if (!(vt > er && vt - er == 0x2bb8910)) return s;          // miss = sentinel (vt != CSTextureImage)
    icon_rpm_i32(img + 0x74, s.x0); icon_rpm_i32(img + 0x78, s.y0);
    icon_rpm_i32(img + 0x7c, s.x1); icon_rpm_i32(img + 0x80, s.y1);
    uintptr_t rtex = 0; icon_rpm_ptr(img + 0x10, rtex);        // Render::Texture (bound)
    if (rtex < 0x10000) return s;
    uintptr_t res = 0; icon_rpm_ptr(rtex + 0x70, res);         // ID3D12Resource
    if (res < 0x10000) return s;
    s.sheet = reinterpret_cast<void *>(res);
    // Read W/H from the vkd3d d3d12_resource INTERNAL struct DIRECTLY (proven: dim@+0x10=3,
    // W@+0x20, H@+0x28) — NOT via GetDesc (the foreign COM call crashed). The DXGI_FORMAT lives
    // at some internal offset (found by the probe's field dump, then read here once known).
    int w = 0, h = 0; icon_rpm_i32(res + 0x20, w); icon_rpm_i32(res + 0x28, h);
    s.sheetW = static_cast<unsigned long long>(w);
    s.sheetH = static_cast<unsigned int>(h);
    s.format = 0;  // TODO: read once the internal format offset is identified (probe dump)
    s.valid = true;
    cache[iconId] = s;
    return s;
}

bool goblin::harvested_icon(int iconId, ItemSprite &out)
{
    std::lock_guard<std::mutex> lk(g_harvest_mtx);
    auto it = g_harvest.find(iconId);
    if (it == g_harvest.end()) return false;
    out = it->second;
    return true;
}

size_t goblin::harvested_count()
{
    std::lock_guard<std::mutex> lk(g_harvest_mtx);
    return g_harvest.size();
}

// DEV force-load TEST — see header + findings §5b. Stream a file RESIDENT by FD4 path through the
// CSFile singleton. The loader (er+0x1f5560) is __fastcall load(CSFile, const wchar_t* path, 0, 0);
// it allocates a request, fills it from the path, and enqueues into the FD4 file/task system → an
// async load, so a bad path fails gracefully (returns null) rather than crashing. We log the handle +
// the harvested count before/after (a force-loaded icon sheet only adds repo entries once its gfx
// binds, so the count may not move immediately — the returned non-null handle is the primary signal).
void *goblin::force_load_file(const char *utf8_path)
{
    if (!goblin::config::dumpIconTextures || !utf8_path || !utf8_path[0]) return nullptr;
    uintptr_t er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!er) return nullptr;
    uintptr_t csfile = 0; icon_rpm_ptr(er + 0x3d5b0f8, csfile);   // CSFile FD4Singleton
    if (csfile < 0x10000) { spdlog::warn("[FORCELOAD] CSFile singleton null (not init yet)"); return nullptr; }

    wchar_t wpath[192] = {0};
    if (MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, wpath, 191) <= 0) return nullptr;

    size_t before; { std::lock_guard<std::mutex> lk(g_harvest_mtx); before = g_harvest.size(); }
    using LoadFn = void *(__fastcall *)(void *, const wchar_t *, void *, uint32_t);  // FUN_1401f5560
    auto load = reinterpret_cast<LoadFn>(er + 0x1f5560);
    void *res = load(reinterpret_cast<void *>(csfile), wpath, nullptr, 0);
    size_t after; { std::lock_guard<std::mutex> lk(g_harvest_mtx); after = g_harvest.size(); }
    spdlog::info("[FORCELOAD] load(CSFile={:#x}, '{}') -> {:#x}  harvested {}->{}",
                 csfile, utf8_path, reinterpret_cast<uintptr_t>(res), before, after);
    return res;
}

// ── DEV bind-flip TEST (RE findings §5e) ─────────────────────────────────────────────────────────
// Question: does setting a loaded resource-GROUP entry's +0x7c "needs-apply" flag trigger the binding
// (the apply vmethod resource+0xc8) that populates the repo with per-icon RECTS — i.e. can we force a
// non-resident item-icon group resident + bound on demand? We hook the residency ticker FUN_140d724c0
// to (a) capture the live menu-resource manager (*param_2 — it is reached via the task system, not a
// flat singleton) and (b) run a one-shot action INLINE on the engine thread BEFORE calling the
// original, so the original's per-tick group-apply (FUN_140d78540) consumes our flag THIS tick.
// All memory access is RPM/WPM (clang-cl elides __try around raw derefs); the by-groupId loaders are
// called directly (same as force_load_file). Gated by config::dumpIconTextures; driven from the P2b panel.
namespace
{
using res_tick_fn = void(__fastcall *)(uintptr_t, uintptr_t *);
res_tick_fn g_res_tick_orig = nullptr;
std::atomic<uintptr_t> g_res_mgr{0};
std::atomic<int> g_bind_action{0};   // 0 idle | 1 dump | 2 load files | 3 flip-bind | 4 load+flip
std::atomic<int> g_bind_gid{1};      // group id for the loaders (1 = 01_Common = the item-icon group)

// ── CreateImage force-bind (RE §5g) ──────────────────────────────────────────────────────────────
// CSScaleformImageCreator::CreateImage (FUN_140d6bbc0) is hooked above (create_image_detour, the
// existing probe hook) which captures the live context g_ci_p1/g_ci_p2. Here we replay it: build a
// synthetic symbol desc for "MENU_ItemIcon_<id>" and call the original via g_create_image_orig.
constexpr int CI_REPLAY_LAST = INT_MIN + 1;   // sentinel: replay the last captured live symbol
std::atomic<int> g_ci_req_icon{INT_MIN};   // queued iconId (INT_MIN = idle, CI_REPLAY_LAST = replay)
alignas(16) char g_ci_name[96];            // 4-aligned scratch for the synthesized symbol name
uintptr_t g_ci_desc[4] = {0};              // synthetic param_3: [0] = (g_ci_name - 0xc), rest 0 (pad)

inline bool res_w32(uintptr_t a, uint32_t v)
{
    SIZE_T n = 0;
    return a && WriteProcessMemory(GetCurrentProcess(), (void *)a, &v, 4, &n) && n == 4;
}

// Walk the manager's loaded-group array (base = align8(mgr+0x9d8), stride 0x10, count mgr+0xbe0; each
// slot holds a POINTER to a group entry). Log each entry's resource obj (+0x18), its apply vmethod
// (resource vtable +0xc8 as an RVA — THIS identifies what the bind does, cf. §5c parse fns), and the
// flags (+0x7c needs-apply, +0x80 applied). If `flip`, set +0x7c=1 to force a re-apply this tick.
void res_walk_groups(uintptr_t mgr, uintptr_t er, bool flip)
{
    uintptr_t count = 0;
    if (!icon_rpm_ptr(mgr + 0xbe0, count) || count == 0 || count > 4096)
    {
        spdlog::warn("[BINDTEST] implausible group count {} @ mgr={:#x} — aborting (wrong manager?)", count, mgr);
        return;
    }
    uintptr_t base = (mgr + 0x9d8 + 7) & ~uintptr_t(7);   // engine's (-(addr)&7) 8-byte pad
    // Game-thread per-node RPM walk (res_tick_detour, F1 BINDTEST) → keep it bench-visible
    // (docs/rpm_walk_audit.md).
    GOBLIN_BENCH_QUIET("bindtest.group_walk");
    int flipped = 0;
    for (uintptr_t i = 0; i < count; ++i)
    {
        uintptr_t entry = 0;
        if (!icon_rpm_ptr(base + i * 0x10, entry) || entry < 0x10000) continue;
        int f7c = 0, f80 = 0;
        icon_rpm_i32(entry + 0x7c, f7c);
        icon_rpm_i32(entry + 0x80, f80);
        uintptr_t res = 0, vt = 0, apply = 0;
        icon_rpm_ptr(entry + 0x18, res);
        if (res > 0x10000) { icon_rpm_ptr(res, vt); if (vt) icon_rpm_ptr(vt + 0xc8, apply); }
        spdlog::info("[BINDTEST] grp[{}] entry={:#x} res={:#x} apply(vt+0xc8)={:#x} RVA={:#x} +0x7c={} +0x80={}",
                     i, entry, res, apply, (apply > er) ? apply - er : 0, f7c, f80);
        if (flip && res_w32(entry + 0x7c, 1)) ++flipped;
    }
    if (flip)
    {
        res_w32(mgr + 0x1e19, 1);   // dirty flag (also drains the +0x1df0 load queue next tick)
        spdlog::info("[BINDTEST] flipped +0x7c on {} groups + set dirty +0x1e19 — orig FUN_140d78540 applies now", flipped);
    }
}

void run_bind_action(uintptr_t mgr, uintptr_t er, int act, int gid)
{
    spdlog::info("[BINDTEST] action={} gid={} mgr={:#x} harvested={} (before)", act, gid, mgr,
                 goblin::harvested_count());
    if ((act == 2 || act == 4) && gid >= 0 && gid <= 8)
    {
        // By-groupId loaders (FUN_140d77550 = TPF, FUN_140d771d0 = sblytbnd). UNLIKE force_load_file's
        // raw CSFile call, these cache the handle at mgr+0xd10/+0xd58+gid*8 — exactly where the bind's
        // apply vmethod looks for the loaded resource. Guarded internally (no-op if already loaded).
        reinterpret_cast<void(__fastcall *)(uintptr_t, uint8_t)>(er + 0xd77550)(mgr, (uint8_t)gid);
        reinterpret_cast<void(__fastcall *)(uintptr_t, uint8_t)>(er + 0xd771d0)(mgr, (uint8_t)gid);
        spdlog::info("[BINDTEST] called FUN_140d77550 + FUN_140d771d0 (mgr, gid={})", gid);
    }
    res_walk_groups(mgr, er, /*flip=*/(act == 3 || act == 4));
}

// Force-call CreateImage for "MENU_ItemIcon_<iconId>" (engine thread, via the ticker request). Builds a
// synthetic symbol desc: param_3 → a qword holding (nameAddr-0xc) so (*p3 & ~3)+0xc == nameAddr (the
// 4-aligned ASCII name). Calls the ORIGINAL via g_create_image_orig (the hook trampoline) with the
// captured creator context. Logs the returned image + whether the repo grew. Caveat (§5b): the
// "<name>_ptl" view resolves against a RESIDENT sheet — pair with "Load files (gid)" if not loaded.
void run_create_icon(uintptr_t er, int iconId)
{
    (void)er;
    if (!g_create_image_orig) { spdlog::warn("[CREATEIMG] no orig (probe hook missing)"); return; }
    if (iconId == CI_REPLAY_LAST)
    {
        if (!g_ci_last[0]) { spdlog::warn("[CREATEIMG] no live symbol captured yet"); return; }
        strncpy(g_ci_name, g_ci_last, sizeof(g_ci_name) - 1);   // replay a known-good symbol (control)
    }
    else
    {
        // GFx import scheme is "img://<name>" (live names showed img://KG_R1 etc.), base = MENU_ItemIcon_<id>.
        snprintf(g_ci_name, sizeof(g_ci_name), "img://MENU_ItemIcon_%d", iconId);
    }
    g_ci_desc[0] = reinterpret_cast<uintptr_t>(g_ci_name) - 0xc;   // (desc[0] & ~3)+0xc == g_ci_name
    size_t before = goblin::harvested_count();
    void *img = g_create_image_orig(g_ci_p1.load(), g_ci_p2.load(), g_ci_desc, nullptr, nullptr,
                                    nullptr, nullptr, nullptr);
    size_t after = goblin::harvested_count();
    spdlog::info("[CREATEIMG] force '{}' (p1={} p2={}) -> img={:#x}  harvested {}->{}", g_ci_name,
                 g_ci_p1.load(), g_ci_p2.load(), reinterpret_cast<uintptr_t>(img), before, after);
}

// Force the REAL grace icon (the mod's gfx sprite, NOT the native SB_ERR time-tinted pin). repo+0x80 =
// MENU_ItemIcon, repo+0xb0 = MENU_MapTile — neither holds the pin sprites, so we force-create the gfx
// img:// imports via the HOOKED CreateImage (the proven path: KG_/SB_ERR resolved). Each result lands
// in g_icon_imgs (via create_image_detour) → dump_icon_textures_live captures it as a grace candidate.
// MENU_MAP_GOBLIN_Grace = canonical; siblings populate the F1 picker. Engine thread; throttled by caller.
int g_grace_force_tries = 0;
std::atomic<bool> g_force_grace_req{false}; // manual F1 "Force graces now" (bypasses the auto cap)
void run_force_grace(uintptr_t er)
{
    if (!g_create_image_orig || g_ci_p1.load() == nullptr) return;
    static const char *kCands[] = {
        "img://MENU_MAP_01_Bonfire",            // the REAL vanilla grace icon (bonfire = grace)
        "img://MENU_MAP_GOBLIN_Grace", "img://MENU_MAP_GOBLIN_SortaGraceIDK",
        "img://MENU_MAP_ERR_GraceUnderground", "img://SB_ERR_Grace_Morning_Color",
    };
    auto CreateImageHooked = reinterpret_cast<create_image_fn>(er + 0xd6bbc0);
    for (const char *c : kCands)
    {
        snprintf(g_ci_name, sizeof(g_ci_name), "%s", c);
        g_ci_desc[0] = reinterpret_cast<uintptr_t>(g_ci_name) - 0xc;
        void *img = CreateImageHooked(g_ci_p1.load(), g_ci_p2.load(), g_ci_desc, nullptr, nullptr,
                                      nullptr, nullptr, nullptr);
        spdlog::info("[GRACE-FORCE] CreateImage('{}') -> {:#x} (try {})", c,
                     reinterpret_cast<uintptr_t>(img), g_grace_force_tries);
    }
}

void __fastcall res_tick_detour(uintptr_t p1, uintptr_t *p2)
{
    if (p2)
    {
        uintptr_t mgr = *p2;
        if (mgr > 0x10000)
        {
            g_res_mgr.store(mgr, std::memory_order_relaxed);
            uintptr_t er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
            int act = g_bind_action.exchange(0, std::memory_order_acq_rel);
            if (act) run_bind_action(mgr, er, act, g_bind_gid.load(std::memory_order_relaxed));
            int ci = g_ci_req_icon.exchange(INT_MIN, std::memory_order_acq_rel);
            if (ci != INT_MIN) run_create_icon(er, ci);
            // Auto force the canonical grace (Morning) for a time-independent live grace. Only while
            // the GPU-sprite grace is on, until it's canon-locked, context captured, throttled, capped.
            if (goblin::config::graceOverlay && goblin::config::graceGpuSprite && !g_grace_locked &&
                g_ci_p1.load(std::memory_order_relaxed) && g_grace_force_tries < 30)
            {
                static int s_gt = 0;
                if ((s_gt++ % 30) == 0) { ++g_grace_force_tries; run_force_grace(er); }
            }
            // Manual force (F1 "Force graces now") — re-poke on demand, bypassing the auto
            // cap/lock/throttle; only needs a captured CreateImage context (g_ci_p1). CreateImage
            // alone only makes the sprites RESIDENT — the candidates are registered by the twin-map
            // WALK (harvest_twin_map_icons → cache_map_sprite_from_img → g_grace_cands), so run it
            // right after (the missing "binding" step), else the F1 viewer shows no candidate.
            if (g_force_grace_req.exchange(false, std::memory_order_acq_rel) &&
                g_ci_p1.load(std::memory_order_relaxed))
            {
                run_force_grace(er);
                if (g_icon_repo)
                    harvest_twin_map_icons(g_icon_repo, er);
            }
        }
    }
    if (g_res_tick_orig) g_res_tick_orig(p1, p2);
}
}  // namespace

// Public driver (P2b panel). Queues a one-shot action consumed on the next residency tick (engine
// thread). action: 1=dump groups, 2=load files(gid), 3=flip-bind all loaded groups, 4=load+flip(gid).
// Returns false if the ticker hasn't captured the manager yet (open the inventory/map once).
bool goblin::bind_test(int action, int groupId)
{
    if (!goblin::config::dumpIconTextures) return false;
    if (g_res_mgr.load(std::memory_order_relaxed) == 0)
    {
        spdlog::warn("[BINDTEST] manager not captured yet — open the inventory or map once (ticker must run)");
        return false;
    }
    g_bind_gid.store(groupId, std::memory_order_relaxed);
    g_bind_action.store(action, std::memory_order_release);
    return true;
}

// Queue a force-CreateImage for "MENU_ItemIcon_<iconId>" — consumed on the next residency tick (engine
// thread). The CreateImage hook must have captured a live context first (open the inventory once).
bool goblin::force_create_icon(int iconId)
{
    if (!goblin::config::dumpIconTextures) return false;
    if (g_res_mgr.load(std::memory_order_relaxed) == 0)
    {
        spdlog::warn("[CREATEIMG] manager not captured yet — open the inventory/map once");
        return false;
    }
    g_ci_req_icon.store(iconId, std::memory_order_release);
    return true;
}

// Manually re-run the grace force-CreateImage (F1 dev button) — consumed on the next residency
// tick (engine thread), bypassing the auto cap/lock. Harvests the MENU_MAP_*/SB_ERR_Grace_*
// gfx-movie sprites into the candidate list. Returns false if the ticker/context isn't ready yet.
bool goblin::force_graces()
{
    if (!goblin::config::dumpIconTextures) return false;
    if (g_res_mgr.load(std::memory_order_relaxed) == 0)
    {
        spdlog::warn("[GRACE-FORCE] manager not captured yet — open the inventory/map once");
        return false;
    }
    g_force_grace_req.store(true, std::memory_order_release);
    return true;
}

// Control test: replay the LAST captured live symbol (a known-good img:// import) to prove the replay
// mechanism end-to-end. If this returns a valid img but force_create_icon(id) returns 0, the item-icon
// base simply isn't a CreateImage import (it's the widget's direct repo find / sblytbnd bind path).
bool goblin::force_create_last()
{
    if (!goblin::config::dumpIconTextures) return false;
    g_ci_req_icon.store(CI_REPLAY_LAST, std::memory_order_release);
    return true;
}

bool goblin::harvested_grace(ItemSprite &out)
{
    std::lock_guard<std::mutex> lk(g_harvest_mtx);
    if (!g_grace_sprite.valid) return false;
    out = g_grace_sprite;
    return true;
}

std::vector<goblin::GraceCandidate> goblin::grace_candidates()
{
    std::lock_guard<std::mutex> lk(g_harvest_mtx);
    return g_grace_cands;
}

// The ERR dungeon-style grace sprite (MENU_MAP_ERR_GraceUnderground). False until captured / if ERR
// isn't installed (sprite absent) → renderer falls back to the vanilla grace for dungeon graces too.
bool goblin::harvested_grace_dungeon(ItemSprite &out)
{
    std::lock_guard<std::mutex> lk(g_harvest_mtx);
    if (!g_grace_dungeon_sprite.valid) return false;
    out = g_grace_dungeon_sprite;
    return true;
}

// DEV (F1 grace-debug): set the active grace sprite from a captured candidate by index, so the
// overlay can test which name maps to the correct grace. Also locks it so the auto-capture won't
// override. Returns false on a bad index. The overlay must then force_rebuild_grace() to re-copy.
bool goblin::set_grace_from_candidate(size_t idx)
{
    std::lock_guard<std::mutex> lk(g_harvest_mtx);
    if (idx >= g_grace_cands.size() || !g_grace_cands[idx].spr.valid) return false;
    g_grace_sprite = g_grace_cands[idx].spr;
    g_grace_locked = true;
    spdlog::info("[GRACE-DBG] active grace set to candidate[{}] '{}'", idx, g_grace_cands[idx].name);
    return true;
}

std::vector<int> goblin::harvested_ids(size_t max)
{
    std::lock_guard<std::mutex> lk(g_harvest_mtx);
    std::vector<int> out;
    out.reserve(g_harvest.size() < max ? g_harvest.size() : max);
    for (const auto &kv : g_harvest)
    {
        if (out.size() >= max) break;
        out.push_back(kv.first);
    }
    return out;
}

void goblin::probe_icon_find_runtime()
{
    if (!goblin::config::dumpIconTextures)
        return;
    // ⛔ DISABLED: find-by-name (resolve_item_sprite) CRASHES DETERMINISTICALLY inside
    // FUN_140d63c30 (exe+0xD63E02, fault exe+0x591401F) when the queried icon's sheet is NOT fully
    // resident — for not-loaded ids it returns a clean miss, but for a partially-referenced id it
    // derefs an unready field and faults. clang-cl can't SEH-guard the foreign call. The earlier
    // [FIND2-TEX] successes were a lucky inventory state. → the SAFE path is enumerate-LOADED images
    // (sprite findings §1: FUN_140d69640 walks the loaded movie's image list — only resident images,
    // no risky by-name query) + integrate on the RENDER thread, not this hotkey thread. resolve_item_sprite
    // stays for resident-only use. See [[overlay-icon-atlas]].
    spdlog::warn("[FIND2] find-by-name probe DISABLED — crashes on non-resident sheets; "
                 "switch to enumerate-loaded (FUN_140d69640). See memory.");
}

// ── OodleLZ_Decompress hook (FD4 RAM-cache RE, windows_fd4_ram_dds_cache_re_prompt.md) ───────────
// The menu texture file 01_common.tpf.dcx is DCX/KRAK-compressed; the engine decompresses it via
// oo2core's OodleLZ_Decompress into a CPU buffer (the TPF, whose entries are the icon-sheet DDS).
// Hooking it logs that buffer's address + size at the moment of decompression — the candidate for a
// persistent CPU DDS cache (read it again menu-closed to test persistence). dst is in OUR address
// space (post-decompress) so we read it directly. 14-arg Oodle signature; we pass everything through.
using oodle_decompress_fn = long long(__fastcall *)(const void *, long long, void *, long long, int,
                                                    int, int, void *, long long, void *, void *,
                                                    void *, long long, int);
oodle_decompress_fn g_oodle_orig = nullptr;
// ER's decompressed TPF buffers are TRANSIENT (freed after it parses them), so we COPY each complete
// DDS out IN the hook (while valid) and ACCUMULATE distinct ones — a browser to find the icon sheet.
std::mutex g_tpf_mtx;
std::vector<std::vector<uint8_t>> g_dds_list; // all distinct complete DDS captured from decompresses

long long __fastcall oodle_decompress_detour(const void *src, long long srcLen, void *dst,
                                             long long dstLen, int a5, int a6, int a7, void *a8,
                                             long long a9, void *a10, void *a11, void *a12,
                                             long long a13, int a14)
{
    long long r = g_oodle_orig(src, srcLen, dst, dstLen, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    if (goblin::config::dumpIconTextures && dst && r >= 0x10000)
    {
        const uint8_t *b = reinterpret_cast<const uint8_t *>(dst);
        // Decompressed menu texture container = a TPF ("TPF\0"). Log its RAM address + size so we can
        // re-read it menu-closed (persistence test) and parse DDS+rects out of it.
        if (b[0] == 'T' && b[1] == 'P' && b[2] == 'F' && b[3] == 0 && r >= 0x20)
        {
            uint32_t fc = 0, doff = 0, dsz = 0;
            std::memcpy(&fc, b + 0x08, 4);
            std::memcpy(&doff, b + 0x10, 4);
            std::memcpy(&dsz, b + 0x14, 4);
            bool dataIsDDS = (size_t)doff + 4 < (size_t)r && b[doff] == 'D' && b[doff + 1] == 'D' &&
                             b[doff + 2] == 'S';
            // COPY the DDS out NOW (the buffer is freed once ER parses it). Only when the whole DDS
            // fits in THIS decompressed block (doff+dsz <= r) — small icon TPFs are self-contained;
            // multi-block sheets need block accumulation (not yet). Our copy is persistent → uploadable
            // anytime, no game bind, no read-after-free.
            if (dataIsDDS && (size_t)doff + dsz <= (size_t)r && dsz >= 148)
            {
                const uint8_t *d = b + doff;
                std::lock_guard<std::mutex> lk(g_tpf_mtx);
                if (g_dds_list.size() < 64)
                {
                    bool dup = false;
                    for (auto &e : g_dds_list)
                        if (e.size() == dsz && std::memcmp(e.data(), d, 32) == 0) { dup = true; break; }
                    if (!dup)
                        g_dds_list.emplace_back(d, d + dsz);
                }
            }
            static int n = 0;
            if (n < 16)
            {
                ++n;
                spdlog::info("[OODLE-TPF] @{:#x} sz={} files={} plat={} enc={} entry0(off={} size={} fmt={}) dataIsDDS={}",
                             reinterpret_cast<uintptr_t>(dst), r, fc, b[0x0C], b[0x0E], doff, dsz, b[0x18], dataIsDDS);
            }
        }
    }
    return r;
}

// IAT hook: overwrite eldenring.exe's import-table pointer for dll!fn with `detour`, saving the
// original into *orig. No trampoline/executable alloc (unlike MinHook → no MH_ERROR_MEMORY_ALLOC
// when oo2core sits >2GB away), and works for a cross-module import. Returns false if the module
// doesn't statically import dll!fn (e.g. it's resolved dynamically via GetProcAddress).
bool iat_hook(uintptr_t modBase, const char *dll, const char *fn, void *detour, void **orig)
{
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(modBase);
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(modBase + dos->e_lfanew);
    auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return false;
    auto *desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(modBase + dir.VirtualAddress);
    for (; desc->Name; ++desc)
    {
        const char *name = reinterpret_cast<const char *>(modBase + desc->Name);
        if (_stricmp(name, dll) != 0)
            continue;
        auto *thunk = reinterpret_cast<IMAGE_THUNK_DATA *>(modBase + desc->FirstThunk);
        DWORD origRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        auto *oThunk = reinterpret_cast<IMAGE_THUNK_DATA *>(modBase + origRva);
        for (; oThunk->u1.AddressOfData; ++thunk, ++oThunk)
        {
            if (oThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)
                continue;
            auto *ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(modBase + oThunk->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char *>(ibn->Name), fn) != 0)
                continue;
            void **slot = reinterpret_cast<void **>(&thunk->u1.Function);
            DWORD oldp = 0;
            if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &oldp))
                return false;
            *orig = *slot;
            *slot = detour;
            VirtualProtect(slot, sizeof(void *), oldp, &oldp);
            return true;
        }
    }
    return false;
}

void install_oodle_hook()
{
    if (!goblin::config::dumpIconTextures || g_oodle_orig)
        return;
    uintptr_t er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!er)
        return;
    if (iat_hook(er, "oo2core_6_win64.dll", "OodleLZ_Decompress",
                 reinterpret_cast<void *>(&oodle_decompress_detour),
                 reinterpret_cast<void **>(&g_oodle_orig)))
        spdlog::info("[OODLE] IAT-hooked OodleLZ_Decompress (orig={}) — open the world map to log the TPF buffer",
                     reinterpret_cast<void *>(g_oodle_orig));
    else
        spdlog::warn("[OODLE] eldenring.exe has no static IAT import of oo2core!OodleLZ_Decompress "
                     "(dynamically loaded?) — decompress hook not installed");
}

// Browser over the distinct DDS captured from ER's decompresses (grabbed in the Oodle hook). count +
// fetch-by-index; the overlay uploads each into its own texture to find the icon sheet visually.
size_t goblin::tpf_dds_count()
{
    std::lock_guard<std::mutex> lk(g_tpf_mtx);
    return g_dds_list.size();
}

bool goblin::tpf_dds_at(size_t i, std::vector<uint8_t> &out)
{
    std::lock_guard<std::mutex> lk(g_tpf_mtx);
    if (i >= g_dds_list.size())
        return false;
    out = g_dds_list[i];
    return true;
}

// Map-point icon rect lookup (resolved from the resident image repo; see store_map_icon_rect, filled
// by the repo walk for MENU_MAP_*). Returns the SubTexture rect for a WORLD_MAP_POINT_PARAM.iconId on
// the SB_MapCursor[_ERR] sheet. false if not captured yet (open the world map) or the id is absent.
size_t goblin::map_icon_layout_count()
{
    std::lock_guard<std::mutex> lk(g_map_icon_mtx);
    return g_map_icon_rects.size() + g_map_icon_named.size();
}

bool goblin::map_icon_rect(int iconId, int &x, int &y, int &w, int &h, void *&sheet)
{
    std::lock_guard<std::mutex> lk(g_map_icon_mtx);
    auto it = g_map_icon_rects.find(iconId);
    if (it == g_map_icon_rects.end()) return false;
    x = it->second.x; y = it->second.y; w = it->second.w; h = it->second.h; sheet = it->second.sheet;
    return true;
}

bool goblin::map_icon_rect_by_name(const char *name, int &x, int &y, int &w, int &h, void *&sheet)
{
    if (!name) return false;
    std::lock_guard<std::mutex> lk(g_map_icon_mtx);
    auto it = g_map_icon_named.find(name);
    if (it == g_map_icon_named.end()) return false;
    x = it->second.x; y = it->second.y; w = it->second.w; h = it->second.h; sheet = it->second.sheet;
    return true;
}

// Background harvest entry point — see goblin_inject.hpp. Relocates the proactive repo walk
// off the game-thread find hook onto the worldmap_probe background poll: harvest_repo_icons
// is read-only RPM on our own process and publishes under its mutexes, so it is thread-safe,
// and its internal throttle keeps the per-RPM Wine cost off the cadence of the engine thread.
void goblin::background_harvest_tick()
{
    harvest_repo_icons();   // resolves the repo from the static anchor; no-op unless icons wanted
}

void goblin::install_icon_texture_probe()
{
    // The find/enum + residency-ticker + CreateImage hooks feed the PRODUCTION native-icon paths
    // (harvest_repo_icons → grace sprites + map_icon_rect rects), so install them whenever graces
    // or native item icons are on — NOT only under the dev dump flag (that was the gate that left
    // GPU graces/boss icons blank without dump_icon_textures). Their heavy work is internally
    // dump-gated; the light hook path + the throttled bulk-read walk are cheap enough for runtime.
    const bool dev = goblin::config::dumpIconTextures;
    const bool native = goblin::config::graceOverlay || goblin::config::nativeItemIcons;
    if (!dev && !native)
        return;
    // DEV-ONLY heavy machinery: Oodle DDS-capture hook + iconId calibration/verification.
    if (dev)
    {
        install_oodle_hook();
        goblin::verify_equip_iconids();
        self_calibrate_iconid();
    }
    void *fn = modutils::scan<void>({.aob = goblin::sig::WORLDMAP_CREATE_IMAGE});
    if (!fn)
    {
        spdlog::warn("[ICONTEX] CreateImage AOB not found (game patch?) — icon probe disabled");
        return;
    }
    try
    {
        modutils::hook(fn, reinterpret_cast<void *>(&create_image_detour),
                       reinterpret_cast<void **>(&g_create_image_orig));
        spdlog::info("[ICONTEX] CreateImage hooked @ {} (probe)", fn);
    }
    catch (const std::exception &e)
    {
        spdlog::error("[ICONTEX] hook failed: {}", e.what());
        g_create_image_orig = nullptr;
    }

    // Enumerate-loaded hook: capture the inventory movie ptr + reverse its image-list layout (safe
    // harvest path; sprite findings §1). Hooked by RVA (FUN_140d69640) — dev probe.
    try
    {
        uintptr_t er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
        void *enumfn = reinterpret_cast<void *>(er + 0xd69640);
        modutils::hook(enumfn, reinterpret_cast<void *>(&enum_detour),
                       reinterpret_cast<void **>(&g_enum_orig));
        void *findfn = reinterpret_cast<void *>(er + 0xd63c30);
        modutils::hook(findfn, reinterpret_cast<void *>(&find_detour),
                       reinterpret_cast<void **>(&g_find_orig));
        spdlog::info("[ENUM2] find-hook (FUN_140d63c30) @ {} + enum @ {} — open inventory to harvest "
                     "resident item icons", findfn, enumfn);
    }
    catch (const std::exception &e)
    {
        spdlog::error("[ENUM] hook failed: {}", e.what());
        g_enum_orig = nullptr;
    }

    // Residency-ticker hook (findings §5e): capture the live menu-resource manager (*param_2) for the
    // bind-flip test + run queued one-shot actions inline on the engine thread. Idle cost = one deref
    // + one atomic load per tick. Dev-gated (whole probe is behind config::dumpIconTextures).
    try
    {
        uintptr_t er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
        void *tickfn = reinterpret_cast<void *>(er + 0xd724c0);   // FUN_140d724c0
        modutils::hook(tickfn, reinterpret_cast<void *>(&res_tick_detour),
                       reinterpret_cast<void **>(&g_res_tick_orig));
        spdlog::info("[BINDTEST] residency-ticker hooked @ {} — open inventory/map to capture manager", tickfn);
    }
    catch (const std::exception &e)
    {
        spdlog::error("[BINDTEST] ticker hook failed: {}", e.what());
        g_res_tick_orig = nullptr;
    }
    // CreateImage context capture + force-replay (§5g) reuses the existing CreateImage probe hook
    // above (create_image_detour) — no separate hook (MinHook can't double-hook the same fn).
}

// Path A verify step (findings §6): on a MAP-OPEN frame, re-read each registered image's
// img+0x10 (the lazily-bound Scaleform::Render::Texture) → BFS to the GXTexture2D → +0x40 =
// ID3D12Resource. Confirms the load-time-null texture is now bound + reachable. Runs once
// per session after the chain first resolves. Called from the worldmap probe loop (map-open
// detector). Read-only RPM (reading the already-bound pointer is thread-safe; we do NOT call
// GetTexture, which would have to be on the render thread).
void goblin::dump_icon_textures_live()
{
    if (!goblin::config::dumpIconTextures || g_icon_imgs.empty())
        return;
    // REPEATABLE (throttled): images bind LAZILY + g_icon_imgs grows as the map draws more sprites,
    // so re-resolve periodically to accumulate ALL SB_ERR_*/icons (the old one-shot caught only what
    // was resident at the first run — e.g. a single grace frame). Candidate capture + logs dedup by
    // name; the verbose [ICONTEX-LIVE] summary stays one-shot (g_icon_live_done).
    static std::chrono::steady_clock::time_point s_last{};
    auto now = std::chrono::steady_clock::now();
    if (g_icon_live_done && now - s_last < std::chrono::milliseconds(500))
        return;
    s_last = now;
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!base)
        return;
    int bound = 0, resolved = 0, logged = 0;
    for (const IconImg &it : g_icon_imgs)
    {
        // ── Resolve the BOUND GPU sheet (binds LAZILY on first render). On vkd3d/Proton far fewer
        // sprites bind than on native D3D12, so this often fails on Linux even for valid sprites.
        // We DO NOT `continue` on failure: the candidate LIST is registered below GPU-independently
        // (fix: the list used to drop 3-4 candidates on Linux because only the 1-2 sprites that
        // happened to bind ever reached the push_back). Only the DISPLAY paths require sheet_ok.
        const uintptr_t mod_lo = base, mod_hi = base + 0x6000000;
        uintptr_t rtex = 0, res = 0, resvt = 0;
        int dim = 0, rw = 0, rh = 0, gfmt = 0;
        bool sheet_ok = false;
        // A REAL Scaleform Render::Texture = heap object (OUTSIDE the ER module) whose vtable is
        // INSIDE the module (.rdata). img+0x10 also takes junk states: 0 (unbound), a small int
        // (0x6), or a CODE/module pointer (0x141xxxxxx) — reject all three.
        if (icon_rpm_ptr(it.img + 0x10, rtex) && rtex >= 0x10000 && rtex < 0x7fffffffffffULL &&
            !(rtex >= mod_lo && rtex < mod_hi))
        {
            uintptr_t rtex_vt = 0;
            if (icon_rpm_ptr(rtex, rtex_vt) && rtex_vt >= mod_lo && rtex_vt < mod_hi)
            {
                ++bound;
                icon_detail_dump(rtex, base); // one-shot full Render::Texture layout (first bound)
                // rtex+0x70 = the backing ID3D12Resource. Do NOT gate on the RPM-read DIM at res+0x10:
                // the vkd3d resource layout drifted (a driver/game update; res+0x10 no longer reads 3) —
                // but res IS the resource; just validate a non-module vtable (a real COM object). The
                // overlay reads the authoritative format/dims via ID3D12Resource::GetDesc() at copy time.
                if (icon_rpm_ptr(rtex + 0x70, res) && res >= 0x10000 && res < 0x7fffffffffffULL &&
                    icon_rpm_ptr(res, resvt) && !(resvt >= mod_lo && resvt < mod_hi))
                {
                    icon_rpm_i32(res + 0x10, dim); icon_rpm_i32(res + 0x20, rw);
                    icon_rpm_i32(res + 0x28, rh); icon_rpm_i32(res + 0x30, gfmt);
                    ++resolved;
                    sheet_ok = true; // a bound, displayable GPU texture
                }
            }
        }
        // ERR map sprites (RE e4b3f6a §6): the discovered grace + all other ERR map icons draw through
        // CreateImage (not the find fn) → captured here with resolved sheet+rect. Collect EVERY
        // 'SB_ERR_*' candidate (dev viewer — to test whether non-grace icons harvest cleanly vs the
        // grace's time-of-day variants), and LOCK the grace sprite on the first LIT '..._Color' frame
        // with a grace-sized rect (any time-of-day; the engine resolves only one per session).
        if (it.name.rfind("SB_ERR_", 0) == 0 || it.name.rfind("MENU_MAP_", 0) == 0)
        {
            // The REAL grace = the mod's gfx sprite MENU_MAP_GOBLIN_Grace (force-created via img://);
            // SB_ERR_Grace_*_Color is the native time-tinted pin (wrong). Lock on the canonical; an
            // SB_ERR_Grace _Color frame is kept only as an overridable fallback. (MENU_MAP_ sprites have
            // no time-of-day variants, so they're always "lit".)
            // Canonical (default) grace = the vanilla icon MENU_MAP_01_Bonfire (bonfire = grace). The
            // ERR dungeon-style grace MENU_MAP_ERR_GraceUnderground is captured separately (below) and
            // used for dungeon graces when ERR is installed. GOBLIN_Grace/SB_ERR_Grace = F1 candidates.
            bool canon = it.name.rfind("MENU_MAP_01_Bonfire", 0) == 0;
            bool is_grace = canon || it.name.rfind("MENU_MAP_GOBLIN_Grace", 0) == 0 ||
                            it.name.rfind("SB_ERR_Grace", 0) == 0;
            bool lit = it.name.rfind("MENU_MAP_", 0) == 0 || it.name.find("Color") != std::string::npos;
            int gw = it.x1 - it.x0, gh = it.y1 - it.y0;
            bool rect_ok = gw >= 8 && gw <= 256 && gh >= 8 && gh <= 256;
            std::lock_guard<std::mutex> lk(g_harvest_mtx);
            // Candidate LIST is GPU-INDEPENDENT — register by name+rect whether or not the sheet is
            // bound, so vkd3d/Proton (which binds far fewer sprites) lists the SAME candidates as
            // native D3D12. Find-or-add by name; when the sheet later binds (sheet_ok), UPGRADE the
            // stored candidate in place so the F1 picker can actually DISPLAY it. (Sprites the game
            // never draws — e.g. the vanilla Bonfire on ERR — stay listed but never become displayable;
            // that's the inherent GPU limitation, not a list bug.)
            goblin::GraceCandidate *cand = nullptr;
            for (auto &gc : g_grace_cands) if (gc.name == it.name) { cand = &gc; break; }
            bool is_new = (cand == nullptr);
            if (is_new && g_grace_cands.size() < 64)
            {
                g_grace_cands.push_back(goblin::GraceCandidate{});
                cand = &g_grace_cands.back();
                cand->name = it.name;
                cand->spr.x0 = it.x0; cand->spr.y0 = it.y0; cand->spr.x1 = it.x1; cand->spr.y1 = it.y1;
                cand->spr.valid = false;   // listed-only until its texture binds
            }
            if (cand && sheet_ok && !cand->spr.valid)   // bound now → resolve sheet so it's displayable
            {
                cand->spr.sheet = reinterpret_cast<void *>(res);
                cand->spr.x0 = it.x0; cand->spr.y0 = it.y0; cand->spr.x1 = it.x1; cand->spr.y1 = it.y1;
                cand->spr.sheetW = static_cast<unsigned long long>(rw);
                cand->spr.sheetH = static_cast<unsigned>(rh);
                cand->spr.format = static_cast<unsigned>(gfmt);
                cand->spr.valid = true;
            }
            if (is_new)
                spdlog::info("[ICON-CAND] '{}' rect=({},{})-({},{}) res={:#x} fmt={} {}{}",
                             it.name, it.x0, it.y0, it.x1, it.y1, res, gfmt,
                             sheet_ok ? "bound" : "LISTED(unbound)", canon ? " <-CANONICAL grace" : "");
            // ── DISPLAY paths below require a BOUND sheet (only rendered sprites have a GPU texture
            // to copy). Keeping these gated on sheet_ok is what preserves the Windows display while the
            // list above is decoupled — the reverted attempt broke display by lazy-resolving these too.
            if (sheet_ok && is_grace && lit && rect_ok && !g_grace_locked && (canon || !g_grace_sprite.valid))
            {
                g_grace_sprite.sheet = reinterpret_cast<void *>(res);
                g_grace_sprite.x0 = it.x0; g_grace_sprite.y0 = it.y0;
                g_grace_sprite.x1 = it.x1; g_grace_sprite.y1 = it.y1;
                g_grace_sprite.sheetW = static_cast<unsigned long long>(rw);
                g_grace_sprite.sheetH = static_cast<unsigned>(rh);
                g_grace_sprite.format = static_cast<unsigned>(gfmt);
                g_grace_sprite.valid = true;
                if (canon) g_grace_locked = true;   // canonical wins + locks; fallback stays overridable
                spdlog::info("[GRACE-SPRITE] '{}' rect=({},{})-({},{}) {}x{} res={:#x} fmt={} ({})",
                             it.name, it.x0, it.y0, it.x1, it.y1, gw, gh, res, gfmt,
                             canon ? "LOCKED canonical MENU_MAP_01_Bonfire" : "fallback (awaiting canonical)");
            }
            // ERR dungeon-style grace — captured separately; its presence = ERR installed. The renderer
            // uses it for dungeon graces. Store once (it doesn't change).
            if (sheet_ok && it.name.rfind("MENU_MAP_ERR_GraceUnderground", 0) == 0 && rect_ok &&
                !g_grace_dungeon_sprite.valid)
            {
                g_grace_dungeon_sprite.sheet = reinterpret_cast<void *>(res);
                g_grace_dungeon_sprite.x0 = it.x0; g_grace_dungeon_sprite.y0 = it.y0;
                g_grace_dungeon_sprite.x1 = it.x1; g_grace_dungeon_sprite.y1 = it.y1;
                g_grace_dungeon_sprite.sheetW = static_cast<unsigned long long>(rw);
                g_grace_dungeon_sprite.sheetH = static_cast<unsigned>(rh);
                g_grace_dungeon_sprite.format = static_cast<unsigned>(gfmt);
                g_grace_dungeon_sprite.valid = true;
                spdlog::info("[GRACE-SPRITE] DUNGEON (ERR) '{}' rect=({},{})-({},{}) res={:#x} stored",
                             it.name, it.x0, it.y0, it.x1, it.y1, res);
            }
        }
        if (!sheet_ok)
            continue;   // unbound entry: candidate was listed above; the sheet tracking below needs res
        // Track unique sheet resources (icons on one sheet share a resource).
        bool known = false;
        for (uintptr_t s : g_icon_sheets) if (s == res) { known = true; break; }
        if (!known && g_icon_sheets.size() < 32)
        {
            g_icon_sheets.push_back(res);
            spdlog::info("[ICONTEX-LIVE] SHEET #{} res={:#x} resVt={:#x} {}x{} (DIM={})",
                         g_icon_sheets.size(), res, resvt, rw, rh, dim);
        }
        if (!g_icon_live_done && logged++ < 16)   // verbose only on the first pass (re-runs are quiet)
            spdlog::info("[ICONTEX-LIVE] name='{}' rect=({},{})-({},{}) sheet={}x{} | res={:#x} {}x{}",
                         it.name, it.x0, it.y0, it.x1, it.y1, it.w, it.h, res, rw, rh);
    }
    if (bound > 0)
    {
        spdlog::info("[ICONTEX-LIVE] summary: {}/{} bound, {} resolved via rtex+0x70, "
                     "{} unique sheet resources",
                     bound, g_icon_imgs.size(), resolved, g_icon_sheets.size());
        // Deterministic hop is solid once several images resolve to a small sheet set.
        if (resolved >= 5)
            g_icon_live_done = true;
    }
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

// Quest-aware NPC gating. LIVE: the refresh loop reads config every tick and
// parks/restores accordingly (disabling restores via the was_enabled edge), so
// no restart needed. Persisted by Save (quest_npc_quest_aware is a Bool entry).
bool goblin::ui::quest_aware() { return goblin::config::questNpcQuestAware; }
void goblin::ui::set_quest_aware(bool on) { goblin::config::questNpcQuestAware = on; }

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

static bool orp_flag_set(uint32_t flag_id)
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

// ── Live-loot: hide loot markers on the LIVE item-lot pickup flag ──────
// Resolve a lot-backed loot marker's LIVE pickup flag from ItemLotParam (same
// lookup as refresh_loot_from_itemlot below), so the overlay map can detect
// collected loot WITHOUT the native injection running. ERR/Randomizer reassign
// loot flags, so the baked textDisableFlagId1 is often stale — the live
// getItemFlagId is authoritative. Returns baked_flag when the row isn't lot-backed
// or the lot can't be resolved (graceful fallback). Always on (the old live_loot_flags
// flag, which gated the NATIVE map's param rewrite, was removed in Phase 2b); this is a
// read-only resolve for the overlay's collected-detection, which must work regardless
// (else ERR-remapped loot like Golden Runes never registers as taken).
// The LotReader is cached after first use (params are loaded by map-open time).
uint32_t goblin::resolve_loot_flag(uint32_t lotId, uint8_t lotType, uint32_t baked_flag)
{
    if (lotType == 0 || lotId == 0)
        return baked_flag;
    static LotReader s_lots;
    static bool s_init = false, s_ok = false;
    if (!s_init) { s_init = true; s_lots.init(); s_ok = s_lots.ok(); }
    if (!s_ok)
        return baked_flag;
    RawItemLotRow *row = s_lots.row(lotId, lotType);
    if (!row)
        return baked_flag;
    uint32_t flag = *reinterpret_cast<uint32_t *>(row->b + 0x80);  // lot-wide getItemFlagId
    if (flag == 0)
    {
        // Single-item lots only (else a slot-1 flag would mark the whole row taken
        // while other loot remains) — mirrors refresh_loot_from_itemlot.
        int32_t item2 = *reinterpret_cast<int32_t *>(row->b + 0x04);
        if (item2 == 0)
            flag = *reinterpret_cast<uint32_t *>(row->b + 0x60);  // getItemFlagId01
    }
    uint32_t resolved = flag ? flag : baked_flag;
    // Non-persistent / repeatable flags have NO save-backed "obtained" bit, so they
    // read false forever (IsEventFlag: a flag whose group isn't in the persistent
    // manager returns false — the temp/event-local groups never are). -1 = "always
    // re-droppable" (Paramdex); >=2^30 = the empirical temp range (golden runes /
    // crafting / consumables — repeatable loot). Treat these as NOT a tracked
    // collectible: return 0 so the caller skips the marker for graying + census
    // (instead of counting it perpetually "remaining"). This is the dominant cause of
    // the 100%-save over-report. See docs/re/windows_collected_loot_flag_re_findings.md.
    if (resolved == 0xFFFFFFFFu || resolved >= 0x40000000u)
        return 0;
    return resolved;
}

// Resolve a lot-backed marker's IDENTITY (name/icon key) from the LIVE ItemLotParam row, so the
// marker shows the item ERR/randomizer actually placed instead of the baked vanilla one. The bake
// stores the offset-encoded textId1 at extraction time; ERR can swap the lot's item → that bake
// drifts. We read the live row's slot-1 item (lotItemId01 @ +0x00, lotItemCategory01 @ +0x20 —
// same row resolve_loot_flag already reads: flag01@0x60, lot flag@0x80) and re-encode it via the
// shared encode_live_item(). Returns the baked key on any miss (no lot, no row, empty/invalid
// slot-1, or a multi-item lot where slot-1 doesn't represent the marker). The encoded key feeds
// both the marker label (FMG via liveLootLabels' PlaceName preload) and item_icon_id().
int32_t goblin::resolve_loot_item_textid(uint32_t lotId, uint8_t lotType, int32_t baked_textid)
{
    if (lotType == 0 || lotId == 0)
        return baked_textid;
    static LotReader s_lots;
    static bool s_init = false, s_ok = false;
    if (!s_init) { s_init = true; s_lots.init(); s_ok = s_lots.ok(); }
    if (!s_ok)
        return baked_textid;
    RawItemLotRow *row = s_lots.row(lotId, lotType);
    if (!row)
        return baked_textid;
    const int32_t item1 = *reinterpret_cast<int32_t *>(row->b + 0x00);  // lotItemId01
    const int32_t cat1  = *reinterpret_cast<int32_t *>(row->b + 0x20);  // lotItemCategory01
    if (item1 <= 0 || cat1 < 1 || cat1 > 5)
        return baked_textid;
    const int32_t live = encode_live_item(item1, cat1);
    return live ? live : baked_textid;
}

// One-shot field dump to RE the real per-item "obtained" flag (see the findings doc).
// For up to ~6 markers per category, log every candidate flag and its live SET/unset
// state. Run on a 100% save: the candidate that reads SET for a known-collected item is
// the correct field; if NONE is set for whole categories, those lots carry no readable
// persistent obtained flag (repeatable, or the real flag isn't in the lot/map data).
void goblin::diag_loot_flags(uint32_t lotId, uint8_t lotType, uint32_t baked, int category,
                             uint32_t nameId)
{
    if (!goblin::config::diagLootFlags) return;
    static int per_cat[NUM_CATEGORIES] = {0};
    if (category < 0 || category >= NUM_CATEGORIES || per_cat[category] >= 6) return;
    auto setstr = [](uint32_t f) -> const char * {
        return f ? (orp_flag_set(f) ? "(SET)" : "(unset)") : "";
    };
    if (lotType == 0 || lotId == 0)
    {
        spdlog::info("[LOOTDIAG] cat {:2} name {} NO-LOT baked={}{}", category, nameId, baked,
                     setstr(baked));
        per_cat[category]++;
        return;
    }
    static LotReader s_diag;
    static bool s_init = false, s_ok = false;
    if (!s_init) { s_init = true; s_diag.init(); s_ok = s_diag.ok(); }
    RawItemLotRow *row = s_ok ? s_diag.row(lotId, lotType) : nullptr;
    if (!row)
    {
        spdlog::info("[LOOTDIAG] cat {:2} name {} lot {}/{} NO-ROW baked={}{}", category, nameId,
                     lotId, lotType, baked, setstr(baked));
        per_cat[category]++;
        return;
    }
    int32_t item2 = *reinterpret_cast<int32_t *>(row->b + 0x04);
    uint32_t lotwide = *reinterpret_cast<uint32_t *>(row->b + 0x80);
    std::string slots;
    for (int i = 0; i < 8; ++i)
    {
        uint32_t f = *reinterpret_cast<uint32_t *>(row->b + 0x60 + i * 4);
        slots += std::to_string(f);
        slots += setstr(f);
        slots += ' ';
    }
    spdlog::info("[LOOTDIAG] cat {:2} name {} lot {}/{} item2={} | @0x80={}{} | baked={}{} | "
                 "slots60: {}",
                 category, nameId, lotId, lotType, item2, lotwide, setstr(lotwide), baked,
                 setstr(baked), slots);
    per_cat[category]++;
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
