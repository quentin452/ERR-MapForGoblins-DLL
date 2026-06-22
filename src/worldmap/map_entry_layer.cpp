#include "map_entry_layer.hpp"

#include "category_meta.hpp"
#include "goblin_map_data.hpp" // MAP_ENTRIES / MAP_ENTRY_COUNT / MapEntry / Category
#include "goblin_inject.hpp"   // marker_world_pos / category_visible / read_event_flag / census
#include "goblin_logic.hpp"    // map_fragment_flag
#include "goblin_collected.hpp" // is_original_row_collected (piece graying)
#include "goblin_kindling.hpp"  // is_row_collected (kindling graying)
#include "goblin_markers.hpp"   // category_name (census log)
#include "goblin/goblin_map_flags.hpp" // flag::Story* (secondary story gate)
#include "goblin_bench.hpp"            // GOBLIN_BENCH scoped timers

#include <spdlog/spdlog.h>

#include <array>
#include <string>
#include <unordered_set>

namespace goblin::worldmap
{
namespace
{
constexpr int NUM_CAT = static_cast<int>(goblin::generated::Category::WorldInteractables) + 1;

std::array<std::vector<Marker>, NUM_CAT> g_buckets;
bool g_built = false;

// Post-event "secondary story flag" for a tile (legacy SetSecondaryFlags + Ashen Capital).
// A marker on a post-event tile — or anywhere in Leyndell, Ashen Capital (area 35) — only
// appears once its story flag is set (the burnt/changed map state). Returns 0 = no gate.
// Mirrors goblin_logic.cpp GetMapFragment's SetSecondaryFlags() tile table + apply_map_logic()
// is_ashen_capital_row(), but UNCONDITIONAL (legacy coupled SetSecondaryFlags to
// require_map_fragments; this is a story-spoiler gate, correct regardless). MapTile(X,Y,Z)
// = (areaNo, gridXNo, gridZNo).
int secondary_story_flag(int area, int gx, int gz)
{
    namespace flag = goblin::flag;
    if (area == 35) // Leyndell, Ashen Capital — whole area is the post-Erdtree-burn state
        return flag::StoryErdtreeOnFire;
    if (gz != 0)
        return 0;
    if ((area == 11 && gx == 5) || (area == 19 && gx == 0)) // post-burn Leyndell / Chapel m19
        return flag::StoryErdtreeOnFire;
    if ((area == 21 && (gx == 0 || gx == 1 || gx == 2)) || (area == 22 && gx == 0))
        return flag::StoryCharmBroken;
    if (area == 20 && gx == 1)
        return flag::StorySealingTreeBurnt;
    return 0;
}

// Inverse of secondary_story_flag: a PRE-event tile that must DISAPPEAR once the story
// flag fires. Leyndell Royal Capital (area 11, m11_00) is consumed by the Ashen Capital
// when the Erdtree burns (StoryErdtreeOnFire), so its markers hide post-burn — EXCEPT the
// post-burn tile (11, gx5, gz0) which secondary_story_flag already gates to SHOW after the
// burn (never both gates on one tile). Mirrors the legacy refresh_royal_eviction, which
// only ever ran on native injected rows (never active in overlay-only mode).
int hide_when_story_flag(int area, int gx, int gz)
{
    namespace flag = goblin::flag;
    if (area == 11 && !(gx == 5 && gz == 0))
        return flag::StoryErdtreeOnFire;
    return 0;
}

// Build every category's marker cache in ONE pass over MAP_ENTRIES (9k rows). Same
// world-projection + group classification as the grace layer.
void build_buckets()
{
    if (g_built)
        return;
    g_built = true;
    GOBLIN_BENCH("build.buckets");
    namespace gen = goblin::generated;
    for (size_t i = 0; i < gen::MAP_ENTRY_COUNT; ++i)
    {
        const gen::MapEntry &e = gen::MAP_ENTRIES[i];
        int c = static_cast<int>(e.category);
        if (c < 0 || c >= NUM_CAT)
            continue;
        const auto &d = e.data;
        int ga;
        float wx, wz;
        goblin::marker_world_pos(d.areaNo, d.gridXNo, d.gridZNo, d.posX, d.posZ, ga, wx, wz,
                                 /*conv_underground=*/true);
        int grp = goblin::marker_group_from(d.areaNo, ga);
        int pname = -1;
        int ckey = goblin::marker_cluster_key(d.areaNo, d.gridXNo, d.gridZNo, d.posX, d.posZ,
                                              &pname);
        int frag = goblin::marker_fragment_flag(d.areaNo, d.gridXNo, d.gridZNo, d.posX, d.posZ);
        // Lot-backed loot: resolve the LIVE pickup flag (baked textDisableFlagId1 is
        // stale under ERR/randomizer) so collected loot grays + the census decrements.
        int collected_flag = (int)goblin::resolve_loot_flag(e.lotId, e.lotType, d.textDisableFlagId1);
        goblin::diag_loot_flags(e.lotId, e.lotType, d.textDisableFlagId1, c, d.textId1);
        Marker m{wx, wz, grp, (int)d.areaNo, c, ckey, pname, d.textId1,
                 category_color(c), category_icon_key(c), frag,
                 e.row_id, (int)d.clearedEventFlagId, collected_flag};
        m.secondary_flag = secondary_story_flag(d.areaNo, d.gridXNo, d.gridZNo);
        m.hide_when_flag = hide_when_story_flag(d.areaNo, d.gridXNo, d.gridZNo);
        // Raw param coords for the engine's live projection (config live_projection).
        m.raw_area = d.areaNo; m.raw_gx = d.gridXNo; m.raw_gz = d.gridZNo;
        m.raw_px = d.posX; m.raw_pz = d.posZ;
        // Lot-backed loot (for anonymous_loot spoiler mode). Pieces/kindling are
        // geom/SFX-tracked, never a lot — exclude them (mirrors goblin_inject's is_lot).
        const bool piece = e.category == gen::Category::ReforgedRunePieces ||
                           e.category == gen::Category::ReforgedEmberPieces ||
                           e.category == gen::Category::LootMaterialNodes;
        const bool kind = e.category == gen::Category::WorldKindlingSpirits;
        m.lot_backed = !piece && !kind && e.lotId != 0 && e.lotType != 0;
        g_buckets[c].push_back(m);
    }
}
} // namespace

MapEntryLayer::MapEntryLayer(int category) : cat_(category)
{
    const char *k = category_icon_key(category);
    name_ = k ? k : "marker";
}

bool MapEntryLayer::visible() const
{
    return goblin::ui::category_visible(cat_) &&
           goblin::ui::section_visible(goblin::ui::category_section(cat_));
}

const std::vector<Marker> &MapEntryLayer::markers() const
{
    build_buckets();
    return g_buckets[cat_];
}

void refresh_overlay_census()
{
    namespace gen = goblin::generated;
    GOBLIN_BENCH("refresh.overlay_census");
    build_buckets(); // ensure the overlay markers exist (one-time)

    static int s_prev_looted[NUM_CAT];
    static bool s_logged_once = false;

    for (int c = 0; c < NUM_CAT; ++c)
    {
        // Graces aren't census items (deduped against the native pin) → no badge.
        if (c == static_cast<int>(gen::Category::WorldGraces))
        {
            goblin::ui::set_category_census(c, 0, 0);
            continue;
        }
        // Piece/kindling rows are geom/SFX-tracked (no textDisableFlag) — their
        // collectibility is known by category, same as the native census.
        const bool piece = c == static_cast<int>(gen::Category::ReforgedRunePieces) ||
                           c == static_cast<int>(gen::Category::ReforgedEmberPieces) ||
                           c == static_cast<int>(gen::Category::LootMaterialNodes);
        const bool kind = c == static_cast<int>(gen::Category::WorldKindlingSpirits);

        int total = 0, looted = 0;
        if (piece || kind)
        {
            // Geom/SFX-tracked: each row is its own collectible, keyed by row_id (these
            // rows have no per-item event flag, so the flag-dedup path can't apply).
            for (const Marker &m : g_buckets[c])
            {
                ++total;
                const bool done = piece ? goblin::collected::is_original_row_collected(m.row_id)
                                        : goblin::kindling::is_row_collected(m.row_id);
                if (done)
                    ++looted;
            }
        }
        else
        {
            // Flag-based: count by DISTINCT event flag, not per row. ERR data shares one
            // collect/clear flag across many markers (e.g. ~61 Fortunes share ~8 flags),
            // so a per-row count over-reports (one pickup would mark ~18 rows taken). A
            // flag = collected_flag (loot pickup) else cleared_flag (boss/hawk kill). For
            // categories with unique flags this equals the row count, so nothing changes
            // there; it only corrects the shared-flag categories. SAME flags the renderer
            // grays on, so the badge tracks the map (modulo shared-flag groups, which the
            // game itself can't disambiguate).
            std::unordered_set<int> all, taken;
            for (const Marker &m : g_buckets[c])
            {
                const int flag = m.collected_flag ? m.collected_flag : m.cleared_flag;
                if (!flag)
                    continue; // not a counted item (NPC, spirit spring, stake, …)
                all.insert(flag);
                if (goblin::ui::read_event_flag((uint32_t)flag))
                    taken.insert(flag);
            }
            total = (int)all.size();
            looted = (int)taken.size();
            // DEBUG: on the FIRST publish, sample a few still-UNSET flags per category.
            // On a 100% save these should be ~none — any here are flags we check that the
            // game never sets (wrong/non-pickup flag) → the over-count to investigate.
            if (!s_logged_once && total - looted > 0)
            {
                std::string s;
                int n = 0;
                for (int f : all)
                    if (!taken.count(f) && n++ < 10)
                        s += std::to_string(f) + " ";
                spdlog::info("[CENSUS-UNSET] cat {:2} '{}' {} unset; sample flags: {}",
                             c, goblin::markers::category_name(static_cast<gen::Category>(c)),
                             total - looted, s);
            }
        }
        goblin::ui::set_category_census(c, total, looted);

        // [OVERLAY-CENSUS] log: full dump on the first publish, then a line whenever a
        // category's looted count changes (so a pickup is visible in the log).
        if ((!s_logged_once || s_prev_looted[c] != looted) && total > 0)
            spdlog::info("[OVERLAY-CENSUS] cat {:2} '{}' remaining={}/{} (looted {} -> {})",
                         c, goblin::markers::category_name(static_cast<gen::Category>(c)),
                         total - looted, total, s_prev_looted[c], looted);
        s_prev_looted[c] = looted;
    }
    s_logged_once = true;
}
} // namespace goblin::worldmap
