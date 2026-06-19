#include "goblin_inject.hpp"
#include "goblin_collected.hpp"
#include "goblin_kindling.hpp"
#include "goblin_config.hpp"
#include "goblin_messages.hpp"
#include "modutils.hpp"
#include "goblin_map_data.hpp"
#include "goblin_item_icons.hpp"
#include "goblin_location_alt.hpp"
#include "goblin_grace_anchors.hpp"
#include "goblin_region_anchors.hpp"
#include "goblin_major_regions.hpp"
#include "goblin_tile_tabs.hpp"
#include "goblin_legacy_conv.hpp"
#include "goblin_markers.hpp"
#include "goblin_bench.hpp"
#include "from/params.hpp"
#include "from/paramdef/WORLD_MAP_POINT_PARAM_ST.hpp"
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
    float *out_ent_x = nullptr, float *out_ent_z = nullptr)
{
    if (d->areaNo == 60 || d->areaNo == 61)
        return false;
    // Underground worlds that have their OWN world-map page must NOT be projected
    // onto the overworld — there is no overworld surface spot for them, so they
    // land "in the sea". Keep them on their native page instead:
    //   area 12      = Siofra/Ainsel/Deeproot/Nokron/Mohgwyn/Lake of Rot
    //   area 40-43   = DLC underground caverns
    // (Catacombs/caves 30-39 and legacy dungeons 10/11/13/14… DO have overworld
    //  entrances, so they keep projecting.)
    if (d->areaNo == 12 || (d->areaNo >= 40 && d->areaNo <= 43))
        return false;

    // Prefer an exact (src_area, src_gx) base-point — its local coords share an
    // origin with this row, so we can keep the in-dungeon offset. Some dungeons
    // (e.g. Fringefolk Hero's Grave m10_01) have NO conv entry of their own in
    // WorldMapLegacyConvParam; fall back to any base-point of the same src_area
    // and cluster the rows at that overworld point (entrance) — visible, if
    // without intra-dungeon spread.
    const goblin::generated::LegacyConvEntry *exact = nullptr;
    const goblin::generated::LegacyConvEntry *area_fb = nullptr;
    for (size_t i = 0; i < goblin::generated::LEGACY_CONV_COUNT; ++i)
    {
        const auto &c = goblin::generated::LEGACY_CONV[i];
        if (c.src_area != d->areaNo)
            continue;
        if (!area_fb)
            area_fb = &c;
        if (c.src_gx == d->gridXNo)
        {
            exact = &c;
            break;
        }
    }
    const auto *c = exact ? exact : area_fb;
    if (!c)
        return false;

    // Exact match: keep the in-dungeon offset. Fallback: cluster at the base
    // point (mixing local coords across grids would misplace, so we don't).
    float wx = static_cast<float>(c->dst_gx) * 256.0f + c->dst_pos_x;
    float wz = static_cast<float>(c->dst_gz) * 256.0f + c->dst_pos_z;
    // The conv base point IS the dungeon's overworld ENTRANCE — hand it back so a
    // cluster of this dungeon's markers can sit there instead of at the centroid
    // of their spread-out projected interior (which can drift off into the sea).
    if (out_ent_x) *out_ent_x = wx;
    if (out_ent_z) *out_ent_z = wz;
    if (exact)
    {
        wx += d->posX - c->src_pos_x;
        wz += d->posZ - c->src_pos_z;
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

// State for runtime toggle (ERSC-hosting workaround). On hotkey press the
// pointers stored on the param-res-cap are swapped between vanilla and
// expanded values. set_param_injection_active() is a no-op until
// inject_map_entries() has populated these.
static uint8_t **g_file_ptr_ref = nullptr;
static int64_t *g_file_size_ref = nullptr;
static uint8_t *g_vanilla_param_file = nullptr;
static int64_t g_vanilla_param_size = 0;
static uint8_t *g_expanded_param_file = nullptr;
static int64_t g_expanded_param_size = 0;
static bool g_param_injection_active = false;

// Data pointers of MFG-injected WorldMapPointParam rows in the expanded table.
// Used by sanitize_injected_textids() (run after the FMG is built) to strip
// textIds that don't resolve to a real string.
static std::vector<uint8_t *> g_injected_row_ptrs;

// Live-loot: lot-backed injected rows. refresh_loot_from_itemlot() reads the
// LIVE ItemLotParam getItemFlagId for each and rewrites textDisableFlagId1 so
// the marker hides on the actual light-point pickup for the loaded regulation
// (Randomizer-compatible). g_lot_backed_set lets apply_flag_or_pairs skip them.
struct LotBackedRow { uint8_t *ptr; uint32_t lotId; uint8_t lotType; };
static std::vector<LotBackedRow> g_lot_backed_rows;
static std::set<uint8_t *> g_lot_backed_set;

// Row IDs of Leyndell Ashen Capital (m35) markers — gated on StoryErdtreeOnFire
// by apply_map_logic so they only appear once the Erdtree has burned.
static std::set<uint64_t> g_ashen_rows;

// Data pointers of Leyndell Royal Capital (m11_00, areaNo 11) markers — the
// INVERSE of the ashen gate: hidden (areaNo 99) once StoryErdtreeOnFire sets,
// because the Royal Capital is consumed by the Ashen Capital after the burn.
// Park-only (burn is permanent → never restore); refresh_royal_eviction()
// applies it after fragment-eviction so it wins. Clustered royal rows are NOT
// registered (the cluster owns their areaNo).
static std::vector<uint8_t *> g_royal_rows;

// Thread 1 v1.5 — quest-aware quest-NPC gating. Each registered WorldQuestNPC row
// carries its NPC's quest-active flags (from goblin_quest_gates, joined from the
// MIT EldenRingQuestLog data). refresh_quest_npc_eviction parks it (areaNo 99)
// while NONE of those flags is set (quest not active) and restores it when active.
// Opt-in (config questNpcQuestAware); coordinates with section/category via
// is_section_hidden_ptr on restore. Clustered rows excluded.
struct QuestRow { uint8_t *ptr; uint8_t orig_area; uint32_t flags[4]; };
static std::vector<QuestRow> g_quest_rows;

static const goblin::generated::QuestGate *lookup_quest_gate(uint32_t nameId)
{
    const auto *a = goblin::generated::QUEST_GATES;
    size_t lo = 0, hi = goblin::generated::QUEST_GATE_COUNT;
    while (lo < hi) { size_t m = (lo + hi) / 2;
        if (a[m].nameId < nameId) lo = m + 1; else hi = m; }
    if (lo < goblin::generated::QUEST_GATE_COUNT && a[lo].nameId == nameId)
        return &a[lo];
    return nullptr;
}

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
struct SectionRow
{
    uint8_t *ptr;       // → row data in the live expanded blob
    Section  sec;
    Category cat;       // fine-grained gate (the 63 show_* categories)
    uint8_t  orig_area; // areaNo to restore on show
    bool     is_piece;
    bool     is_kindling;
    uint64_t row_id;
    int      grp_key;   // cluster grouping key: nearest-grace index, or entrance key, or -1 = homeless
    int      grp_pname; // PlaceName id for the pile label (-1 = count-only)
    int      grp_tab;   // anchor map sub-page (tabId) — DIAGNOSTIC ONLY (underground 12000/12001/...)
    float    ent_x;     // projected-dungeon overworld ENTRANCE (world coords); <0 = not a projected dungeon
    float    ent_z;
};
static std::vector<SectionRow> g_section_rows;

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
// Cluster state (declared before the apply_* visibility fns, which gate clusters).
// A collectable member of a cluster, with every signal needed to tell if it's been
// taken: the textDisableFlagId1 event flag (plain loot), or piece/kindling tracking
// (Reforged Rune Pieces / Kindling Spirits — collected by row id, no event flag).
struct ClusterMemberRef { uint32_t flag; uint64_t row_id; bool is_piece; bool is_kindling; };
struct ClusterRow { uint8_t *ptr; uint8_t area; int count_textid; std::vector<ClusterMemberRef> members; Category cat; };
static std::vector<ClusterRow> g_clusters;        // the synthetic cluster icons
struct ClusterMember { uint8_t *ptr; uint8_t orig_area; Category cat; };
static std::vector<ClusterMember> g_cluster_members;  // individuals parked under a cluster

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
static bool g_clustering_active = false;  // clusters built this session

// A cluster icon (and its parked members) belong to ONE category since buckets
// are per-category. Hide them when that category OR its section is toggled off,
// so a disabled category doesn't leave its cluster glyphs on the map.
static bool cluster_should_hide(Category cat)
{
    return !g_category_visible[static_cast<int>(cat)].load() ||
           !g_section_visible[static_cast<int>(section_of(cat))].load();
}

static void apply_section_visibility(Section s, bool visible)
{
    int touched = 0;
    std::lock_guard<std::mutex> lk(g_section_hidden_mtx);
    for (const auto &r : g_section_rows)
    {
        if (r.sec != s) continue;
        uint8_t *area = r.ptr + 0x20;  // WORLD_MAP_POINT_PARAM_ST.areaNo
        if (!visible)
        {
            *area = 99;
            g_section_hidden_ptrs.insert(r.ptr);  // claim authority: keep at 99
        }
        else
        {
            g_section_hidden_ptrs.erase(r.ptr);   // release this owner
            bool keep_hidden =
                g_category_hidden_ptrs.count(r.ptr) ||   // category still hides it
                (r.is_piece && goblin::collected::is_row_collected(r.row_id)) ||
                (r.is_kindling && goblin::kindling::is_row_collected(r.row_id));
            *area = keep_hidden ? 99 : r.orig_area;
        }
        touched++;
    }
    // Gate cluster icons + members whose category lives in this section.
    bool expanded = g_clusters_expanded.load();
    for (const auto &cl : g_clusters)
        if (section_of(cl.cat) == s)
            cl.ptr[0x20] = (!visible || expanded || !g_cluster_debug.load() || cluster_should_hide(cl.cat)) ? 99 : cl.area;
    for (const auto &m : g_cluster_members)
        if (section_of(m.cat) == s)
            m.ptr[0x20] = (visible && expanded && !cluster_should_hide(m.cat)) ? m.orig_area : 99;
    spdlog::info("[SECTION] {} -> {} ({} rows)",
                 section_name(s), visible ? "SHOWN" : "HIDDEN", touched);
}

// Per-category visibility — the fine-grained twin of apply_section_visibility.
// Same areaNo park/restore + two-set coordination so a category hide survives
// the fragment/collected restore paths and doesn't fight a section toggle.
static void apply_category_visibility(Category c, bool visible)
{
    int touched = 0;
    std::lock_guard<std::mutex> lk(g_section_hidden_mtx);
    for (const auto &r : g_section_rows)
    {
        if (r.cat != c) continue;
        uint8_t *area = r.ptr + 0x20;
        if (!visible)
        {
            *area = 99;
            g_category_hidden_ptrs.insert(r.ptr);
        }
        else
        {
            g_category_hidden_ptrs.erase(r.ptr);
            bool keep_hidden =
                g_section_hidden_ptrs.count(r.ptr) ||   // section still hides it
                (r.is_piece && goblin::collected::is_row_collected(r.row_id)) ||
                (r.is_kindling && goblin::kindling::is_row_collected(r.row_id));
            *area = keep_hidden ? 99 : r.orig_area;
        }
        touched++;
    }
    // Cluster icons + their parked members belong to one category — gate them too,
    // else a disabled category leaves its (typed) cluster glyphs on the map.
    bool expanded = g_clusters_expanded.load();
    for (const auto &cl : g_clusters)
        if (cl.cat == c)
            cl.ptr[0x20] = (!visible || expanded || !g_cluster_debug.load() || cluster_should_hide(c)) ? 99 : cl.area;
    for (const auto &m : g_cluster_members)
        if (m.cat == c)
            m.ptr[0x20] = (visible && expanded && !cluster_should_hide(c)) ? m.orig_area : 99;
    spdlog::info("[CATEGORY] {} -> {} ({} rows)",
                 goblin::markers::category_name(c), visible ? "SHOWN" : "HIDDEN", touched);
}

// Master "Show icons" on/off via the SAME live areaNo lever as sections/
// categories (not the param-file swap, which only reapplies per-region as the
// game re-reads the file → icons vanish gradually). Parks/restores every
// injected row at once; g_master_off makes the restore paths keep them at 99.
static void apply_master_visibility(bool icons_on)
{
    g_master_off.store(!icons_on);
    std::lock_guard<std::mutex> lk(g_section_hidden_mtx);
    for (const auto &r : g_section_rows)
    {
        uint8_t *area = r.ptr + 0x20;
        if (!icons_on)
        {
            *area = 99;
        }
        else
        {
            bool keep_hidden =
                g_section_hidden_ptrs.count(r.ptr) || g_category_hidden_ptrs.count(r.ptr) ||
                (r.is_piece && goblin::collected::is_row_collected(r.row_id)) ||
                (r.is_kindling && goblin::kindling::is_row_collected(r.row_id));
            *area = keep_hidden ? 99 : r.orig_area;
        }
    }
    spdlog::info("[MASTER] icons {} ({} injected rows)",
                 icons_on ? "SHOWN" : "HIDDEN", g_section_rows.size());
}

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
        float dx = g.wx - wx, dz = g.wz - wz;
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
static void apply_cluster_expanded(bool expanded)
{
    // Collapsed: show the cluster icon (unless its category/section is hidden);
    // park members. Expanded: park the icon; show members (same gate).
    int cl_shown = 0, mem_parked = 0;
    for (const auto &c : g_clusters)
    {
        c.ptr[0x20] = (expanded || cluster_should_hide(c.cat)) ? 99 : c.area;
        if (c.ptr[0x20] != 99) cl_shown++;
    }
    for (const auto &m : g_cluster_members)
    {
        m.ptr[0x20] = (expanded && !cluster_should_hide(m.cat)) ? m.orig_area : 99;
        if (m.ptr[0x20] == 99) mem_parked++;
    }
    spdlog::info("[CLUSTER] {} -> {}/{} cluster icons shown, {}/{} members parked",
                 expanded ? "EXPANDED" : "COLLAPSED", cl_shown, g_clusters.size(),
                 mem_parked, g_cluster_members.size());
}

// Show / hide the on-map cluster bubbles. ON = the pile glyph is on its page with
// its count on line 2. OFF = the bubble is parked off-page (areaNo 99) entirely —
// no phantom numberless glyph; the per-category overlay census is the count source.
// Members stay parked either way (they only un-park in expanded view), so OFF just
// removes the pile icon, keeping the freeze-fix parking intact.
static void apply_cluster_debug(bool show_bubbles)
{
    bool expanded = g_clusters_expanded.load();
    for (const auto &c : g_clusters)
    {
        auto *st = reinterpret_cast<from::paramdef::WORLD_MAP_POINT_PARAM_ST *>(c.ptr);
        st->textId2 = show_bubbles ? c.count_textid : -1;  // line 2 = count; textId1 (name) stays
        bool hide = !show_bubbles || expanded || cluster_should_hide(c.cat);
        c.ptr[0x20] = hide ? 99 : c.area;
    }
    spdlog::info("[CLUSTER] bubbles -> {}", show_bubbles ? "SHOWN(+count)" : "HIDDEN");
}

// Cluster label census for setup_messages (PlaceName textId → member count).
const std::vector<std::pair<int, std::string>> &goblin::cluster_label_census()
{
    return g_cluster_census;
}

// ─── Player world position (WorldChrMan) — for proximity clustering (v2) ──
// Chain extracted from Hexinton all-in-one CT v6.0:
//   WorldChrMan static: AOB `48 8B FA 0F 11 41 70 48 8B 05`; the trailing
//   `48 8B 05` is `mov rax,[rip+disp32]` at +7 (ends +0xE) → static slot =
//   finder + 0xE + *(int32*)(finder+0xA).
//   Global pos floats: [[[WorldChrMan]+0x10EF8]+0]+0x6B0/6B4/6B8 = X/Y/Z.
// Offsets are game-version specific (CT v6.0); VERIFY in-game before trusting.
static void **g_wcm_static = nullptr;
static bool g_wcm_tried = false;

static void resolve_world_chr_man()
{
    g_wcm_tried = true;
    auto *finder = reinterpret_cast<uint8_t *>(
        modutils::scan<void>({.aob = "48 8B FA 0F 11 41 70 48 8B 05"}));
    if (!finder)
    {
        spdlog::warn("[PLAYER] WorldChrMan AOB not found");
        return;
    }
    int32_t disp = *reinterpret_cast<int32_t *>(finder + 0xA);
    g_wcm_static = reinterpret_cast<void **>(finder + 0xE + disp);
    spdlog::info("[PLAYER] WorldChrMan static @ {:p}", (void *)g_wcm_static);
}

// DIAGNOSTIC probe (POD-only; no C++ objects in the __try). Fills intermediate
// pointers + two candidate coordinate chains so one in-game run identifies the
// correct offsets. Caller logs the result OUTSIDE the SEH frame.
struct PlayerProbe
{
    void *wcm, *player, *subA;      // [static], [wcm+10EF8], [player+0]
    float a[3]; bool a_ok;          // candidate A: global = [..+0]+6B0/6B4/6B8
    void *physMod;                  // candidate B physics module
    float b[3]; bool b_ok;          // candidate B: pCoordAdr = [[[[[WCM]+1E508]+58]+10]+190]+68
};

static void probe_player_seh(void **wcm_static, PlayerProbe *pr)
{
    pr->wcm = pr->player = pr->subA = pr->physMod = nullptr;
    pr->a_ok = pr->b_ok = false;
    __try
    {
        auto *wcm = *reinterpret_cast<uint8_t **>(wcm_static);
        pr->wcm = wcm;
        if (!wcm) return;
        // Candidate A: WorldChrMan + LocalPlayerOffset(0x10EF8) + [+0] + 0x6B0..
        auto *player = *reinterpret_cast<uint8_t **>(wcm + 0x10EF8);
        pr->player = player;
        if (player)
        {
            auto *subA = *reinterpret_cast<uint8_t **>(player + 0x0);
            pr->subA = subA;
            if (subA)
            {
                pr->a[0] = *reinterpret_cast<float *>(subA + 0x6B0);
                pr->a[1] = *reinterpret_cast<float *>(subA + 0x6B4);
                pr->a[2] = *reinterpret_cast<float *>(subA + 0x6B8);
                pr->a_ok = true;
            }
            // Candidate B: physics chain [[[[player]+58]+10]+190]+68  (pCoordAdr,
            // but using the LocalPlayerOffset player rather than +1E508).
            auto *p2 = *reinterpret_cast<uint8_t **>(player + 0x58);
            if (p2) { auto *p3 = *reinterpret_cast<uint8_t **>(p2 + 0x10);
            if (p3) { auto *phys = *reinterpret_cast<uint8_t **>(p3 + 0x190);
            pr->physMod = phys;
            if (phys) {
                pr->b[0] = *reinterpret_cast<float *>(phys + 0x68 + 0x0);
                pr->b[1] = *reinterpret_cast<float *>(phys + 0x68 + 0x4);
                pr->b[2] = *reinterpret_cast<float *>(phys + 0x68 + 0x8);
                pr->b_ok = true;
            } } }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool goblin::get_player_world_pos(float &x, float &y, float &z)
{
    if (!g_wcm_tried) resolve_world_chr_man();
    if (!g_wcm_static) return false;
    PlayerProbe pr{};
    probe_player_seh(g_wcm_static, &pr);
    spdlog::info("[PLAYER] wcm={:p} player={:p} subA={:p} phys={:p} | "
                 "A(+0,+6B0)={} X={:.1f} Y={:.1f} Z={:.1f} | "
                 "B(phys+68)={} X={:.1f} Y={:.1f} Z={:.1f}",
                 pr.wcm, pr.player, pr.subA, pr.physMod,
                 pr.a_ok, pr.a[0], pr.a[1], pr.a[2],
                 pr.b_ok, pr.b[0], pr.b[1], pr.b[2]);
    if (pr.b_ok) { x = pr.b[0]; y = pr.b[1]; z = pr.b[2]; return true; }
    if (pr.a_ok) { x = pr.a[0]; y = pr.a[1]; z = pr.a[2]; return true; }
    return false;
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
        .aob = "48 8B 0D ?? ?? ?? ?? 48 8D 54 24 20 E8 ?? ?? ?? ?? F2 0F 10 05 ?? ?? ?? ??",
        .relative_offsets = {{3, 7}}}));
    g_mappos_mgr_slot = reinterpret_cast<uintptr_t>(modutils::scan<void>({
        .aob = "48 8B 0D ?? ?? ?? ?? 48 8D 53 10 E8 ?? ?? ?? ?? 4C 8B E8",
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
        pr->lx = *reinterpret_cast<float *>(mgr + 0x70);  // block-local X
        pr->lz = *reinterpret_cast<float *>(mgr + 0x74);  // block-local Z (+0x78 = height)
        pr->ok = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool goblin::get_player_map_pos(int &out_area, float &world_x, float &world_z,
                                int *out_gx, int *out_gz)
{
    if (!g_mappos_tried) resolve_player_map_pos_statics();
    if (!g_mapid_slot || !g_mappos_mgr_slot) return false;
    MapPosProbe pr{};
    probe_map_pos_seh(g_mapid_slot, g_mappos_mgr_slot, &pr);
    if (!pr.ok) return false;
    // If the player is inside a dungeon that PROJECTS to the overworld (legacy
    // dungeons like Leyndell/Stormveil, catacombs…), project their position the same
    // way the markers are projected — else the player reads the dungeon's native
    // area (e.g. 11) while its piles live on area 60, so distance-adaptive never
    // engages there and everything stays clustered. (project_* is plain C++; run it
    // OUTSIDE the SEH frame on the values the probe captured.)
    from::paramdef::WORLD_MAP_POINT_PARAM_ST tmp{};
    tmp.areaNo  = static_cast<uint8_t>(pr.area);
    tmp.gridXNo = static_cast<uint8_t>(pr.gx);
    tmp.gridZNo = static_cast<uint8_t>(pr.gz);
    tmp.posX = pr.lx;
    tmp.posZ = pr.lz;
    if (project_dungeon_row_to_overworld(&tmp))
    {
        out_area = tmp.areaNo;
        world_x = tmp.gridXNo * 256.0f + tmp.posX;
        world_z = tmp.gridZNo * 256.0f + tmp.posZ;
        if (out_gx) *out_gx = tmp.gridXNo;
        if (out_gz) *out_gz = tmp.gridZNo;
    }
    else
    {
        out_area = pr.area;
        world_x = pr.gx * 256.0f + pr.lx;   // marker space: gridX*256 + local
        world_z = pr.gz * 256.0f + pr.lz;
        if (out_gx) *out_gx = pr.gx;        // reliable tile (from MapId) — valid even underground
        if (out_gz) *out_gz = pr.gz;
    }
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
        if (pref)  { try { return &(*pref)[lot_id]; }  catch (...) {} }
        if (other) { try { return &(*other)[lot_id]; } catch (...) {} }
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
static void replan_clusters()
{
    using ST = from::paramdef::WORLD_MAP_POINT_PARAM_ST;
    std::lock_guard<std::mutex> lk(g_section_hidden_mtx);

    // 1. Tear down the previous plan: restore the members we parked, park all pool
    //    rows, clear the registries.
    for (const auto &m : g_cluster_members)
        if (g_cluster_member_ptrs.count(m.ptr)) m.ptr[0x20] = m.orig_area;
    for (auto *p : g_cluster_pool)
    {
        p[0x20] = 99;                                          // park main page
        reinterpret_cast<ST *>(p)->areaNo_forDistViewMark = 99; // and distant-view page
    }
    g_clusters.clear();
    g_cluster_members.clear();
    g_cluster_member_ptrs.clear();

    if (!goblin::config::enableClustering || g_cluster_pool.empty())
    {
        g_clustering_active = false;
        return;
    }

    // 2. Bucket the live rows by their grouping key (nearest grace, or dungeon
    //    entrance). Rows with no anchor (grp_key < 0) and categories the user left
    //    unchecked (read LIVE from g_category_cluster) stay exact on the map.
    struct Bucket { std::vector<size_t> members; double sx = 0, sz = 0; float py = 0;
                    uint8_t area = 0; float ent_x = -1, ent_z = -1; int pname = -1; int tab = 0; };
    std::unordered_map<int, Bucket> buckets;        // key = grace index / entrance key
    int skip_noloc = 0, skip_unchecked = 0;         // diagnostics (why a row stays exact)
    for (size_t i = 0; i < g_section_rows.size(); i++)
    {
        const auto &r = g_section_rows[i];
        if (r.grp_key < 0) { skip_noloc++; continue; }                    // no anchor → exact
        if (!g_category_cluster[static_cast<int>(r.cat)].load()) { skip_unchecked++; continue; } // unchecked
        auto *st = reinterpret_cast<ST *>(r.ptr);
        // Accumulate the centroid in WORLD coords (grid tile * 256 + tile-local
        // pos) so a pile whose members straddle grid tiles lands correctly. 256 =
        // display-grid tile size (matches project_dungeon_row_to_overworld).
        float wx = static_cast<float>(st->gridXNo) * 256.0f + st->posX;
        float wz = static_cast<float>(st->gridZNo) * 256.0f + st->posZ;
        auto &b = buckets[r.grp_key];
        b.members.push_back(i);
        b.sx += wx; b.sz += wz; b.py = st->posY;
        b.area = r.orig_area;
        b.tab = r.grp_tab;                             // DIAGNOSTIC: anchor sub-page
        if (b.pname < 0) b.pname = r.grp_pname;        // label = anchor's region name
        // Projected dungeon → remember its overworld entrance so the pile sits
        // there (one point) rather than at the centroid of its spread interior.
        if (r.ent_x >= 0) { b.ent_x = r.ent_x; b.ent_z = r.ent_z; }
    }

    // 3. Locations over the threshold → fill a pool row, park members. Sparse
    //    locations (≤ threshold) stay exact. Threshold counts markers per location.
    int base_thr = static_cast<int>(goblin::config::clusterThreshold);
    if (base_thr < 1) base_thr = 1;  // defense: 0 would cluster every location → pool blowout
    // Distance-adaptive: read the player position ONCE (map-open replan). Each pile's
    // threshold ramps base→far over near→far radius (tiles), SAME map area only, so
    // distant dense spots merge harder (fewer far icons) while near you stays detailed.
    const bool dist_adaptive = goblin::config::clusterDistanceAdaptive;
    int player_area = -1; float player_wx = 0, player_wz = 0;
    int player_gx = -1, player_gz = -1;
    const bool have_player =
        dist_adaptive && goblin::get_player_map_pos(player_area, player_wx, player_wz,
                                                    &player_gx, &player_gz);
    // The grid*256+local world frame is only valid on the 256-tiled overworld pages
    // (60/61) — and projected dungeons resolve the player onto 60. Underground
    // (area 12 / DLC 40-43) has non-256 tiles AND a leaf-block-local player float,
    // so Euclidean distance there is garbage. Fall back to TILE distance (player &
    // pile tiles are reliable from MapId/gridXNo) for those pages.
    const bool euclid_frame = (player_area == 60 || player_area == 61);
    // Underground (non-Euclidean frame): the player's reliable MapId tile -> sub-page
    // (tabId). Piles on the SAME sub-page as the player get near-detail; other
    // sub-pages cluster. (Tile/Euclidean distance can't separate underground sub-
    // regions — they share coarse gridXNo 1/2 — so discriminate by sub-page instead.)
    const int player_tab =
        (have_player && !euclid_frame) ? tab_for_tile(player_area, player_gx, player_gz) : -1;
    int near_thr = static_cast<int>(goblin::config::clusterNearThreshold);
    if (near_thr < 1) near_thr = 1;
    const float near_u  = goblin::config::clusterNearRadius * 256.0f;
    const float far_u   = goblin::config::clusterFarRadius * 256.0f;
    size_t pi = 0;
    int dropped = 0, sub_threshold = 0;
    const bool dbg = goblin::config::debugLogging;          // detailed dumps off by default
    std::map<std::pair<int, int>, int> piles_at;           // (area,tab) -> # piles
    std::map<std::pair<int, int>, int> subthr_at;          // (area,tab) -> # sub-threshold locations
    for (auto &kv : buckets)
    {
        Bucket &b = kv.second;
        // Pile position (also its world centroid for distance). Computed BEFORE the
        // threshold so distance-adaptive can use it; no pool row taken yet. A
        // projected dungeon sits at its overworld ENTRANCE (256-tiled → world split
        // valid); otherwise keep a REAL member's grid tile and average posX/posZ over
        // members in THAT SAME tile — never re-split a world centroid by 256 (area-12
        // underground tiles aren't 256 wide, so the split invents a bad grid index).
        // Entrance-key buckets (graceless projected dungeons, key >= 1000000) sit at
        // the dungeon entrance. Grace buckets — including dungeons WITH graces, whose
        // members are projected — use the centroid of their (projected) member tiles.
        uint8_t cgx, cgz; float cpx, cpz;
        const bool entrance_bucket = (kv.first >= 1000000) && (b.ent_x >= 0);
        if (entrance_bucket)
        {
            int gx = static_cast<int>(std::floor(b.ent_x / 256.0));
            int gz = static_cast<int>(std::floor(b.ent_z / 256.0));
            cgx = static_cast<uint8_t>(gx);
            cgz = static_cast<uint8_t>(gz);
            cpx = static_cast<float>(b.ent_x - gx * 256.0);
            cpz = static_cast<float>(b.ent_z - gz * 256.0);
        }
        else
        {
            auto *seed = reinterpret_cast<ST *>(g_section_rows[b.members[0]].ptr);
            cgx = seed->gridXNo; cgz = seed->gridZNo;
            double sx = 0, sz = 0; int n = 0;
            for (size_t mi : b.members)
            {
                auto *m = reinterpret_cast<ST *>(g_section_rows[mi].ptr);
                if (m->gridXNo == cgx && m->gridZNo == cgz) { sx += m->posX; sz += m->posZ; n++; }
            }
            cpx = static_cast<float>(sx / n);
            cpz = static_cast<float>(sz / n);
        }

        // Per-pile threshold: HIGH near the player (few piles → detail / real items),
        // ramping DOWN to base_thr far away (more clustering → fewer distant icons).
        // Same area only — world coords aren't comparable across pages.
        int thr = base_thr;
        if (have_player && b.area == player_area)
        {
            if (euclid_frame && far_u > near_u)
            {
                // Overworld / projected-dungeon: real Euclidean distance in marker
                // space, ramped near_thr (detail) -> base_thr (clustered).
                float dx = (cgx * 256.0f + cpx) - player_wx;
                float dz = (cgz * 256.0f + cpz) - player_wz;
                float d = std::sqrt(dx * dx + dz * dz);
                float t = (d - near_u) / (far_u - near_u);
                if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
                thr = static_cast<int>(std::lround(near_thr + t * (base_thr - near_thr)));
                if (thr < 1) thr = 1;
            }
            else if (!euclid_frame && player_tab > 0)
            {
                // Underground: sub-page (tabId) gradient. The player's sub-region gets
                // near-detail (near_thr), every other sub-page clusters (base_thr).
                // Discrete — no ramp, since sub-pages have no in-between distance.
                thr = (b.tab == player_tab) ? near_thr : base_thr;
                if (thr < 1) thr = 1;
            }
        }

        if (static_cast<int>(b.members.size()) <= thr)
        {
            sub_threshold++;
            if (dbg) subthr_at[{b.area, b.tab}]++;
            continue;
        }
        if (pi >= g_cluster_pool.size()) { dropped++; continue; }

        uint8_t *cp = g_cluster_pool[pi++];
        auto *cd = reinterpret_cast<ST *>(cp);
        // Seed the pile from a REAL member row so it inherits EVERY page-selecting
        // field (base / DLC / underground); then override position/label/icon below.
        *cd = *reinterpret_cast<ST *>(g_section_rows[b.members[0]].ptr);
        // RACE GUARD: the seed-copy just made this pool row an exact copy of an
        // on-page member (its areaNo, icon, isAreaIcon, size). The game renders on
        // another thread, so if it reads the row NOW it shows a duplicate, sometimes
        // OVERSIZED (member isAreaIcon) marker. Keep the row OFF-PAGE while we set its
        // fields, and publish areaNo (→ visible) LAST, fully formed.
        cd->areaNo = 99;
        cd->areaNo_forDistViewMark = 99;
        cd->eventFlagId = 0; cd->clearedEventFlagId = 0;     // a pile has no appear/clear gate
        cd->textDisableFlagId1 = cd->textDisableFlagId2 = cd->textDisableFlagId3 = 0;
        cd->textDisableFlagId4 = cd->textDisableFlagId5 = cd->textDisableFlagId6 = 0;
        cd->textDisableFlagId7 = cd->textDisableFlagId8 = 0;
        cd->textId5 = cd->textId6 = cd->textId7 = cd->textId8 = -1;
        cd->gridXNo = cgx; cd->gridZNo = cgz;
        cd->posX = cpx; cd->posZ = cpz;
        cd->posY = b.py;
        // Mirror the distant-view coords (used by some pages / zoom levels).
        cd->gridXNo_forDistViewMark = cgx;
        cd->gridZNo_forDistViewMark = cgz;
        cd->posX_forDistViewMark = cpx;
        cd->posY_forDistViewMark = b.py;
        cd->posZ_forDistViewMark = cpz;
        cd->iconId = static_cast<uint16_t>(goblin::generated::CLUSTER_ICON_ID);
        // The pile is a POINT icon. Seeding from a member can inherit isAreaIcon
        // (a range icon = "same size as the map") → the pile renders oversized; and
        // a stale distViewIconId would show the seed's glyph when zoomed out. Force
        // both to the cluster glyph / point.
        cd->isAreaIcon = false;
        cd->distViewIconId = static_cast<uint16_t>(goblin::generated::CLUSTER_ICON_ID);
        int cnt = std::min<int>(static_cast<int>(b.members.size()), CLUSTER_MAX_COUNT);
        int cnt_textid = CLUSTER_TEXTID_BASE + cnt;    // → pre-injected number string
        // Line 1 = the anchor's region name (its PlaceName id renders directly); if
        // the anchor has NO name, fall back to the count so the pile ALWAYS has a
        // hover tooltip (was -1 → no tooltip at all on nameless piles). Line 2 = the
        // live count when "show counts" is on (apply_cluster_debug toggles it), but
        // not on nameless piles (the count is already on line 1 there).
        const bool named = (b.pname > 0);
        cd->textId1 = named ? b.pname : cnt_textid;
        cd->textId2 = (named && g_cluster_debug.load()) ? cnt_textid : -1;
        cd->textId3 = cd->textId4 = -1;
        // PUBLISH last: every field is now set, so make the row visible only now.
        // (areaNo is the on-page lever; the render thread sees a fully-formed row.)
        // Bubbles default OFF (counts live in the overlay census) — park unless the
        // user has the "Show cluster bubbles" toggle on. apply_cluster_debug flips
        // this live; the section/category gates re-apply the same g_cluster_debug check.
        uint8_t pub_area = g_cluster_debug.load() ? b.area : 99;
        cd->areaNo_forDistViewMark = pub_area;
        cd->areaNo = pub_area;

        std::vector<ClusterMemberRef> mrefs;
        Category domcat = g_section_rows[b.members[0]].cat;  // for section gating
        for (size_t mi : b.members)
        {
            const auto &r = g_section_rows[mi];
            r.ptr[0x20] = 99;                          // park member off-page
            g_cluster_member_ptrs.insert(r.ptr);
            g_cluster_members.push_back({r.ptr, r.orig_area, r.cat});
            uint32_t f = reinterpret_cast<ST *>(r.ptr)->textDisableFlagId1;  // collect flag
            // Track any member that CAN be collected — plain loot (event flag) AND
            // Reforged pieces/kindling (row-id tracked, no flag) — so the live count
            // decrements for ERR items too, not only base flag-loot.
            if (f || r.is_piece || r.is_kindling)
                mrefs.push_back({f, r.row_id, r.is_piece, r.is_kindling});
        }
        g_clusters.push_back({cp, b.area, cnt_textid, std::move(mrefs), domcat});
        piles_at[{b.area, b.tab}]++;
        if (dbg)
            spdlog::info("[CLUSTER-DUMP] #{} key={} area={} tab={} grid=({},{}) pos=({:.1f},{:.1f}) "
                         "pname={} members={} mode={}",
                         g_clusters.size() - 1, kv.first, b.area, b.tab, cgx, cgz,
                         cd->posX, cd->posZ, b.pname, static_cast<int>(b.members.size()),
                         (b.ent_x >= 0) ? "ENTRANCE" : "CENTROID");
    }
    g_clustering_active = !g_clusters.empty();
    spdlog::info("[CLUSTER] replan by-location: {} piles, {} members parked, {}/{} pool used, {} dropped",
                 g_clusters.size(), g_cluster_members.size(),
                 pi, g_cluster_pool.size(), dropped);
    spdlog::info("[CLUSTER] stayed-exact: {} no-location, {} unchecked-category, {} locations sub-threshold (base <= {}{})",
                 skip_noloc, skip_unchecked, sub_threshold, base_thr,
                 (dist_adaptive && have_player)
                     ? (euclid_frame ? ", distance-adaptive[euclid]"
                                     : ", distance-adaptive[subpage tab=" + std::to_string(player_tab) +
                                       " tile=" + std::to_string(player_gx) + "," + std::to_string(player_gz) + "]")
                     : "");
    {
        // Per-area cluster tally (which map page each pile lands on): 60/61 =
        // overworld, 12 = underground, others = legacy dungeons. Tells us where the
        // stray "in the sea" piles live and whether underground gets any clusters.
        std::map<int, int> per_area;
        for (const auto &c : g_clusters) per_area[c.area]++;
        std::string s;
        for (const auto &kv : per_area)
            s += " a" + std::to_string(kv.first) + "=" + std::to_string(kv.second);
        spdlog::info("[CLUSTER] piles per area:{}", s.empty() ? " (none)" : s);
    }
    {
        // Per-(area, sub-page) tally — splits underground area 12 into its real
        // pages (Ainsel 12000 / Siofra 12001 / Deeproot 12002), so an empty sub-page
        // (no piles) is visible as the absence of its tab.
        std::string s;
        for (const auto &kv : piles_at)
            s += " a" + std::to_string(kv.first.first) + "t" + std::to_string(kv.first.second) +
                 "=" + std::to_string(kv.second);
        spdlog::info("[CLUSTER] piles per area/tab:{}", s.empty() ? " (none)" : s);
    }
    if (dbg)
    {
        // Which (area, sub-page) had locations that stayed exact for being sub-
        // threshold — tells "Siofra/Deeproot are just sparse" apart from "mis-paged".
        std::string s;
        for (const auto &kv : subthr_at)
            s += " a" + std::to_string(kv.first.first) + "t" + std::to_string(kv.first.second) +
                 "=" + std::to_string(kv.second);
        spdlog::info("[CLUSTER-DUMP] sub-threshold per area/tab:{}", s.empty() ? " (none)" : s);
    }

    // 4. Apply the current collapsed/expanded view to the new plan.
    apply_cluster_expanded(g_clusters_expanded.load());
}

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

void goblin::inject_map_entries()
{
    GOBLIN_BENCH("map.inject.total");
    // (The CSFreeListMemorySystem int3-assert NOP patch that used to run here
    // was removed 2026-05-29: it was an artifact of the old hosting-crash
    // theory. The real cause was the 16-align bug in the wrapper_row_locator
    // layout; with that fixed, hosting works with no assert patching —
    // verified live. See docs/ersc_hosting_and_map_autohide.md.)

    struct InjectedEntry
    {
        int32_t row_id;
        uint64_t original_row_id;
        const from::paramdef::WORLD_MAP_POINT_PARAM_ST *data;
        bool is_piece;     // collected::register_param_ptr (CSWorldGeomMan-tracked)
        bool is_kindling;  // kindling::register_param_ptr  (SFX-region-tracked)
        Category category;
        uint32_t lotId;    // live-loot: source ItemLotParam row (0 = none)
        uint8_t lotType;   // 0=none, 1=ItemLotParam_map, 2=ItemLotParam_enemy
    };

    // Live-loot icons (config::liveLootIcons): a randomized lot may now hold an
    // item of a different category than the one baked at this marker. Read the
    // live item, look up the icon + category it would get as a normal marker,
    // and gate / re-icon by THAT instead of the baked category. Resolved icons
    // are keyed by original_row_id and applied when the row is copied below.
    LotReader lot_reader;
    if (goblin::config::liveLootIcons)
        lot_reader.init();
    std::unordered_map<uint64_t, uint16_t> live_icon_override;
    size_t live_recat = 0;

    // Filter: only include enabled categories (disabled ones are simply not injected)
    std::vector<InjectedEntry> entries;
    entries.reserve(generated::MAP_ENTRY_COUNT);

    size_t skipped_by_config = 0;
    {
    GOBLIN_BENCH("map.inject.filter");
    for (size_t i = 0; i < generated::MAP_ENTRY_COUNT; i++)
    {
        const auto &e = generated::MAP_ENTRIES[i];
        bool is_piece = e.category == Category::ReforgedRunePieces ||
                        e.category == Category::ReforgedEmberPieces ||
                        e.category == Category::LootMaterialNodes;
        bool is_kindling = e.category == Category::WorldKindlingSpirits;
        // Live-loot linkage: only for lot-backed loot rows, and never for
        // piece/kindling rows (those are geom/SFX-tracked via collected::).
        uint32_t lotId = (is_piece || is_kindling) ? 0 : e.lotId;
        uint8_t lotType = (is_piece || is_kindling) ? 0 : e.lotType;

        // Resolve the gate/icon from the LIVE item when live-loot icons is on.
        // Spoiler-free mode takes precedence: keep the BAKED category gate (so
        // visibility doesn't leak the hidden item's type) and force the "?" icon
        // on every lot-backed marker.
        Category gate_cat = e.category;
        const bool is_lot = (lotType != 0 && lotId != 0);
        if (goblin::config::anonymousLoot && is_lot)
        {
            live_icon_override[e.row_id] = goblin::generated::ANON_ICON_ID;
        }
        else if (goblin::config::liveLootIcons && is_lot && lot_reader.ok())
        {
            if (RawItemLotRow *r = lot_reader.row(lotId, lotType))
            {
                int32_t item_id = *reinterpret_cast<int32_t *>(r->b + 0x00);   // lotItemId01
                int32_t cat     = *reinterpret_cast<int32_t *>(r->b + 0x20);   // lotItemCategory01
                if (item_id > 0)
                {
                    const auto *ic = lookup_item_icon(encode_live_item(item_id, cat));
                    if (ic)
                    {
                        if (ic->category != gate_cat) live_recat++;
                        gate_cat = ic->category;
                        live_icon_override[e.row_id] = ic->iconId;
                    }
                }
            }
        }

        // (park-all) Inject EVERY category's rows. Disabled ones are parked
        // (areaNo 99 = off-page = free at map-open) at init and stay live-
        // toggleable from the menu. Store gate_cat (the effective, live-loot-
        // adjusted category) so the registry/parking/menu all agree.
        if (!is_category_enabled(gate_cat))
            skipped_by_config++;   // now "injected but born-hidden"
        entries.push_back({0, e.row_id, &e.data, is_piece, is_kindling, gate_cat, lotId, lotType});
    }
    } // map.inject.filter

    // Clustering plan (density-triggered, static). Bucket injected markers by
    // their FINAL (projected) area + cell; any cell over the threshold becomes a
    // single cluster row (appended to `entries`) and its members are recorded
    // for parking. Pieces/kindling (rune/ember pieces, material nodes) ARE
    // clustered — they're the densest free-pickup clutter — but their
    // collected/kindling registration is skipped below when clustered so the two
    // areaNo owners don't fight.
    // ── Cluster pool reservation (the plan itself is RUNTIME) ────────────────
    // The cluster plan is computed at RUNTIME by replan_clusters() so enable /
    // soft-hard / threshold / exclude all take effect with NO restart. The param
    // table can't grow at runtime, so reserve a fixed POOL of spare cluster rows
    // here (parked off-page); replan fills/empties them live. Counts are live too:
    // pre-inject number strings "1".."MAX" → a pile points its label at
    // CLUSTER_TEXTID_BASE + count.
    std::vector<size_t> pool_entry_idx;
    {
        GOBLIN_BENCH("map.inject.cluster_pool");
        for (int n = 1; n <= CLUSTER_MAX_COUNT; n++)
            g_cluster_census.emplace_back(CLUSTER_TEXTID_BASE + n, std::to_string(n));
        // Category-name labels (textId1 = the pile's type, e.g. "Smithing Stones").
        for (int c = 0; c < NUM_CATEGORIES; c++)
            g_cluster_census.emplace_back(CLUSTER_CATNAME_TEXTID_BASE + c,
                                          goblin::markers::category_name(static_cast<Category>(c)));
        from::paramdef::WORLD_MAP_POINT_PARAM_ST tmpl =
            entries.empty() ? from::paramdef::WORLD_MAP_POINT_PARAM_ST{}
                            : *entries.front().data;
        for (size_t k = 0; k < CLUSTER_POOL_SIZE; k++)
        {
            auto *cd = new from::paramdef::WORLD_MAP_POINT_PARAM_ST(tmpl);
            cd->areaNo = 99;             // parked; replan sets the real page + pos
            cd->areaNo_forDistViewMark = 99;  // also park the distant-view page (template = base 60)
            cd->iconId = static_cast<uint16_t>(goblin::generated::CLUSTER_ICON_ID);
            cd->eventFlagId = 0; cd->clearedEventFlagId = 0;
            cd->textId1 = -1;            // replan sets the live count label
            cd->textId2 = cd->textId3 = cd->textId4 = -1;
            cd->textId5 = cd->textId6 = cd->textId7 = cd->textId8 = -1;
            cd->textDisableFlagId1 = cd->textDisableFlagId2 = cd->textDisableFlagId3 = 0;
            cd->textDisableFlagId4 = cd->textDisableFlagId5 = cd->textDisableFlagId6 = 0;
            cd->textDisableFlagId7 = cd->textDisableFlagId8 = 0;
            pool_entry_idx.push_back(entries.size());
            entries.push_back({0, 0, cd, false, false, Category::WorldInteractables, 0, 0});
        }
        spdlog::info("[CLUSTER] reserved pool of {} spare rows (+{} count strings); "
                     "plan is RUNTIME (replan_clusters)", CLUSTER_POOL_SIZE, CLUSTER_MAX_COUNT);
    }
#if 0  // OLD init-time plan — replaced by runtime replan_clusters()
    {
        struct Bucket { std::vector<size_t> members; double sx = 0, sz = 0; float py = 0;
                        uint8_t area = 0, gx = 0, gz = 0; Category cat{}; };
        std::unordered_map<uint64_t, Bucket> buckets;
        // HARD clustering = bucket a cell's markers regardless of category (ONE mixed
        // pile per dense cell — far more aggressive declutter). SOFT (default) = per-
        // category buckets (typed piles). Excluded categories stay exact in both.
        const bool hard = goblin::config::clusterHard;
        // Key by category too: each category clusters separately in a cell, so a
        // per-category threshold is well-defined and the cluster label/icon can be
        // typed (e.g. "Smithing Stones (12)" + "Golden Runes (9)" instead of one
        // mixed pile). cat in bits 56-63, area 48-55, cx/cz in 24-bit lanes.
        auto cell_key = [](Category cat, uint8_t area, int cx, int cz) -> uint64_t {
            return (static_cast<uint64_t>(static_cast<uint8_t>(cat)) << 56) |
                   (static_cast<uint64_t>(area) << 48) |
                   ((static_cast<uint64_t>((cx + 0x400000) & 0xFFFFFF)) << 24) |
                    (static_cast<uint64_t>((cz + 0x400000) & 0xFFFFFF));
        };
        for (size_t i = 0; i < entries.size(); i++)
        {
            // Per-category opt-out: excluded categories never join a bucket, so
            // they stay exact markers (no parking, no cluster row).
            if (!category_clustered_cfg(entries[i].category))
                continue;
            from::paramdef::WORLD_MAP_POINT_PARAM_ST tmp = *entries[i].data;
            if (goblin::config::projectDungeons)
                project_dungeon_row_to_overworld(&tmp);
            int cx = static_cast<int>(std::floor(tmp.posX / CLUSTER_CELL));
            int cz = static_cast<int>(std::floor(tmp.posZ / CLUSTER_CELL));
            Category key_cat = hard ? static_cast<Category>(0) : entries[i].category;
            auto &b = buckets[cell_key(key_cat, tmp.areaNo, cx, cz)];
            b.members.push_back(i);
            b.sx += tmp.posX; b.sz += tmp.posZ; b.py = tmp.posY;
            b.area = tmp.areaNo; b.gx = tmp.gridXNo; b.gz = tmp.gridZNo;
            b.cat = entries[i].category;  // soft: all same; hard: dominant set below
        }
        // Census for the sorted dump below (quantifies each pile so the
        // Leyndell-area cluster's size can be read off without hovering — used to
        // test the post-burn overload theory: Royal areaNo-11 + Ashen areaNo-35
        // both project to the same overworld cell, so one cluster = their sum).
        struct CensusRow { int count; uint8_t area, gx, gz; float cx, cz; };
        std::vector<CensusRow> census;

        int cidx = 0;
        for (auto &kv : buckets)
        {
            Bucket &b = kv.second;
            // HARD uses the global threshold (a mixed pile has no per-category
            // threshold); SOFT uses the per-category effective threshold.
            int thr = hard ? static_cast<int>(goblin::config::clusterThreshold)
                           : cluster_threshold_for_cfg(b.cat);
            if (static_cast<int>(b.members.size()) <= thr) continue;
            if (hard)
            {
                // Pick the dominant member category for the mixed cluster's icon /
                // label / section-gating.
                std::unordered_map<int, int> cc;
                int best = -1, bestc = 0;
                for (size_t mi : b.members)
                {
                    int c = ++cc[static_cast<int>(entries[mi].category)];
                    if (c > bestc) { bestc = c; best = static_cast<int>(entries[mi].category); }
                }
                if (best >= 0) b.cat = static_cast<Category>(best);
            }
            auto *cd = new from::paramdef::WORLD_MAP_POINT_PARAM_ST(*entries[b.members[0]].data);
            cd->areaNo = b.area; cd->gridXNo = b.gx; cd->gridZNo = b.gz;
            cd->posX = static_cast<float>(b.sx / b.members.size());
            cd->posZ = static_cast<float>(b.sz / b.members.size());
            cd->posY = b.py;
            cd->iconId = static_cast<uint16_t>(goblin::generated::CLUSTER_ICON_ID); // distinct "stack of dots" glyph
            // Clean standalone icon: no gates. Label = the member count by default
            // (shown on hover); the F11 debug toggle flips it to icon-only (-1).
            cd->eventFlagId = 0; cd->clearedEventFlagId = 0;
            int textid = CLUSTER_TEXTID_BASE + cidx;
            cd->textId1 = textid;
            cd->textId2 = cd->textId3 = cd->textId4 = -1;
            cd->textId5 = cd->textId6 = cd->textId7 = cd->textId8 = -1;
            cd->textDisableFlagId1 = cd->textDisableFlagId2 = cd->textDisableFlagId3 = 0;
            cd->textDisableFlagId4 = cd->textDisableFlagId5 = cd->textDisableFlagId6 = 0;
            cd->textDisableFlagId7 = cd->textDisableFlagId8 = 0;
            {
                // Buckets are now single-category, so label by TYPE (+ region for
                // context when known): "Smithing Stones — Limgrave (12)".
                int cnt = static_cast<int>(b.members.size());
                // HARD = mixed pile → label by region only (no single type).
                std::string type = hard ? "" : goblin::markers::category_name(b.cat);
                std::string region = goblin::cluster_region_label(b.area, b.gx, b.gz);
                if (region.empty())
                {
                    // Projected tile maps to no fragment region (Haligtree, the
                    // underground, Leyndell-legacy…) → name by the dominant member
                    // ORIGINAL area (pre-projection data here, projection runs later).
                    std::unordered_map<int, int> acount;
                    int best = -1, bestc = 0;
                    for (size_t mi : b.members)
                    {
                        int a = entries[mi].data->areaNo;
                        int c = ++acount[a];
                        if (c > bestc) { bestc = c; best = a; }
                    }
                    region = goblin::area_region_label(best);
                }
                std::string label = type.empty() ? region : type;
                if (!region.empty() && !type.empty()) label += " — " + region;
                label += " (" + std::to_string(cnt) + ")";
                g_cluster_census.emplace_back(textid, std::move(label));
            }
            {
                auto &mf = cluster_flags_by_textid[textid];
                for (size_t mi : b.members)
                {
                    clustered_member_ids.insert(entries[mi].original_row_id);
                    uint32_t f = entries[mi].data->textDisableFlagId1; // collect flag
                    if (f) mf.push_back(f);
                }
            }
            cluster_entry_idx.push_back(entries.size());
            cluster_count_textid.push_back(textid);
            entries.push_back({0, 0, cd, false, false, b.cat, 0, 0});
            census.push_back({static_cast<int>(b.members.size()), b.area, b.gx, b.gz,
                              cd->posX, cd->posZ});
            cidx++;
        }
        g_clustering_active = !g_cluster_census.empty();  // any pile over threshold?
        spdlog::info("[CLUSTER] planned {} clusters covering {} markers (cell={}, threshold={})",
                     g_cluster_census.size(), clustered_member_ids.size(), CLUSTER_CELL,
                     static_cast<int>(goblin::config::clusterThreshold));
        // Sorted census (biggest piles first). area=60/61 overworld, area 11/35 =
        // Leyndell Royal/Ashen if unprojected. world tile XX=gridX/2, YY=gridZ/2.
        std::sort(census.begin(), census.end(),
                  [](const CensusRow &a, const CensusRow &b) { return a.count > b.count; });
        for (size_t i = 0; i < census.size(); i++)
        {
            const auto &c = census[i];
            spdlog::info("[CLUSTER] #{:<3} count={:<4} area={:<2} tile=({},{}) "
                         "worldTile=({},{}) pos=({:.0f},{:.0f})",
                         i + 1, c.count, c.area, c.gx, c.gz, c.gx / 2, c.gz / 2, c.cx, c.cz);
        }
    }
#endif  // OLD init-time plan

    spdlog::info("Injecting {} map entries ({} skipped by config, {} live-recategorized)",
                 entries.size(), skipped_by_config, live_recat);
    spdlog::info("[BENCH] map.inject.filter.count: {} kept of {} baked",
                 entries.size(), generated::MAP_ENTRY_COUNT);

    auto param_res_cap = find_world_map_point_param_res_cap();
    if (!param_res_cap)
    {
        spdlog::error("WorldMapPointParam not found");
        return;
    }

    auto *rescap = reinterpret_cast<uint8_t *>(param_res_cap->param_header);
    auto *&file_ptr_ref = *reinterpret_cast<uint8_t **>(rescap + 0x80);
    auto &file_size_ref = *reinterpret_cast<int64_t *>(rescap + 0x78);

    auto *old_param_file = file_ptr_ref;
    auto *old_table = reinterpret_cast<ParamTable *>(old_param_file);
    uint16_t orig_num_rows = old_table->num_rows;

    spdlog::debug("Original WorldMapPointParam: {} rows", orig_num_rows);

    // Collect vanilla row IDs to avoid collisions
    std::set<int32_t> vanilla_ids;
    for (uint16_t i = 0; i < orig_num_rows; i++)
        vanilla_ids.insert(static_cast<int32_t>(old_table->rows[i].row_id));

    // Assign sequential IDs starting from 1, skipping vanilla IDs
    std::unordered_map<uint64_t, uint64_t> id_remap;  // original -> dynamic
    int32_t next_id = 1;
    for (auto &entry : entries)
    {
        while (vanilla_ids.count(next_id))
            next_id++;
        id_remap[entry.original_row_id] = static_cast<uint64_t>(next_id);
        entry.row_id = next_id++;
    }

    // Update collected + kindling systems with new dynamic IDs
    collected::remap_row_ids(id_remap);
    kindling::remap_row_ids(id_remap);

    // Pool row_ids (the reserved spare cluster rows) → the build loop collects
    // their live ptrs into g_cluster_pool; replan_clusters() fills them at runtime.
    std::set<int32_t> pool_row_ids;
    for (size_t idx : pool_entry_idx) pool_row_ids.insert(entries[idx].row_id);
    g_cluster_pool.clear();
    g_cluster_pool.reserve(pool_entry_idx.size());

    spdlog::debug("Assigned IDs: {} entries, range {}-{}, remapped {} piece IDs",
                  entries.size(),
                  entries.empty() ? 0 : entries.front().row_id,
                  entries.empty() ? 0 : entries.back().row_id,
                  id_remap.size());

    uint32_t new_entry_count = static_cast<uint32_t>(entries.size());
    uint32_t total_rows = orig_num_rows + new_entry_count;

    spdlog::debug("Injecting {} entries ({} total)", new_entry_count, total_rows);

    constexpr size_t WRAPPER_HEADER = 0x10;
    constexpr size_t HEADER_SIZE = 0x40;
    constexpr size_t ROW_LOCATOR_SIZE = sizeof(ParamRowInfo);
    constexpr size_t PARAM_DATA_SIZE = sizeof(from::paramdef::WORLD_MAP_POINT_PARAM_ST);
    constexpr size_t WRAPPER_ROW_LOC_SIZE = sizeof(WrapperRowLocator);

    const char *type_str = reinterpret_cast<const char *>(old_param_file + old_table->param_type_offset);
    size_t type_str_len = strlen(type_str) + 1;

    size_t row_locators_start = HEADER_SIZE;
    size_t data_start = row_locators_start + total_rows * ROW_LOCATOR_SIZE;
    size_t data_end = data_start + total_rows * PARAM_DATA_SIZE;
    size_t type_str_start = data_end;
    size_t after_type_str = type_str_start + type_str_len;
    // Align wrapper_row_loc to 16: the param lookup-by-id engine reads this
    // offset from the wrapper header and rounds it UP to 16 (`(x+0xf)&~0xf`)
    // before using it as the binary-search base. 4-align worked for WMP only
    // because it's iterated, never id-looked-up — but keep it correct so an
    // id lookup (or a future engine path) can't read past the array. (This
    // exact bug crashed TutorialParam save-load; see inject_tutorial_popup_rows.)
    size_t wrapper_row_loc_start = (after_type_str + 0xf) & ~(size_t)0xf;
    size_t wrapper_row_loc_end = wrapper_row_loc_start + total_rows * WRAPPER_ROW_LOC_SIZE;
    size_t param_file_size = wrapper_row_loc_end;
    size_t total_alloc = WRAPPER_HEADER + param_file_size;

    // HeapAlloc (not VirtualAlloc): Seamless Co-op's `game_memory_unlimiter`
    // module crashes when hosting if our expanded ParamTable lives on a
    // dedicated VirtualAlloc'd page region — ERSC apparently expects param
    // memory to come from the process heap. HEAP_ZERO_MEMORY zero-inits.
    allocation = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total_alloc);
    if (!allocation)
    {
        spdlog::error("HeapAlloc failed ({} bytes)", total_alloc);
        return;
    }

    auto *new_wrapper = reinterpret_cast<uint8_t *>(allocation);
    auto *new_param_file = new_wrapper + WRAPPER_HEADER;
    auto *new_table = reinterpret_cast<ParamTable *>(new_param_file);

    *reinterpret_cast<uint32_t *>(new_wrapper + 0x00) = static_cast<uint32_t>(wrapper_row_loc_start);
    *reinterpret_cast<int32_t *>(new_wrapper + 0x04) = static_cast<int32_t>(total_rows);

    memcpy(new_param_file, old_param_file, HEADER_SIZE);
    new_table->num_rows = static_cast<uint16_t>(total_rows);
    new_table->param_type_offset = type_str_start;
    *reinterpret_cast<uint32_t *>(new_param_file + 0x00) = static_cast<uint32_t>(type_str_start);
    *reinterpret_cast<uint16_t *>(new_param_file + 0x04) = static_cast<uint16_t>(data_start);
    *reinterpret_cast<uint64_t *>(new_param_file + 0x30) = data_start;

    memcpy(new_param_file + type_str_start, type_str, type_str_len);

    struct RowSource
    {
        int32_t row_id;
        const uint8_t *data_ptr;
        bool is_piece;
        bool is_kindling;
        Category category;
        uint64_t original_row_id;  // pre-remap id (matches locationOverrides keys); 0 for vanilla rows
        uint32_t lotId;            // live-loot: source ItemLotParam row (0 = none)
        uint8_t lotType;           // 0=none, 1=ItemLotParam_map, 2=ItemLotParam_enemy
    };

    std::vector<RowSource> all_rows;
    all_rows.reserve(total_rows);

    for (uint16_t i = 0; i < orig_num_rows; i++)
    {
        auto *data = old_param_file + old_table->rows[i].param_offset;
        all_rows.push_back({static_cast<int32_t>(old_table->rows[i].row_id), data, false, false, {}, 0, 0, 0});
    }
    for (auto &entry : entries)
    {
        all_rows.push_back({entry.row_id, reinterpret_cast<const uint8_t *>(entry.data),
                            entry.is_piece, entry.is_kindling, entry.category, entry.original_row_id,
                            entry.lotId, entry.lotType});
    }

    {
        GOBLIN_BENCH("map.inject.sort");
        std::sort(all_rows.begin(), all_rows.end(),
                  [](const RowSource &a, const RowSource &b) { return a.row_id < b.row_id; });
    }
    spdlog::info("[BENCH] map.inject.rows.count: {} rows (vanilla {} + injected {})",
                 all_rows.size(), orig_num_rows, new_entry_count);

    auto *new_locators = reinterpret_cast<ParamRowInfo *>(new_param_file + row_locators_start);
    auto *new_wrapper_locs = reinterpret_cast<WrapperRowLocator *>(new_param_file + wrapper_row_loc_start);
    size_t file_end_marker = type_str_start + type_str_len;

    int reprojected_dungeons = 0;
    {
    GOBLIN_BENCH("map.inject.build_rows");
    for (size_t i = 0; i < all_rows.size(); i++)
    {
        size_t data_offset = data_start + i * PARAM_DATA_SIZE;
        new_locators[i].row_id = static_cast<uint64_t>(all_rows[i].row_id);
        new_locators[i].param_offset = data_offset;
        new_locators[i].param_end_offset = file_end_marker;
        memcpy(new_param_file + data_offset, all_rows[i].data_ptr, PARAM_DATA_SIZE);
        // Nearest-grace anchor — computed on the marker's ORIGINAL (pre-projection)
        // area + coords, so a catacomb/legacy marker matches a grace inside its OWN
        // dungeon, not a random surface grace. This is the authoritative location
        // grouping (replaces the incomplete textId-based location lookup).
        int grace_idx = -1, grace_pname = -1, grace_tab = 0;
        if (all_rows[i].original_row_id != 0)
        {
            auto *mrow = reinterpret_cast<from::paramdef::WORLD_MAP_POINT_PARAM_ST *>(
                new_param_file + data_offset);
            float mwx = static_cast<float>(mrow->gridXNo) * 256.0f + mrow->posX;
            float mwz = static_cast<float>(mrow->gridZNo) * 256.0f + mrow->posZ;
            find_nearest_grace(mrow->areaNo, mwx, mwz, &grace_idx, &grace_pname, &grace_tab);
            // No named grace nearby (DLC graces have no PlaceName) → label by the
            // nearest MSB region volume; if even that misses (anchor coverage gap),
            // fall back to the coarsest major-region name (never leave count-only).
            if (grace_pname <= 0)
                grace_pname = find_nearest_region_pname(mrow->areaNo, mwx, mwz);
            if (grace_pname <= 0)
                grace_pname = find_nearest_major_region_pname(mrow->areaNo, mwx, mwz);
        }
        // Bug A: reproject injected dungeon rows onto the overworld so minor-
        // dungeon icons render. original_row_id == 0 ⇒ vanilla row (left as-is).
        bool was_royal = false;  // Leyndell Royal Capital (m11_00) → hide post-burn
        float ent_x = -1.0f, ent_z = -1.0f;  // overworld entrance if this row gets projected
        if (goblin::config::projectDungeons && all_rows[i].original_row_id != 0)
        {
            auto *prow = reinterpret_cast<from::paramdef::WORLD_MAP_POINT_PARAM_ST *>(
                new_param_file + data_offset);
            // Leyndell Ashen Capital (m35) describes the capital AFTER the Erdtree
            // burns. Tag these rows (before projection clobbers areaNo) so
            // apply_map_logic can gate their icon on StoryErdtreeOnFire instead of
            // showing them from the start.
            if (prow->areaNo == 35)
                g_ashen_rows.insert(static_cast<uint64_t>(all_rows[i].row_id));
            // Royal Capital (areaNo 11) is the inverse — visible until the burn,
            // then hidden. Tag before projection clobbers areaNo; registered below
            // (only if not clustered).
            //
            // areaNo 11 is NOT all Royal Capital. WorldMapLegacyConvParam
            // (LEGACY_CONV) splits src_area 11 by src_gx into three sub-maps:
            //   gx 0  = Royal Capital   (m11_00) → hide post-burn  ← only this
            //   gx 5  = Ashen Capital   (m11_05) → APPEARS post-burn (never hide)
            //   gx 10 = Shunning-Grounds(m11_10) → persists post-burn (never hide)
            // Gating on gridXNo==0 keeps the Subterranean Shunning-Grounds (and any
            // vanilla Ashen rows) visible after the Erdtree burns. gridXNo here is
            // still the source sub-grid (projection below clobbers it).
            was_royal = (prow->areaNo == 11 && prow->gridXNo == 0);
            if (project_dungeon_row_to_overworld(prow, &ent_x, &ent_z))
                reprojected_dungeons++;
            // A projected dungeon (catacomb/cave) whose own area had no named grace
            // or region stays nameless — but its pile sits at the overworld ENTRANCE.
            // Label it by the overworld region there (prow->areaNo is now 60/61).
            if (grace_pname <= 0 && ent_x >= 0.0f)
            {
                grace_pname = find_nearest_region_pname(prow->areaNo, ent_x, ent_z);
                if (grace_pname <= 0)
                    grace_pname = find_nearest_major_region_pname(prow->areaNo, ent_x, ent_z);
            }
        }
        new_wrapper_locs[i].row = all_rows[i].row_id;
        new_wrapper_locs[i].index = static_cast<int32_t>(i);

        // Record MFG-injected rows (vanilla rows have original_row_id 0) so
        // sanitize_injected_textids() can later strip any textId that the
        // expanded PlaceName FMG didn't end up containing.
        if (all_rows[i].original_row_id)
            g_injected_row_ptrs.push_back(new_param_file + data_offset);

        // Live-loot: remember lot-backed rows for refresh_loot_from_itemlot().
        if (all_rows[i].lotType != 0 && all_rows[i].lotId != 0)
        {
            uint8_t *rp = new_param_file + data_offset;
            g_lot_backed_rows.push_back({rp, all_rows[i].lotId, all_rows[i].lotType});
            g_lot_backed_set.insert(rp);

            // Live-loot icons: re-icon the marker to match the live item's
            // category (resolved in the filter loop, keyed by original id).
            auto ico = live_icon_override.find(all_rows[i].original_row_id);
            if (ico != live_icon_override.end())
                reinterpret_cast<from::paramdef::WORLD_MAP_POINT_PARAM_ST *>(rp)->iconId =
                    ico->second;
        }

        // Hybrid sub-area location naming (PRIMARY): overwrite the marker's location
        // line (textId2) with the height-aware sub-area name from generated::LOCATION_ALT
        // (MSB MapPoint/MapNameOverride volume containment, else nearest authored anchor in
        // 3D). The table only holds rows where the hybrid name differs from the baked one;
        // rows absent from it keep their baked textId2 = the FALLBACK (tile/nearest-grace
        // via resolve_location_id_at) for overworld / no-volume / no-anchor spots.
        // The value may be a synthetic compose id (generated::LOCATION_COMPOSE) for
        // duplicate-named sub-zones — goblin_messages builds its FMG string.
        if (all_rows[i].original_row_id)
        {
            auto *alt_end = generated::LOCATION_ALT + generated::LOCATION_ALT_COUNT;
            auto *alt = std::lower_bound(
                generated::LOCATION_ALT, alt_end, all_rows[i].original_row_id,
                [](const generated::LocationAlt &a, uint64_t id) { return a.row_id < id; });
            if (alt != alt_end && alt->row_id == all_rows[i].original_row_id)
            {
                auto *p = reinterpret_cast<from::paramdef::WORLD_MAP_POINT_PARAM_ST *>(
                    new_param_file + data_offset);
                // Overwrite the marker's LOCATION slot (slot picked at generation time:
                // textId2 for plain loot, textId3 for enemy-drops). slot 0 = no baseline
                // location → add one in the first free of textId2/textId3.
                int32_t *tid[9]  = {nullptr, &p->textId1, &p->textId2, &p->textId3, &p->textId4,
                                    &p->textId5, &p->textId6, &p->textId7, &p->textId8};
                unsigned int *fl[9] = {nullptr, &p->textDisableFlagId1, &p->textDisableFlagId2,
                                       &p->textDisableFlagId3, &p->textDisableFlagId4,
                                       &p->textDisableFlagId5, &p->textDisableFlagId6,
                                       &p->textDisableFlagId7, &p->textDisableFlagId8};
                uint8_t s = alt->slot;
                if (s >= 2 && s <= 8)
                {
                    *tid[s] = alt->textId2;   // hide-flag already set on this slot by the generator
                }
                else  // s == 0: add a location line where none existed (e.g. gestures)
                {
                    int add = (p->textId2 == -1) ? 2 : (p->textId3 == -1 ? 3 : 0);
                    if (add)
                    {
                        *tid[add] = alt->textId2;
                        *fl[add] = p->textDisableFlagId1;  // hide with the marker on pickup
                    }
                }
            }
        }

        // Kill display mode (bosses / hawks / NPC invaders): green checkmark
        // vs hide killed. Without this, rows baked with BOTH clearedEventFlagId
        // and textDisableFlagId hide all their text on kill and the icon
        // vanishes before the checkmark can ever show.
        auto cat = all_rows[i].category;
        if (cat == Category::WorldBosses || cat == Category::WorldSpiritspringHawks ||
            cat == Category::WorldHostileNPC)
        {
            auto *p = reinterpret_cast<from::paramdef::WORLD_MAP_POINT_PARAM_ST *>(
                new_param_file + data_offset);
            if (goblin::config::hideKilledBosses)
            {
                p->clearedEventFlagId = 0;  // no green checkmark, text hides → icon hides
            }
            else
            {
                p->textDisableFlagId1 = 0;  // keep green checkmark, don't hide text
                p->textDisableFlagId2 = 0;  // keep location text visible too
            }
        }

        auto *cp = new_param_file + data_offset;

        // Reserved POOL cluster row → collect its ptr for replan_clusters() and
        // skip the rest (it's not a section/royal/quest row). Parked off-page.
        if (all_rows[i].original_row_id == 0 && pool_row_ids.count(all_rows[i].row_id))
        {
            cp[0x20] = 99;
            g_cluster_pool.push_back(cp);
            continue;
        }

        // Register the row for the in-game per-section toggle (our injected rows
        // only; vanilla rows have original_row_id 0 and are never group-toggled).
        // areaNo here is final (post dungeon-reprojection) and pre piece/kindling
        // 99-hide, so it is the correct value to restore on a section "show".
        // ALL injected rows are section-registered now; replan_clusters() parks the
        // ones it folds into a pile and the eviction coordination keeps them parked.
        if (all_rows[i].original_row_id)
        {
            // Cluster grouping key: bucket by the nearest grace (overworld AND
            // dungeons WITH graces, e.g. Leyndell's 18 — so a high threshold lets
            // their sub-areas show as individual items instead of one mega-pile).
            // Only a GRACELESS projected dungeon (most catacombs/caves) folds into a
            // single entrance pile. Label = the nearest grace's region name.
            int grp_key, grp_pname = grace_pname;
            if (grace_idx >= 0)
                grp_key = grace_idx;                           // nearest-grace pile
            else if (ent_x >= 0.0f)
                grp_key = entrance_cluster_key(ent_x, ent_z);  // graceless dungeon → one entrance pile
            else
                grp_key = -1;                                  // no grace, not projected → exact
            g_section_rows.push_back({cp, section_of(all_rows[i].category),
                                      all_rows[i].category,
                                      cp[0x20], all_rows[i].is_piece,
                                      all_rows[i].is_kindling,
                                      static_cast<uint64_t>(all_rows[i].row_id),
                                      grp_key, grp_pname, grace_tab, ent_x, ent_z});
        }

        // Royal Capital row → register for post-burn hide.
        if (was_royal)
            g_royal_rows.push_back(cp);

        // Quest-NPC row → register for quest-aware gating (opt-in). nameId from
        // textId1 (= nameId + 700000000); only rows whose NPC is in the curated
        // quest-gate table.
        if (all_rows[i].category == Category::WorldQuestNPC)
        {
            auto *st = reinterpret_cast<from::paramdef::WORLD_MAP_POINT_PARAM_ST *>(cp);
            if (st->textId1 >= 700000000)
            {
                const auto *g = lookup_quest_gate(
                    static_cast<uint32_t>(st->textId1) - 700000000u);
                if (g)
                    g_quest_rows.push_back(
                        {cp, cp[0x20], {g->flags[0], g->flags[1], g->flags[2], g->flags[3]}});
            }
        }
    }
    } // map.inject.build_rows

    spdlog::info("[QUEST-NPC] registered {} marker rows from the {}-gate curated table "
                 "(quest-aware gating only covers these; other NPCs always show)",
                 g_quest_rows.size(), goblin::generated::QUEST_GATE_COUNT);

    if (goblin::config::projectDungeons)
        spdlog::info("Reprojected {} minor-dungeon rows onto the overworld (LEGACY_CONV)",
                     reprojected_dungeons);

    // Register Rune/Ember piece + kindling-spirit pointers for real-time tracking.
    // Pieces are CSWorldGeomMan-driven (collected::); kindling spirits are
    // SFX-region-driven (kindling::). Same hide-trick (areaNo = 99).
    int registered_pieces = 0, hidden_pieces = 0;
    int registered_kindling = 0, hidden_kindling = 0;
    {
    GOBLIN_BENCH("map.inject.register_pieces");
    for (size_t i = 0; i < all_rows.size(); i++)
    {
        size_t data_offset = data_start + i * PARAM_DATA_SIZE;
        auto *param_ptr = new_param_file + data_offset;
        uint64_t row_id = static_cast<uint64_t>(all_rows[i].row_id);

        // Clustering is now RUNTIME: a piece/kindling folded into a pile is kept
        // parked via the eviction coordination (is_section_hidden_ptr), so no
        // inject-time skip is needed. Pool rows aren't pieces/kindling.
        if (all_rows[i].is_piece)
        {
            collected::register_param_ptr(row_id, param_ptr);
            registered_pieces++;
            if (collected::is_row_collected(row_id))
            {
                param_ptr[0x20] = 99;  // areaNo = 99
                hidden_pieces++;
            }
        }
        else if (all_rows[i].is_kindling)
        {
            kindling::register_param_ptr(row_id, param_ptr);
            registered_kindling++;
            if (kindling::is_row_collected(row_id))
            {
                param_ptr[0x20] = 99;
                hidden_kindling++;
            }
        }
    }
    } // map.inject.register_pieces

    spdlog::info("Registered {} piece + {} kindling pointers ({} + {} hidden at inject)",
                 registered_pieces, registered_kindling, hidden_pieces, hidden_kindling);

    spdlog::debug("Swapping param_file pointer: {:p} -> {:p}", (void *)old_param_file, (void *)new_param_file);

    // Capture state for runtime toggle. Save original size before overwriting.
    g_file_ptr_ref = &file_ptr_ref;
    g_file_size_ref = &file_size_ref;
    g_vanilla_param_file = old_param_file;
    g_vanilla_param_size = file_size_ref;
    g_expanded_param_file = new_param_file;
    g_expanded_param_size = static_cast<int64_t>(param_file_size);

    file_ptr_ref = new_param_file;
    file_size_ref = static_cast<int64_t>(param_file_size);
    g_param_injection_active = true;

    // Seed per-section runtime gates from config and apply any that start
    // hidden (areaNo 99). Default is all-visible → no-op, zero regression.
    const bool sec_cfg[SECTION_COUNT] = {
        goblin::config::sectionEquipment, goblin::config::sectionKeyItems,
        goblin::config::sectionLoot,      goblin::config::sectionMagic,
        goblin::config::sectionQuest,     goblin::config::sectionReforged,
        goblin::config::sectionWorld,
    };
    for (int s = 0; s < SECTION_COUNT; s++)
    {
        g_section_visible[s].store(sec_cfg[s]);
        if (!sec_cfg[s])
            apply_section_visibility(static_cast<Section>(s), false);
    }
    spdlog::info("[SECTION] registered {} toggleable rows across {} sections",
                 g_section_rows.size(), SECTION_COUNT);

    // Seed per-category runtime gates from config (park-all): every category is
    // now injected; the ones disabled in config start parked (areaNo 99) but
    // stay live-toggleable from the menu. Default-enabled categories are a no-op.
    {
        int born_hidden = 0;
        for (int c = 0; c < NUM_CATEGORIES; c++)
        {
            bool on = is_category_enabled(static_cast<Category>(c));
            g_category_visible[c].store(on);
            if (!on)
            {
                apply_category_visibility(static_cast<Category>(c), false);
                born_hidden++;
            }
        }
        spdlog::info("[CATEGORY] seeded {} categories ({} parked at init)",
                     NUM_CATEGORIES, born_hidden);
    }

    // Seed per-category cluster opt-in from config::clusterExclude (menu display
    // only — the cluster plan above already read the same config directly).
    {
        int excluded = 0;
        for (int c = 0; c < NUM_CATEGORIES; c++)
        {
            bool on = category_clustered_cfg(static_cast<Category>(c));
            g_category_cluster[c].store(on);
            g_category_threshold[c].store(cluster_threshold_for_cfg(static_cast<Category>(c)));
            if (!on) excluded++;
        }
        if (excluded)
            spdlog::info("[CLUSTER] {} categories excluded from clustering", excluded);
    }

    // Seed the master on/off from the persisted config (menu 'Show icons' / F10).
    // Park everything now if it starts hidden (the watcher's change-detector
    // wouldn't fire, since prev == current at startup).
    g_icons_user_disabled.store(goblin::config::iconsHidden);
    if (goblin::config::iconsHidden)
        apply_master_visibility(false);

    // Build the initial cluster plan at RUNTIME from the rows we just registered.
    // enableClustering ⇔ collapsed (clusters shown); off ⇔ expanded (members shown).
    g_clusters_expanded.store(!goblin::config::enableClustering);
    replan_clusters();

    spdlog::debug("Injection complete: {} total rows", total_rows);
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


// ─── Runtime param toggle (drives the F10 personal show/hide) ────────

void goblin::set_param_injection_active(bool active)
{
    GOBLIN_BENCH("toggle.param_swap");
    if (!g_file_ptr_ref)
    {
        spdlog::warn("[TOGGLE] Param swap state not initialized — inject_map_entries() didn't run");
        return;
    }
    if (active == g_param_injection_active)
        return;
    if (active)
    {
        *g_file_ptr_ref = g_expanded_param_file;
        *g_file_size_ref = g_expanded_param_size;
    }
    else
    {
        *g_file_ptr_ref = g_vanilla_param_file;
        *g_file_size_ref = g_vanilla_param_size;
    }
    g_param_injection_active = active;
    spdlog::info("[TOGGLE] WorldMapPointParam -> {}", active ? "EXPANDED" : "VANILLA");
}

bool goblin::is_param_injection_active()
{
    return g_param_injection_active;
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
            .aob = "48 8B 05 ?? ?? ?? ?? 8B D1 48 85 C0 74 17 48 8B 88 80 00 00 00 48 85 C9",
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
            .aob = "48 8B 05 ?? ?? ?? ?? 33 DB 48 89 74 24",
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

bool goblin::ui::clustering_active() { return g_clustering_active; }
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
            .aob = "48 8B 05 ?? ?? ?? ?? 33 DB 48 89 74 24",
            .relative_offsets = {{3, 7}},
        }));
        fe_man_slot = reinterpret_cast<void **>(modutils::scan<void *>({
            .aob = "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 11 8B 80 3C 65 00 00",
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

bool goblin::is_ashen_capital_row(uint64_t row_id)
{
    return g_ashen_rows.count(row_id) != 0;
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
                { .aob = "48 83 EC 28 8B 12 85 D2" });
            g_orp_event_man_slot = reinterpret_cast<void **>(modutils::scan<void *>(
                { .aob = "48 8B 3D ?? ?? ?? ?? 48 85 FF ?? ?? 32 C0 E9",
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

// ── Fragment-eviction registry ───────────────────────────────────────
// areaNo lives at byte offset 0x20 (uint8) of WORLD_MAP_POINT_PARAM_ST — same
// offset hide_icon / collected use. 99 = off-page (no map-open cost); orig_area
// = the real page (60) to restore when the gate flag turns on.
struct FragGatedRow { uint8_t *ptr; uint8_t orig_area; uint32_t flag; };
static std::vector<FragGatedRow> g_frag_rows;

void goblin::register_fragment_gated_row(void *param_data, uint8_t original_area,
                                         uint32_t gate_flag)
{
    if (!param_data || gate_flag == 0) return;
    g_frag_rows.push_back(
        {reinterpret_cast<uint8_t *>(param_data), original_area, gate_flag});
}

int goblin::refresh_fragment_eviction()
{
    GOBLIN_BENCH("refresh.fragment_eviction");
    // Safety probe: AlwaysOn (6001) is ER's "always set" flag. If the flag API
    // can't even read it as true, the IsEventFlag resolution is wrong/unreliable
    // — parking would hide the WHOLE map. Bail out (restore everything to its
    // page, defer to the game's native eventFlagId gating) until the API is fixed.
    bool api_ok = orp_flag_set(6001);
    int evicted = 0;
    for (auto &r : g_frag_rows)
    {
        bool discovered = api_ok ? orp_flag_set(r.flag) : true;
        if (discovered)
        {
            // Don't un-hide a row a section toggle is keeping hidden (the
            // cold-API safety would otherwise stomp the user's section choice).
            if (r.ptr[0x20] == 99 && !goblin::is_section_hidden_ptr(r.ptr))
                r.ptr[0x20] = r.orig_area;  // discovered → restore
        }
        else
        {
            if (r.ptr[0x20] != 99) r.ptr[0x20] = 99;            // undiscovered → park
            evicted++;
        }
    }
    static int last_parked = -1;
    if (evicted != last_parked)
    {
        last_parked = evicted;
        spdlog::info("[FRAG-EVICT] {} gate-flagged rows; {} parked off-page (api_ok={})",
                     g_frag_rows.size(), evicted, api_ok);
    }
    return evicted;
}

// Thread 4 — inverse of the ashen gate. Once StoryErdtreeOnFire (flag 118) sets,
// the Leyndell Royal Capital is consumed by the Ashen Capital, so its markers
// must vanish. Park-only (the burn is permanent → never restore). Must run AFTER
// refresh_fragment_eviction so the hide wins over a fragment "discovered" restore.
int goblin::refresh_royal_eviction()
{
    GOBLIN_BENCH("refresh.royal_eviction");
    if (g_royal_rows.empty()) return 0;
    // Safety: only act when the flag API is warm (AlwaysOn 6001 reads true), else
    // leave royal rows to their normal gating — never blank on a cold API.
    if (!orp_flag_set(6001)) return 0;
    if (!orp_flag_set(118 /* goblin::flag::StoryErdtreeOnFire */)) return 0;  // not burned → visible
    int parked = 0;
    for (auto *p : g_royal_rows)
        if (p[0x20] != 99) { p[0x20] = 99; parked++; }
    static bool logged = false;
    if (!logged && parked)
    {
        logged = true;
        spdlog::info("[ROYAL-EVICT] Erdtree burned → parked {} Royal Capital rows",
                     g_royal_rows.size());
    }
    return parked;
}

// Thread 1 v1.5 — quest-aware quest-NPC gating. Park a registered quest-NPC row
// while its questline is inactive (none of its flags set), restore when active.
// Opt-in. Runs after section/category apply in the loop; respects them on restore.
int goblin::refresh_quest_npc_eviction()
{
    GOBLIN_BENCH("refresh.quest_npc_eviction");
    if (g_quest_rows.empty()) return 0;
    // Track enabled-edge so disabling the toggle (e.g. live from the overlay)
    // restores every row we parked instead of leaving it stuck off-page.
    static bool was_enabled = false;
    if (!goblin::config::questNpcQuestAware)
    {
        if (!was_enabled) return 0;            // already idle, nothing to undo
        int restored = 0;
        for (auto &r : g_quest_rows)
            if (r.ptr[0x20] == 99 && !goblin::is_section_hidden_ptr(r.ptr))
            { r.ptr[0x20] = r.orig_area; restored++; }
        was_enabled = false;
        return restored;
    }
    // Cold-API safety: if AlwaysOn (6001) can't be read true, leave rows as-is —
    // never blank every quest NPC because the flag API isn't warm yet.
    if (!orp_flag_set(6001)) return 0;
    was_enabled = true;
    int changed = 0;
    for (auto &r : g_quest_rows)
    {
        bool active = false;
        for (uint32_t f : r.flags)
            if (f && orp_flag_set(f)) { active = true; break; }
        if (!active)
        {
            if (r.ptr[0x20] != 99) { r.ptr[0x20] = 99; changed++; }
        }
        else if (r.ptr[0x20] == 99 && !goblin::is_section_hidden_ptr(r.ptr))
        {
            r.ptr[0x20] = r.orig_area; changed++;
        }
    }
    // Log the parked total on change (how many of the registered/covered quest-NPC
    // rows are currently hidden because their questline is inactive).
    int parked = 0;
    for (auto &r : g_quest_rows) if (r.ptr[0x20] == 99) parked++;
    static int last_parked = -1;
    if (parked != last_parked)
    {
        last_parked = parked;
        spdlog::info("[QUEST-NPC] {} of {} covered rows parked (inactive questline)",
                     parked, g_quest_rows.size());
    }
    return changed;
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

// Cluster depletion: when every flag-backed member of a cluster is collected, swap
// the cluster icon to the green CLUSTER_DONE glyph (else keep teal). Only while the
// clusters are SHOWN (collapsed); throttled (piles don't deplete fast). No RE —
// iconId is a mutable param field, like the areaNo flips.
int goblin::refresh_cluster_depletion()
{
    GOBLIN_BENCH("refresh.cluster_depletion");
    if (!g_clustering_active || g_clusters_expanded.load()) return 0;
    // DIAGNOSTIC: collapsed ⇒ EVERY member must be parked (areaNo 99). If any are
    // SHOWN, something un-parks them after the COLLAPSED apply (real bug). If 0,
    // the individual icons on the map are SPARSE markers (sub-threshold cells —
    // never clustered, shown by design) or excluded categories, NOT cluster members.
    {
        int shown = 0;
        for (const auto &m : g_cluster_members)
            if (m.ptr[0x20] != 99) shown++;
        static int last_shown = -1;
        if (shown != last_shown)
        {
            last_shown = shown;
            spdlog::info("[CLUSTER-CHECK] {} of {} members SHOWN while collapsed "
                         "(should be 0; >0 = un-park bug, 0 = the loose icons are "
                         "sub-threshold/excluded, not members)",
                         shown, g_cluster_members.size());
        }
    }
    if (!orp_flag_set(6001)) return 0; // cold API → never mis-deplete
    using clock = std::chrono::steady_clock;
    static clock::time_point last{};
    auto now = clock::now();
    if (now != clock::time_point{} && now - last < std::chrono::milliseconds(1000))
        return 0;
    last = now;
    // Diagnostic (debug_logging only): verify the AOB-anchored player map-pos reader
    // resolves + reads sane marker-space coords, ahead of distance-adaptive clustering.
    if (goblin::config::debugLogging)
    {
        int parea; float pwx, pwz;
        if (goblin::get_player_map_pos(parea, pwx, pwz))
            spdlog::info("[PLAYER] map-pos area={} world=({:.1f},{:.1f}) [tile {},{}]",
                         parea, pwx, pwz, static_cast<int>(pwx / 256.0f), static_cast<int>(pwz / 256.0f));
        else
            spdlog::info("[PLAYER] map-pos unavailable (AOB unresolved or chain faulted)");
    }
    int changed = 0, with_flags = 0, depleted_n = 0;
    for (auto &c : g_clusters)
    {
        if (c.members.empty()) continue; // no collectible members → never "done"
        with_flags++;
        // Count collected members — by event flag (plain loot) OR by piece/kindling
        // row-id tracking (Reforged items have no event flag). Drives both the
        // done-icon swap and the live REMAINING count.
        int collected_n = 0;
        for (const auto &m : c.members)
        {
            bool got = (m.flag && orp_flag_set(m.flag)) ||
                       (m.is_piece && goblin::collected::is_row_collected(m.row_id)) ||
                       (m.is_kindling && goblin::kindling::is_row_collected(m.row_id));
            if (got) collected_n++;
        }
        bool depleted = (collected_n == static_cast<int>(c.members.size()));
        if (depleted) depleted_n++;
        uint16_t want = depleted ? goblin::generated::CLUSTER_DONE_ICON_ID
                                 : goblin::generated::CLUSTER_ICON_ID;
        auto *st = reinterpret_cast<from::paramdef::WORLD_MAP_POINT_PARAM_ST *>(c.ptr);
        if (st->iconId != want) { st->iconId = want; changed++; }
        // Bug fix: the label count was frozen at replan time (= TOTAL members, incl.
        // already-collected loot), so it never went down as you looted. Show the
        // live REMAINING = total - collected. count_textid encodes the total.
        if (g_cluster_debug.load())
        {
            int total = c.count_textid - CLUSTER_TEXTID_BASE;
            int remaining = total - collected_n;
            if (remaining < 0) remaining = 0;
            int want_tid = CLUSTER_TEXTID_BASE + std::min(remaining, CLUSTER_MAX_COUNT);
            if (st->textId2 != want_tid) { st->textId2 = want_tid; changed++; }
        }
    }
    // Diagnostic: log when the depleted count changes (and the startup state) so we
    // can tell apart "no collect-flags captured" vs "swapped but icon not re-read".
    static int last_depleted = -1;
    if (depleted_n != last_depleted)
    {
        last_depleted = depleted_n;
        spdlog::info("[CLUSTER-DEPLETE] {} clusters, {} with collect-flags, {} depleted, "
                     "{} icon-swapped this pass", g_clusters.size(), with_flags,
                     depleted_n, changed);
    }
    return changed;
}

// Per-category uncollected census — feeds the overlay's "<remaining>/<total>"
// badge next to each category. Gated to "menu on-screen" + throttled to 1s so the
// 9296-row flag sweep is free when the panel is closed. Collected detection mirrors
// cluster depletion: plain loot via textDisableFlagId1 + orp_flag_set, Reforged
// pieces/kindling via row-id tracking. Categories with no collectible rows
// (graces/NPCs/regions) cache remaining = -1 so the overlay draws no badge.
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
    using ST = from::paramdef::WORLD_MAP_POINT_PARAM_ST;
    int collectible[NUM_CATEGORIES] = {0};
    int looted[NUM_CATEGORIES]      = {0};
    for (const auto &r : g_section_rows)
    {
        int ci = static_cast<int>(r.cat);
        if (ci < 0 || ci >= NUM_CATEGORIES) continue;
        uint32_t f = reinterpret_cast<ST *>(r.ptr)->textDisableFlagId1;  // collect flag
        if (!(f || r.is_piece || r.is_kindling)) continue;  // not a collectible row
        collectible[ci]++;
        bool taken = (f && orp_flag_set(f)) ||
                     (r.is_piece && goblin::collected::is_row_collected(r.row_id)) ||
                     (r.is_kindling && goblin::kindling::is_row_collected(r.row_id));
        if (taken) looted[ci]++;
    }
    for (int c = 0; c < NUM_CATEGORIES; c++)
    {
        g_cat_total[c].store(collectible[c]);
        g_cat_remaining[c].store(collectible[c] > 0 ? (collectible[c] - looted[c]) : -1);
    }
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
// Reads each lot-backed marker's source ItemLotParam row from memory and sets
// textDisableFlagId1 to the lot's current getItemFlagId. Because we read the
// LOADED regulation (vanilla, Randomizer, any file mod), the marker hides on
// the actual light-point pickup regardless of which item the lot now gives.
// One-shot at init: the flag VALUE in a row is static post-load; the engine
// then evaluates textDisableFlagId1 live every frame. See reference_cleared_badge
// / the randomizer-compat research. Gated by config::liveLootFlags/Labels.
void goblin::refresh_loot_from_itemlot()
{
    const bool do_flags  = goblin::config::liveLootFlags;
    const bool do_labels = goblin::config::liveLootLabels;
    const bool do_anon   = goblin::config::anonymousLoot;
    if ((!do_flags && !do_labels && !do_anon) || g_lot_backed_rows.empty())
        return;
    GOBLIN_BENCH("map.live_loot.total");

    LotReader lots;
    lots.init();
    if (!lots.ok())
    {
        spdlog::warn("[LIVE-LOOT] ItemLotParam not available — skipped");
        return;
    }
    auto read_row = [&](uint32_t lot_id, uint8_t lot_type) { return lots.row(lot_id, lot_type); };

    int updated = 0, relabeled = 0, not_found = 0, no_flag = 0;
    for (auto &lr : g_lot_backed_rows)
    {
        RawItemLotRow *row = read_row(lr.lotId, lr.lotType);
        if (!row) { not_found++; continue; }
        auto *p = reinterpret_cast<from::paramdef::WORLD_MAP_POINT_PARAM_ST *>(lr.ptr);

        if (do_flags)
        {
            uint32_t flag = *reinterpret_cast<uint32_t *>(row->b + 0x80);  // lot-wide getItemFlagId
            if (flag == 0)
            {
                // Fall back to the per-slot flag only for single-item lots
                // (lotItemId02 @0x04 == 0), else a slot-1 award would hide the
                // marker while other loot remains.
                int32_t item2 = *reinterpret_cast<int32_t *>(row->b + 0x04);
                if (item2 == 0)
                    flag = *reinterpret_cast<uint32_t *>(row->b + 0x60);  // getItemFlagId01
            }
            if (flag)
            {
                // Hide the WHOLE marker on the live pickup flag. A loot marker
                // carries the same disable flag on every populated text line
                // (item line + location line; verified uniform across all
                // lot-backed rows) — the engine only drops the icon once ALL
                // its lines are disabled. Rewriting just slot 1 left the
                // location line (slot 2) pinned to the stale baked flag, which
                // never fires under a regulation that reassigns flags (the
                // randomizer), so the marker never disappeared. Update every
                // line that had a (non-zero) disable flag baked.
                int *tids[8] = {&p->textId1, &p->textId2, &p->textId3, &p->textId4,
                                &p->textId5, &p->textId6, &p->textId7, &p->textId8};
                unsigned int *fls[8] = {&p->textDisableFlagId1, &p->textDisableFlagId2,
                                        &p->textDisableFlagId3, &p->textDisableFlagId4,
                                        &p->textDisableFlagId5, &p->textDisableFlagId6,
                                        &p->textDisableFlagId7, &p->textDisableFlagId8};
                for (int i = 0; i < 8; ++i)
                    if (*tids[i] > 0 && *fls[i] != 0)
                        *fls[i] = flag;
                updated++;
            }
            else no_flag++;
        }

        if (do_anon)
        {
            // Spoiler-free mode: replace the item name with the generic
            // localized label. Same slot guard as the live relabel below — only
            // overwrite an actual item-name slot, never a location/enemy slot.
            int32_t cur = p->textId1;
            if (cur >= 50000000 && cur < 600000000 && cur != ANON_LABEL_TEXTID)
            {
                p->textId1 = ANON_LABEL_TEXTID;
                relabeled++;
            }
        }
        else if (do_labels)
        {
            // Relabel the item-name slot (textId1) to whatever the lot now
            // gives. Guard: only touch textId1 if it already holds an
            // item-name encoded id (50M..600M item bands) — never clobber a
            // location (<50M) or enemy/npc (>=700M) slot. The encoded id maps
            // into the full item-name space copied into PlaceName at init.
            int32_t cur = p->textId1;
            if (cur >= 50000000 && cur < 600000000)
            {
                int32_t item_id = *reinterpret_cast<int32_t *>(row->b + 0x00);  // lotItemId01
                int32_t cat     = *reinterpret_cast<int32_t *>(row->b + 0x20);  // lotItemCategory01
                int32_t enc = encode_live_item(item_id, cat);
                if (item_id > 0 && enc > 0 && enc != cur)
                {
                    p->textId1 = enc;
                    relabeled++;
                }
            }
        }
    }
    spdlog::info("[LIVE-LOOT] {} hide-flags, {} relabels set from live ItemLotParam "
                 "({} lots not found, {} no flag, {} lot-backed total)",
                 updated, relabeled, not_found, no_flag, g_lot_backed_rows.size());
}

void goblin::menu_auto_toggle_loop()
{
    bool prev_user_disabled = g_icons_user_disabled.load();

    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Show the native banner when the user flips the master-off via hotkey.
        // Driven from this thread (the param-state owner) so all game-state
        // mutation happens in one place.
        bool user_disabled_now = g_icons_user_disabled.load();
        if (user_disabled_now != prev_user_disabled)
        {
            apply_master_visibility(!user_disabled_now);  // hide first (instant areaNo park)
            show_toggle_banner(!user_disabled_now);       // banner after (off the hot path)
            prev_user_disabled = user_disabled_now;
        }

        // Per-section toggle requests posted by the hotkey thread. Apply the
        // areaNo flips on the live blob and persist the choice here (single
        // owner of game-state mutation), then fire whatever toast was queued.
        int areq = g_section_apply_req.exchange(-1);
        if (areq >= 0 && areq < SECTION_COUNT)
        {
            apply_section_visibility(static_cast<Section>(areq), g_section_visible[areq].load());
            goblin::save_section_states(goblin::config_ini_path());
        }

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

        // Per-category toggle requests posted by the overlay menu. Applied here
        // (single owner of game-state mutation). Scans dirty flags so a "toggle
        // whole section" that flips many at once all land. Persistence to INI is
        // the Save button's job (P3c) — this just applies live.
        for (int c = 0; c < NUM_CATEGORIES; c++)
            if (g_category_dirty[c].exchange(false))
                apply_category_visibility(static_cast<Category>(c),
                                          g_category_visible[c].load());

        // Menu "Save" → persist current visibility to the ini (file I/O here, off
        // the render thread).
        if (g_save_req.exchange(false))
            persist_settings();

        // Danger zone: re-seed config from defaults + write the ini (off the
        // render thread). Runtime visibility is unchanged until a restart.
        if (g_reset_defaults_req.exchange(false))
            goblin::reset_to_defaults_and_save(goblin::config_ini_path());

        // Distance-adaptive: keep the plan tracking the player. Re-planning AT map-open
        // wouldn't show until the NEXT open (the game reads the rows at open time), so
        // instead re-plan IN-WORLD as the player moves — then any open shows current
        // bands. Throttled; only when the player moved a meaningful distance (or
        // changed map area) since the last plan, and only while the map is CLOSED
        // (don't thrash mid-view). Feature-gated.
        if (goblin::config::clusterDistanceAdaptive && goblin::config::enableClustering)
        {
            static std::chrono::steady_clock::time_point s_last_chk{};
            static int s_last_area = -999;
            static float s_last_x = 0, s_last_z = 0;
            static int s_last_gx = -999, s_last_gz = -999;
            auto now2 = std::chrono::steady_clock::now();
            if ((now2 == std::chrono::steady_clock::time_point{} || now2 - s_last_chk > std::chrono::milliseconds(2000))
                && !goblin::world_map_open())
            {
                s_last_chk = now2;
                int pa, pgx = -1, pgz = -1; float px, pz;
                if (goblin::get_player_map_pos(pa, px, pz, &pgx, &pgz))
                {
                    // re-plan on area change, or — underground, where px/pz are leaf-
                    // block-local garbage — on TILE change (reliable from MapId); else
                    // (overworld) once the player drifts past ~half the near radius.
                    float move_gate = std::max(256.0f, goblin::config::clusterNearRadius * 128.0f);
                    bool tile_changed = (pgx != s_last_gx || pgz != s_last_gz);
                    float moved = (pa != s_last_area) ? 1e9f : std::sqrt(
                        (px - s_last_x) * (px - s_last_x) + (pz - s_last_z) * (pz - s_last_z));
                    if (moved > move_gate || tile_changed)
                    {
                        s_last_area = pa; s_last_x = px; s_last_z = pz;
                        s_last_gx = pgx; s_last_gz = pgz;
                        g_cluster_replan_dirty.store(true);
                    }
                }
            }
        }

        // Runtime cluster re-plan (enable / soft-hard / threshold / exclude). Rebuilds
        // the whole plan from the live rows into the pool, then applies the view.
        // ONLY while the map is CLOSED: replan's teardown briefly un-parks every old
        // member then re-parks them into new piles, so applying it mid-render leaves
        // PHANTOM / duplicate icons. Deferred (keep the dirty flag set) until the map
        // closes, then it's applied for the next open — matching "reopen to apply".
        // Likewise the expand/collapse re-park.
        bool map_open_now = goblin::world_map_open();
        if (!map_open_now && g_cluster_replan_dirty.load())
        {
            g_cluster_replan_dirty.store(false);
            replan_clusters();
        }
        // Cluster expand/collapse + debug-label flips (areaNo / textId on the live blob).
        if (!map_open_now && g_cluster_expand_dirty.load())
        {
            g_cluster_expand_dirty.store(false);
            apply_cluster_expanded(g_clusters_expanded.load());
        }
        // Bubble show/hide now PARKS the cluster glyph (areaNo), not just a textId
        // flip — so gate it to map-closed like expand/replan (parking the live blob
        // while the map renders risks the duplicate/oversized race). Applies on next
        // open. Keep the dirty bit set until then so the toggle isn't lost.
        if (!map_open_now && g_cluster_debug_dirty.load())
        {
            g_cluster_debug_dirty.store(false);
            apply_cluster_debug(g_cluster_debug.load());
        }

        // (Player-position probe logging removed — proximity clustering paused:
        // live player world coords are unstable/chunk-local and the map-cursor /
        // live-refresh paths both need the blocked CSWorldMapMenu RE. The reader
        // get_player_world_pos() is kept dormant for when that RE lands.)

        // (Master on/off is applied via apply_master_visibility above — the live
        // areaNo lever — not the param-file swap, which reflected only per-region
        // as the game re-read the file. set_param_injection_active stays for the
        // ERSC-hosting revert path.)
    }
}
