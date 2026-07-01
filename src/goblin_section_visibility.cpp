#include "goblin_inject.hpp"
#include "goblin_inject_shared.hpp"
#include "goblin_config.hpp"
#include "goblin_map_data.hpp"
#include "goblin_markers.hpp"
#include "modutils.hpp"
#include "goblin_bench.hpp"
#include "goblin_overlay_render_loader.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <vector>

//
// Per-section runtime visibility + marker-clustering state — split out of
// goblin_inject.cpp 2026-07-01 (docs/plans/goblin_inject_refactor_plan.md PR 3).
// Pure relocation, no logic changes. Non-contiguous in the original file: the
// visibility+clustering globals/is_section_hidden_ptr, the cluster-config
// helpers + seed_runtime_gates, the visibility/clustering half of "Overlay
// control API" (leaving save/reset/toast/injected_row_ptrs behind, those are
// unrelated concerns that happened to sit in the same banner section), and
// the per-category census all depend on each other but on nothing outside
// this set — audit found zero leakage into grace anchors, TutorialParam,
// player-position, native-grace-suppression, or the already-moved loot-
// resolve/icon-harvest/item-classify files. "Marker clustering" section
// (CLUSTER_CELL/CLUSTER_POOL_SIZE/etc.) includes some genuinely dead
// constants (replan_clusters() referenced only in comments, never called —
// likely superseded by the spatial-grid-opti work) — moved as-is, not
// cleaned up (pure relocation, that's a separate future cleanup).
//
// menu_auto_toggle_loop() (stays in goblin_inject.cpp, a generic watcher
// dispatching toast/save/reset/visibility-apply) directly read
// g_icons_user_disabled and did g_section_apply_req.exchange(-1) — now goes
// through 2 tiny accessors declared in goblin_inject_shared.hpp:
// icons_user_disabled() / take_section_apply_req(). orp_flag_set (needed by
// refresh_category_census) reuses PR 0's existing goblin_inject_shared.hpp
// declaration, no new plumbing there.
//

using Category = goblin::generated::Category;

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
    case Category::WorldDivineTower:
    case Category::WorldEvergaol:
    case Category::WorldMinorErdtree:
    case Category::WorldGrandLift:
    case Category::WorldDungeon:
    case Category::WorldLegacyDungeon:
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

// One injected, section-toggleable row in the expanded blob. orig_area is the
// row's FINAL areaNo (post dungeon-reprojection) so a show restores the right
// page. Piece/kindling rows may be independently 99-hidden when collected — a
// section "show" must not resurrect those.

// Number of marker categories (enum has no COUNT sentinel; keep in sync).
static constexpr int NUM_CATEGORIES = static_cast<int>(Category::WorldLegacyDungeon) + 1;
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


// Master-off intent set by the toggle hotkey. When true the user has
// explicitly hidden the icons, so the auto-toggle must keep the table vanilla
// even while the world map is open. Shared between the hotkey and watcher
// threads; a lone bool flag is fine, but use atomic for correctness.
static std::atomic<bool> g_icons_user_disabled{false};

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

// Sync the live section/category visibility into the config vars, then write the
// ini. The menu is now the category authority, so drop the showAll shortcut
// (else it would force every category back on at next load).
void persist_settings()
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

// Per-category uncollected census — feeds the overlay's "<remaining>/<total>"
// badge next to each category. Gated to "menu on-screen" + throttled to 1s so the
// 9296-row flag sweep is free when the panel is closed. Collected detection mirrors
// cluster depletion: plain loot via textDisableFlagId1 + orp_flag_set, Reforged
// pieces/kindling via row-id tracking. Categories with no collectible rows
// (graces/NPCs/regions) cache remaining = -1 so the overlay draws no badge.
// Overlay-only census: implemented in the worldmap module (it owns the marker buckets), which is
// render-side under Slice C's split — goes through goblin_overlay_render_loader instead of a
// direct forward-declared call so this keeps working once GOBLIN_OVERLAY_HOTRELOAD=ON is real.

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
    goblin::overlay_render_loader::call_refresh_overlay_census();
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

// Accessors for goblin_inject.cpp's menu_auto_toggle_loop — see goblin_inject_shared.hpp.
bool icons_user_disabled() { return g_icons_user_disabled.load(); }
int take_section_apply_req() { return g_section_apply_req.exchange(-1); }
