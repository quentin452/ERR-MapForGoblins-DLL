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
#include "goblin_logic.hpp"
#include "goblin_markers.hpp"
#include "goblin_bench.hpp"
#include "from/params.hpp"
#include "from/paramdef/WORLD_MAP_POINT_PARAM_ST.hpp"
#include "from/paramdef/WORLD_MAP_PIECE_PARAM_ST.hpp"
#include "goblin_quest_gates.hpp"
#include "goblin_quest_steps.hpp"
#include "goblin_logic.hpp"

#include <algorithm>
#include <atomic>
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

    // Prefer an EXACT (src_area, src_gx, src_gz) base-point — its local coords share
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
        // nearest grace's region only when no MapNameOverride volume contains the marker.
        int region = region_name_pname(area, gridX, gridZ, posX, posZ);
        *out_pname = region ? region : (has_grace ? pname : -1);
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
    // Try the LIVE WorldMapPointParam first (rows with the grace icon). NOTE: vanilla
    // WorldMapPointParam does NOT contain grace markers — in the base game grace map-pins
    // are generated from BonfireWarpParam at runtime, not stored here. The project AUTHORS
    // the grace rows from MASSEDIT (positions extracted offline from MSB / BonfireWarpParam)
    // and injects them. So pre-injection this finds ~0; we fall back to the baked set, which
    // IS that offline-extracted game data. (A true live path would read BonfireWarpParam +
    // resolve each bonfire's MSB position — deferred, see [[live-param-vs-baked-data]].)
    try
    {
        for (auto [rowId, row] :
             from::params::get_param<from::paramdef::WORLD_MAP_POINT_PARAM_ST>(L"WorldMapPointParam"))
        {
            if (row.iconId != 370) continue;   // Site of Grace icon (ERR profile)
            g_live_graces.push_back({ row.areaNo, row.gridXNo, row.gridZNo,
                                      row.posX, row.posZ, row.textId1, rowId,
                                      (int)row.textDisableFlagId1 });
        }
    }
    catch (...) {}

    if (g_live_graces.empty())
    {
        for (size_t i = 0; i < goblin::generated::MAP_ENTRY_COUNT; ++i)
        {
            const auto &e = goblin::generated::MAP_ENTRIES[i];
            if (e.category != goblin::generated::Category::WorldGraces) continue;
            g_live_graces.push_back({ e.data.areaNo, e.data.gridXNo, e.data.gridZNo,
                                      e.data.posX, e.data.posZ, e.data.textId1, e.row_id,
                                      (int)e.data.textDisableFlagId1 });
        }
        spdlog::info("[LIVE-GRACE] live param has no grace pins (vanilla WMP lacks them) → "
                     "using {} baked graces (offline MSB/BonfireWarp extraction)",
                     g_live_graces.size());
    }
    else
        spdlog::info("[LIVE-GRACE] {} grace rows from live WorldMapPointParam", g_live_graces.size());
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

// ── Live world-map icon refresh (EXPERIMENTAL, config::liveRefreshWorldMap) ──
//
// The engine (re)builds the placed map-point icons in FUN_140a82a80(PointMan, ctx)
// (Windows RE, docs/windows_re_live_refresh_capture.md): it news CSWorldMapPointIns
// (ctor 0xa811e0), evaluates the per-row show-predicate (vtable+0x8), and inserts/
// updates/removes the std::map<int id, CSWorldMapPointIns*> at PointMan+0x398. Our
// section/category hide flips WORLD_MAP_POINT_PARAM_ST.areaNo to 99 on the live
// param blob — correct, but INVISIBLE while the map is open, because the per-frame
// reconcile (FUN_140a832a0) only UPDATES already-built icons; it never re-evaluates
// visibility on the +0x398 set. Only the build above does, and it runs off a
// transient per-frame context (ctx = {+0x34 page filter, +0x48 ParamRepo*}) that we
// must NOT fabricate — a bogus ctx would crash.
//
// So we HOOK the build fn and passively record the engine's own (this, ctx) as it
// is called naturally. When a refresh is requested (after an areaNo edit, map open)
// the detour re-invokes the ORIGINAL once more with that captured real pair — a
// second add/remove reconcile against the freshly-edited params. This keeps ALL
// game-state mutation on the engine's own thread; the watcher thread only sets a
// flag. Off unless config::liveRefreshWorldMap; the user runtime-verifies that the
// build is invoked while the map is open and that the extra pass refreshes icons
// with no crash (doc "Runtime test plan").
namespace
{
using world_map_build_fn = void(__fastcall *)(void *pointman, void *ctx);
world_map_build_fn g_wm_build_orig = nullptr;     // MinHook trampoline (relocated original)
std::atomic<bool>  g_wm_refresh_request{false};

// Detour over FUN_140a82a80. POD-only body (no C++ unwinding) so the __try is legal.
// The trampoline is the relocated original — calling it does NOT re-enter this
// detour, so the extra pass cannot recurse.
void __fastcall wm_build_detour(void *pointman, void *ctx)
{
    g_wm_build_orig(pointman, ctx);
    if (g_wm_refresh_request.exchange(false))
    {
        __try { g_wm_build_orig(pointman, ctx); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}
} // namespace

void goblin::install_live_refresh_hook()
{
    if (!goblin::config::liveRefreshWorldMap) return;
    // Entry AOB of FUN_140a82a80 (verified UNIQUE; no RIP-relative bytes, so the raw
    // prologue is patch-resilient). this=rcx (PointMan), ctx=rdx ({+0x34,+0x48}).
    void *fn = modutils::scan<void>({
        .aob = goblin::sig::WORLDMAP_POINT_CTOR,
    });
    if (!fn)
    {
        spdlog::warn("[LIVE-REFRESH] build-fn AOB not found (game patch?) — live refresh disabled");
        return;
    }
    try
    {
        modutils::hook(fn, reinterpret_cast<void *>(&wm_build_detour),
                       reinterpret_cast<void **>(&g_wm_build_orig));
        spdlog::info("[LIVE-REFRESH] world-map build hooked @ {} (experimental)", fn);
    }
    catch (const std::exception &e)
    {
        spdlog::error("[LIVE-REFRESH] hook failed: {}", e.what());
        g_wm_build_orig = nullptr;
    }
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

void icon_log_image(uintptr_t img, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
    if (!goblin::config::dumpIconTextures || !img)
        return;
    static int logged = 0;
    if (logged > 40)
        return;
    logged++;
    // Rect SOLVED: x0,y0,x1,y1 contiguous at +0x74/+0x78/+0x7c/+0x80; dims +0x84/+0x88.
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0, w = 0, h = 0;
    icon_rpm_i32(img + 0x74, x0); icon_rpm_i32(img + 0x78, y0);
    icon_rpm_i32(img + 0x7c, x1); icon_rpm_i32(img + 0x80, y1);
    icon_rpm_i32(img + 0x84, w);  icon_rpm_i32(img + 0x88, h);
    // Locate the backing texture: dump every heap ptr field 0x08..0xF0 with its vtable RVA
    // (vt − er_base), so we can ID the texture object even if the findings' vtable RVAs drifted.
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    char buf[480]; int len = 0; buf[0] = 0;
    for (int o = 0x08; o <= 0xF0; o += 8)
    {
        uintptr_t p = 0;
        if (!icon_rpm_ptr(img + o, p) || p < 0x10000 || p >= 0x7fffffffffffULL) continue;
        uintptr_t vt = 0;
        long rva = -1;
        if (icon_rpm_ptr(p, vt) && vt > base && vt < base + 0x6000000) rva = (long)(vt - base);
        len += std::snprintf(buf + len, sizeof(buf) - len, " +%x=%llx[vt=%lx]", o,
                             (unsigned long long)p, rva);
        if (len > (int)sizeof(buf) - 40) break;
    }
    spdlog::info("[ICONTEX] img={:#x} rect=({},{})-({},{}) sheet={}x{} | ptrs:{}",
                 img, x0, y0, x1, y1, w, h, buf);
}

void *__fastcall create_image_detour(void *a0, void *a1, void *a2, void *a3, void *a4, void *a5,
                                     void *a6, void *a7)
{
    void *ret = g_create_image_orig(a0, a1, a2, a3, a4, a5, a6, a7);
    icon_log_image((uintptr_t)ret, (uintptr_t)a1, (uintptr_t)a2, (uintptr_t)a3);
    return ret;
}
} // namespace

void goblin::install_icon_texture_probe()
{
    if (!goblin::config::dumpIconTextures)
        return;
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
}

void goblin::refresh_world_map_icons()
{
    if (!g_wm_build_orig) return;           // hook not installed (flag off / AOB miss)
    if (!goblin::world_map_open()) return;  // only meaningful while the 2D map is up
    g_wm_refresh_request.store(true);       // engine thread replays on its next build
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
// or the lot can't be resolved (graceful fallback). NOT gated on
// config::liveLootFlags: that flag governs whether the NATIVE map rewrites its param
// rows; this is a read-only resolve for the overlay's collected-detection, which must
// work regardless (else ERR-remapped loot like Golden Runes never registers as taken).
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
