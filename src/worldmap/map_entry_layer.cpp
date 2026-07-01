#include "map_entry_layer.hpp"

#include "category_meta.hpp"
#include "loot_disk.hpp"       // disk-MSB loot source (DiskTreasure / load_disk_treasures) + load_quest_npcs
#include "quest_npc_layer.hpp" // QuestFallbackNpc + set_quest_fallback_npcs (runtime quest fallback)
#include "goblin_quest_steps.hpp" // goblin::generated::QUEST_BROWSER (hand-authored fail_flags to join on)
#include "goblin_config.hpp"   // config::lootFromDiskMsb
#include "goblin_map_data.hpp" // MAP_ENTRIES / MAP_ENTRY_COUNT / MapEntry / Category
#include "goblin_world_feature_models.hpp" // WORLD_FEATURE_MODELS (asset-model World features)
#include "goblin_inject.hpp"   // marker_world_pos / category_visible / read_event_flag / census
#include "goblin_logic.hpp"    // map_fragment_flag
#include "goblin_collected.hpp" // is_original_row_collected (piece graying)
#include "goblin_kindling.hpp"  // is_row_collected (kindling graying)
#include "goblin_markers.hpp"   // category_name (census log)
#include "goblin_messages.hpp"  // lookup_text_utf8 ([BAKED-RESIDUAL] self-describing names)
#include "goblin_config.hpp"    // config::suppressNativeBosses
#include "goblin/goblin_map_flags.hpp" // flag::Story* (secondary story gate)
#include "goblin_bench.hpp"            // GOBLIN_BENCH scoped timers
#include "from/params.hpp"                            // live WorldMapPointParam (boss source)
#include "from/paramdef/WORLD_MAP_POINT_PARAM_ST.hpp" // WORLD_MAP_POINT_PARAM_ST

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <atomic>
#include <string>
#include <thread>
#include <unordered_map>
#include <map>
#include <mutex>
#include <unordered_set>

namespace goblin::worldmap
{
namespace
{
constexpr int NUM_CAT = static_cast<int>(goblin::generated::Category::WorldInteractables) + 1;

std::array<std::vector<Marker>, NUM_CAT> g_buckets;

// MSB EntityID -> projected world position, for QuestNpcLayer's entity_world_pos().
// Built once per build_buckets_impl() pass (same lifecycle as g_buckets, cleared and
// repopulated together) from disk_enemies/disk_collectibles -- NOT a second parse.
struct EntityPos { float wx, wz; int group; };
std::unordered_map<uint32_t, EntityPos> g_entity_pos;

// [SKIPPED] diag (config diag_loot_pos): disk placements PARSED from the mod's files but NOT drawn,
// aggregated by reason across all passes (each pass adds its local filtered counts at its log line).
// "shown vs skipped" visibility — the inverse of [COVERAGE]. Reset at the top of build_buckets_impl.
struct SkipTally {
    int unclassified = 0;   // item type genuinely unknown → no category bucket
    int dedup = 0;          // already placed by another pass (treasure-dup / (entity,lot) / pos dedup)
    int no_anchor = 0;      // no lot / no positionable MSB entity to place it
    int by_design = 0;      // intentionally suppressed: rune/ember GEOM-tracked, anti-flood clutter,
                            // respawnable/no-one-time-flag enemy drops
    int merchant_phantom = 0; // shop-∞ bake fallback at the tile corner (dropped)
    int dummy_inert = 0;    // cut DummyAsset (no EntityID → not reachable)
};
SkipTally g_skip;

// ── [WATCH] per-lot trace (config diag_loot_pos) ──────────────────────────────
// Follow specific lotIds through the disk passes to answer "was lot X parsed / emitted / skipped
// (why) / dropped at finalize" — the offline tools/_probe_missing_lots.py oracle made runtime-
// observable. A watched lot that produces NO [WATCH] line at all was never reached by any pass
// (parser scope miss); one that emits but is then ABSENT from the final buckets was dropped
// downstream. Edit this set to chase a new report. See [[disk-parser-coverage-gaps]].
const std::unordered_set<uint32_t> kWatchLots = {
    12070500, 12070320, 1038490030,                          // Ghostflame Torch / Burred Bolt (treasures)
    333062001, 333062011, 333062021, 333065001, 337000805,   // Larval Tears / boss armour (enemy base+k)
};
inline void watch_lot(uint32_t lot, const char *stage, const std::string &detail = "")
{
    if (!goblin::config::diagLootPos || !kWatchLots.count(lot)) return;
    spdlog::info("[WATCH] lot={} {}{}{}", lot, stage, detail.empty() ? "" : " ", detail);
}

// Built exactly once via std::call_once — by the setup_mod prebuild thread
// (prebuild_markers, kills the first-map-open hitch) OR lazily by the first
// markers()/census call, whichever runs first. call_once blocks concurrent
// callers until the build completes, so markers() always sees a full cache.
std::once_flag g_build_once;

// ── Position cell for the finalize geom dedup ─────────────────────────────────
// A quantized projected-position bucket: (group, 0.5-unit world X/Z). worldX/Z are
// produced by the SAME marker_world_pos projection for every source (push_marker),
// so a baked gather node and the disk collectible that re-emits it share a cell
// (the bake's 1e-3 position rounding is far below the 0.5 grid). Exact tuple key
// (not a pre-mixed hash) so distinct cells never collide into a false drop.
struct Cell
{
    int group, qx, qz;
    bool operator==(const Cell &o) const
    { return group == o.group && qx == o.qx && qz == o.qz; }
};
struct CellHash
{
    size_t operator()(const Cell &c) const
    {
        size_t h = 1469598103934665603ULL;
        for (int v : {c.group, c.qx, c.qz})
        { h ^= static_cast<uint32_t>(v); h *= 1099511628211ULL; }
        return h;
    }
};
inline Cell cell_of(const Marker &m)
{
    return Cell{m.group,
                static_cast<int>(std::lround(m.worldX * 2.0f)),
                static_cast<int>(std::lround(m.worldZ * 2.0f))};
}

// Identity key for piece dedup = tile + MSB part name. Pieces carry an exact, unique name per
// placement (AEG099_821_90NN), so matching a baked piece to its disk twin by (tile, name) is
// position-INDEPENDENT — it survives tiles where the bake stored an anomalous offset position
// (e.g. m11_10 Leyndell Ashen Capital: bake pos = raw MSB pos + (-2195,-352)), which a positional
// dedup can't match. See [[aeg-collectible-source]].
inline std::string piece_key(uint32_t area, uint32_t gx, uint32_t gz, const char *name)
{
    const uint32_t tile = (area << 24) | (gx << 16) | (gz << 8);
    return std::to_string(tile) + "|" + name;
}

// Synthetic row_id base for runtime (disk-pass) geom markers that need GEOF/WGM collected-
// graying (Rune/Ember Pieces). Above the 32-bit WorldMapPointParam/MASSEDIT id space so it
// never collides with a baked (dynamic) row_id; the same id is set on the Marker AND registered
// in goblin::collected so is_*_row_collected(row_id) resolves. See [[aeg-collectible-source]].
constexpr uint64_t kRuntimeGeomRowBase = 0x1'0000'0000ULL;

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

// Build ONE marker for category `c` from a WorldMapPointParam row `d` (baked MAP_ENTRY or live
// param) + its row_id + lot linkage, and push it to its bucket. Shared by the MAP_ENTRIES pass and
// the live-boss pass so both produce identical markers.
void push_marker(uint64_t row_id, const from::paramdef::WORLD_MAP_POINT_PARAM_ST &d, int c,
                 uint32_t lotId, uint8_t lotType,
                 Source source = Source::Baked, bool live_classified = false)
{
    namespace gen = goblin::generated;
    int ga;
    float wx, wz;
    goblin::marker_world_pos(d.areaNo, d.gridXNo, d.gridZNo, d.posX, d.posZ, ga, wx, wz,
                             /*conv_underground=*/true);
    int grp = goblin::marker_group_from(d.areaNo, ga);
    int pname = -1;
    int ckey = goblin::marker_cluster_key(d.areaNo, d.gridXNo, d.gridZNo, d.posX, d.posZ, &pname);
    int frag = goblin::marker_fragment_flag(d.areaNo, d.gridXNo, d.gridZNo, d.posX, d.posZ);
    // Lot-backed loot: resolve the LIVE pickup flag (baked textDisableFlagId1 is
    // stale under ERR/randomizer) so collected loot grays + the census decrements.
    int collected_flag = (int)goblin::resolve_loot_flag(lotId, lotType, d.textDisableFlagId1);
    goblin::diag_loot_flags(lotId, lotType, d.textDisableFlagId1, c, d.textId1);
    // Item IDENTITY (name/icon key) is read LIVE from ItemLotParam for lot-backed markers, so the
    // marker shows the item ERR actually placed (drift-proof); non-lot rows + any miss fall back to
    // the baked textId1. setup_messages preloads the FMG name for this SAME resolved key.
    const int32_t name_id = goblin::resolve_loot_item_textid(lotId, lotType, d.textId1);
    Marker m{wx, wz, grp, (int)d.areaNo, c, ckey, pname, name_id,
             category_color(c), category_icon_key(c), frag,
             row_id, (int)d.clearedEventFlagId, collected_flag};
    m.secondary_flag = secondary_story_flag(d.areaNo, d.gridXNo, d.gridZNo);
    m.hide_when_flag = hide_when_story_flag(d.areaNo, d.gridXNo, d.gridZNo);
    // Raw param coords for the engine's live projection (config live_projection).
    m.raw_area = d.areaNo; m.raw_gx = d.gridXNo; m.raw_gz = d.gridZNo;
    m.raw_px = d.posX; m.raw_pz = d.posZ;
    // Lot-backed loot (for anonymous_loot spoiler mode). Pieces/kindling are
    // geom/SFX-tracked, never a lot — exclude them (mirrors goblin_inject's is_lot).
    const auto cat = static_cast<gen::Category>(c);
    const bool piece = cat == gen::Category::ReforgedRunePieces ||
                       cat == gen::Category::ReforgedEmberPieces ||
                       cat == gen::Category::LootMaterialNodes;
    const bool kind = cat == gen::Category::WorldKindlingSpirits;
    m.lot_backed = !piece && !kind && lotId != 0 && lotType != 0;
    m.source = source;
    m.live_classified = live_classified;
    m.lotId = lotId;
    m.lotType = lotType;
    m.worldY = d.posY;  // DX item 7: block-local altitude for the above/below-player badge
    // Per-item icon: resolve THIS lot's real inventory iconId from the live ItemLotParam -> EquipParam,
    // so a loot marker draws its OWN item icon instead of the category representative. Mod-agnostic
    // (reads the active install's live params). resolve_loot_item_textid with baked_textid=0 returns the
    // offset-encoded item key (>=100M) on a live hit, 0 on a miss; item_real_icon_id maps that key to the
    // EquipParam iconId. Any miss leaves item_icon_id=0 so the renderer falls back to the category rep.
    if (m.lot_backed) {
        const int32_t key = goblin::resolve_loot_item_textid(lotId, lotType, 0);
        if (key >= 100000000) {
            const int icon = goblin::item_real_icon_id(key);
            if (icon > 0) m.item_icon_id = icon;
        }
        // Stacking: a lot can bundle several items (multiple slots, or qty>1 on slot 01) — the
        // marker still draws slot-01's icon/name but carries the total count for the "×N" badge.
        m.count = goblin::lot_item_count(lotId, lotType);
    }
    g_buckets[c].push_back(m);
}

// ── Mechanism C: shared ItemLotParam sequence-sibling expansion ───────────────────────────
// ER bundles a multi-item GUARANTEED reward as a CONTIGUOUS ItemLotParam run base, base+1, base+2, …
// but every disk SOURCE (MSB Treasure, NpcParam enemy drop, EMEVD award) carries only the chain BASE;
// the engine awards the whole contiguous run until a param gap. This helper places each NOTABLE
// sibling base+1..base+kMaxSib at the base's world position `at`, classified live. It is the SINGLE
// source of the sibling rule — previously copy-pasted into the treasure + emevd passes, and ABSENT
// from the enemy pass, which silently dropped boss-bundle items (e.g. the m12 Larval Tears that sit
// at base+1 of a placed enemy's itemLotId_enemy; tools/_probe_missing_lots.py).
//
// Per requested table (`SibTable.tbl`: 1=ItemLotParam_map, 2=_enemy) a chain ENDS at the first param
// gap or the first sub in that table's `stopSet` (the next bundle's base). A sub already owned by
// another pass (`skipA`/`skipB`) or already emitted this build (`sib_seen`) is skipped. Rune/Ember
// Pieces are suppressed (shown via the Reforged GEOM category at their real scattered spots). Counts
// land in `out`; `lot_tile`(+`tile_pack`) and `per_cat` are optional (treasure tile map / verbose).
struct SibCounts { int emitted = 0, runeember = 0, unclassified = 0; };
struct SibTable { uint8_t tbl; const std::unordered_set<uint32_t> *stopSet; };

void emit_lot_siblings(uint32_t baseLot,
                       const from::paramdef::WORLD_MAP_POINT_PARAM_ST &at,
                       std::initializer_list<SibTable> tables,
                       const std::unordered_set<uint32_t> *skipA,
                       const std::unordered_set<uint32_t> *skipB,
                       std::unordered_set<uint32_t> &covered,
                       std::unordered_set<uint32_t> &sib_seen,
                       SibCounts &out,
                       std::unordered_map<uint32_t, uint32_t> *lot_tile = nullptr,
                       uint32_t tile_pack = 0,
                       std::unordered_map<int, int> *per_cat = nullptr)
{
    constexpr uint32_t kMaxSib = 50;
    for (const SibTable &st : tables)
    {
        for (uint32_t off = 1; off <= kMaxSib; ++off)
        {
            uint32_t sub = baseLot + off;
            if (st.stopSet && st.stopSet->count(sub)) break;      // next bundle's base → chain ends
            uint32_t sflag = 0;
            int32_t skey = 0;
            if (!goblin::lot_row_in_table(sub, st.tbl, &sflag, &skey)) break;  // param gap → chain ends
            if (sflag == 0 || skey == 0) continue;                // exists but repeatable/empty
            if ((skipA && skipA->count(sub)) || (skipB && skipB->count(sub))) continue;  // owned elsewhere
            if (!sib_seen.insert(sub).second) continue;           // already emitted as a sibling
            if (skey >= 500000000 && skey < 600000000)
            {
                int32_t gid = skey - 500000000;
                if (gid == 800010 || gid == 850010) { ++out.runeember; watch_lot(sub, "skip-sibling", "rune/ember"); continue; }
            }
            int scat = goblin::item_marker_category(skey);
            bool slc = false;
            if (scat < 0) { scat = goblin::classify_item_live(skey); slc = (scat >= 0); }
            if (scat < 0 || scat >= NUM_CAT) { ++out.unclassified; watch_lot(sub, "skip-sibling", "unclassified"); continue; }
            from::paramdef::WORLD_MAP_POINT_PARAM_ST sd = at;
            push_marker(/*row_id=*/sub, sd, scat, sub, /*lotType=*/st.tbl, Source::DiskMSB, slc);
            covered.insert(sub);
            if (lot_tile) lot_tile->emplace(sub, tile_pack);
            ++out.emitted;
            if (per_cat) ++(*per_cat)[scat];
            watch_lot(sub, "emit-sibling", "tbl=" + std::to_string(st.tbl) + " cat=" + std::to_string(scat));
        }
    }
}

// Build the WorldBosses bucket LIVE from WorldMapPointParam field-boss rows (textId2==5100),
// not from the bake. The live row is the authoritative source (correct position +
// clearedEventFlagId + textId1 name + iconId); reading it kills per-ERR-version drift and the
// boss_list.json matching anomalies (see windows_enemy_boss_runtime_pos_re_findings.md). All
// OTHER categories stay baked (NPCs are NpcParam-synthetic, loot is MSB/item-lot — no live WMP
// source). NOTE: live rows are still in the dungeon-internal frame (area 10/31/…) like the bake,
// so this does NOT change the dungeon-boss projection drift — same marker_world_pos path.
void build_live_bosses()
{
    namespace gen = goblin::generated;
    const int c = static_cast<int>(gen::Category::WorldBosses);
    int n = 0;
    try
    {
        for (auto [rowId, row] :
             from::params::get_param<from::paramdef::WORLD_MAP_POINT_PARAM_ST>(L"WorldMapPointParam"))
        {
            if (row.textId2 != 5100) continue;          // field-boss marker rows only
            push_marker(rowId, row, c, /*lotId=*/0u, /*lotType=*/0u, Source::Live);
            ++n;
            // Suppress the game's NATIVE boss icon: clear dispMask on the live row so the engine
            // skips drawing it, leaving the overlay as the sole boss source (no double icon). Safe
            // because push_marker already snapshotted pos/name/icon and it IGNORES dispMask, so our
            // marker is unaffected. `row` is a reference into live param memory (params.hpp).
            if (goblin::config::suppressNativeBosses)
            {
                row.dispMask00 = false;
                row.dispMask01 = false;
                row.dispMask02 = 0;
            }
        }
    }
    catch (...)
    {
        spdlog::warn("[BOSSLIVE] WorldMapPointParam not readable — boss markers absent this build");
        return;
    }
    spdlog::info("[BOSSLIVE] built {} boss markers from live WorldMapPointParam (textId2==5100)", n);
}

// Build the loot markers from the ACTIVE mod's REAL disk MSBs (config
// loot_from_disk_msb). Each POSITIONED Treasure → one marker via the SAME
// push_marker path as the bake, so projection/identity/flags are identical: the
// tile filename gives area/grid, Part+0x20 gives the block-local pos, and the
// live ItemLotParam (joined by lotId, lotType 1) gives the item identity +
// category + pickup flag. A lotId may map to MULTIPLE parts → each is emitted.
// Records every lotId it placed in `covered` so build_buckets DROPS the baked
// rows the disk now owns (lotId-coverage replace). Logs [LOOTDISK].
// Packed tile key (area<<16 | gx<<8 | gz) for the diagnostic log below.
static uint32_t pack_tile(uint8_t a, uint8_t gx, uint8_t gz)
{
    return ((uint32_t)a << 16) | ((uint32_t)gx << 8) | gz;
}

static void build_disk_loot_markers(const std::vector<DiskTreasure> &treasures,
                                     std::unordered_set<uint32_t> &covered,
                                     std::unordered_map<uint32_t, uint32_t> &lot_tile)
{
    GOBLIN_BENCH("build.disk_loot");
    int emitted = 0, unclassified = 0;
    // All Treasure base lots (the lotId each MSB Treasure carries @td+0x10), built up front so
    // the sibling walk below can stop a chain at the next treasure base regardless of emit order.
    std::unordered_set<uint32_t> bases;
    bases.reserve(treasures.size());
    for (const DiskTreasure &t : treasures) bases.insert(t.lotId);
    for (const DiskTreasure &t : treasures)
    {
        from::paramdef::WORLD_MAP_POINT_PARAM_ST d{};
        d.areaNo = t.area;
        d.gridXNo = t.gx;
        d.gridZNo = t.gz;
        d.posX = t.posX;
        d.posZ = t.posZ;
        d.posY = t.posY;  // DX item 7: altitude badge
        // Identity/category from the LIVE lot (lotType 1 = ItemLotParam_map). textId1
        // stays -1 and the flags 0, so push_marker resolves the item + pickup flag
        // live (the lot-backed path) — exactly like a baked lot-backed row.
        int32_t key = goblin::resolve_loot_item_textid(t.lotId, 1, -1);
        // Region Map fragments are owned by the dedicated build_disk_maps_markers pass (routed to
        // WorldMaps). Skip them here so the same pickup isn't ALSO emitted into a generic Loot bucket
        // — without this the live fallback (no map case) filed every disk-placed map under Crafting
        // Materials, drawing each map twice (surfaced by docs/item_classification.md). ER's own
        // sortGroupId 190/191 via goods_is_map; map goods carry the +500M goods encoding.
        if (key >= 500000000 && goblin::goods_is_map(key - 500000000))
        { watch_lot(t.lotId, "skip-treasure", "map-routed"); continue; }
        int c = goblin::item_marker_category(key);
        bool lc = false;
        if (c < 0) { c = goblin::classify_item_live(key); lc = (c >= 0); }  // live fallback (any mod / unbaked item)
        if (c < 0 || c >= NUM_CAT)
        {
            ++unclassified;  // item type genuinely unknown → no bucket
            watch_lot(t.lotId, "skip-treasure", "unclassified");
            continue;
        }
        push_marker(/*row_id=*/t.lotId, d, c, t.lotId, /*lotType=*/1, Source::DiskMSB, lc);
        covered.insert(t.lotId);
        lot_tile.emplace(t.lotId, pack_tile(t.area, t.gx, t.gz)); // first tile per lot
        ++emitted;
        watch_lot(t.lotId, "emit-treasure", "cat=" + std::to_string(c));
    }

    // ── Sequence-sibling expansion (multi-lot treasures) — shared emit_lot_siblings ──
    // A physical MSB Treasure carries ONE itemLotId@td+0x10 (the chain BASE), but ER bundles a
    // multi-item reward as a CONTIGUOUS ItemLotParam_map sequence base, base+1, base+2, … (e.g. a
    // corpse grants a full armour set = 4 rows). The MSB parser reads only the base, so the bake's
    // expanded sibling rows are exactly the DEBAKE-GAP (~310/328 measured: Armour sets dominate).
    // Table 1 (_map) only; a chain stops at the next treasure base (`bases`).
    SibCounts sib;
    std::unordered_set<uint32_t> sib_seen;
    for (const DiskTreasure &t : treasures)
    {
        from::paramdef::WORLD_MAP_POINT_PARAM_ST at{};
        at.areaNo = t.area;
        at.gridXNo = t.gx;
        at.gridZNo = t.gz;
        at.posX = t.posX;
        at.posZ = t.posZ;
        emit_lot_siblings(t.lotId, at, {{1, &bases}}, /*skipA=*/nullptr, /*skipB=*/nullptr,
                          covered, sib_seen, sib, &lot_tile, pack_tile(t.area, t.gx, t.gz));
    }
    spdlog::info("[LOOTDISK] emitted {} disk loot markers + {} sequence-siblings (multi-lot "
                 "treasures), {} lots covered, {} base-unclassified ({} sibling rune/ember-skipped, "
                 "{} sibling-unclassified)",
                 emitted, sib.emitted, (int)covered.size(), unclassified, sib.runeember,
                 sib.unclassified);
    g_skip.unclassified += unclassified + sib.unclassified;
    g_skip.by_design += sib.runeember;
}

// AEG collectible markers (config loot_collectibles): each placed AEG Asset whose
// AssetEnvironmentGeometryParam[aegRow].pickUpItemLotParamId is a real lot becomes a
// marker. Identity/category come LIVE from that lot (resolve_loot_item_textid), exactly
// like the disk-loot path — no bake, no manual model→item table, works on any mod.
// EMIT PER PLACEMENT: collectibles share one lot across many world nodes (e.g. every
// Gaseous Stone = lot 998500), each a distinct pickup at its own position — so we do
// NOT dedup by lot (that would collapse 505 nodes to 1). We only skip placements whose
// lot is a real MSB Treasure (the ground-item pickup assets the treasure path already
// places). Each emitted lot is added to `covered` so the baked-row replace drops any
// baked duplicate (a lotId there covers all its markers).
static void build_disk_collectible_markers(const std::vector<DiskCollectible> &collectibles,
                                           const std::unordered_set<uint32_t> &treasure_lots,
                                           std::unordered_set<uint32_t> &covered,
                                           std::unordered_set<Cell, CellHash> &out_cells,
                                           std::unordered_set<std::string> &out_piece_keys,
                                           std::unordered_set<std::string> &out_gather_keys)
{
    GOBLIN_BENCH("build.disk_collectibles");
    int emitted = 0, no_lot = 0, unclassified = 0, dup = 0, piece_emitted = 0, clutter_skip = 0;
    uint64_t next_rt = 0;  // running index for synthetic runtime geom row_ids (pieces)
    std::vector<goblin::collected::RuntimeEntry> rt_entries;  // pieces to register for graying
    const bool verbose = goblin::config::diagLootPos;
    std::unordered_map<int, int> per_cat;  // category → emitted count (diag)
    // [DEBAKE-RECOVER] sizing (verbose only): how much of the non-_8xx "clutter" actually carries
    // recoverable loot. The 328 DEBAKE-GAP = corpse/body pickups (AEG099_630/090, …) the _8xx filter
    // drops along with the pot/jar clutter. Measure pickups-with-a-lot, of those how many are FLAGGED
    // (getItemFlagId!=0 → one-time loot spot, not respawning clutter) or equipment, to gauge the
    // re-flood before widening the filter. Per-placement (≈ markers added if recovered).
    int rec_pickup = 0, rec_flagged = 0, rec_equip = 0, rec_dup = 0;
    std::unordered_set<uint32_t> rec_flag_lots;
    for (const DiskCollectible &c : collectibles)
    {
        // Scope to GATHER NODES via the LIVE native signal (isEnableRepick) — replaces the old
        // "_8xx model range" heuristic. A gather node respawns + hides until then (the bake's
        // exact filter); one-shot breakable clutter (pots/jars/corpses, AEG099_68x/72x/73x,
        // AEG463_65x) is isEnableRepick=false → skipped. This emits EVERY gather node, including
        // the _6xx/_7xx/_9xx ones the bake's "Material Nodes" category covered (no model table).
        if (!goblin::aeg_is_gather(c.aegRow))
        {
            ++clutter_skip;
            if (verbose)  // size the recoverable corpse-loot inside the excluded clutter
            {
                uint32_t clot = goblin::aeg_pickup_lot(c.aegRow);
                if (clot != 0)
                {
                    ++rec_pickup;
                    if (treasure_lots.count(clot)) ++rec_dup;  // already placed by the treasure pass
                    if (goblin::resolve_loot_flag(clot, 1, 0) != 0)
                    { ++rec_flagged; rec_flag_lots.insert(clot); }   // one-time loot spot
                    int32_t ckey = goblin::resolve_loot_item_textid(clot, 1, -1);
                    int ccat = goblin::item_marker_category(ckey);
                    if (ccat < 0) ccat = goblin::classify_item_live(ckey);
                    // Equip* = the first 5 Category enumerators (Armaments..Talismans).
                    if (ccat >= 0 && ccat <= (int)goblin::generated::Category::EquipTalismans)
                        ++rec_equip;
                }
            }
            continue;
        }
        // Rune (AEG099_821) / Ember (AEG099_822) Pieces — the REAL placed objects. Their
        // pickUpItemLotParamId points only at the shared Runic/Ember TRACE counter (a "shadow reward"
        // tally you gain on pickup, NOT the object's identity), so resolving the lot mislabels them
        // "Runic Trace". The native identity is the object itself (ActionButtonText "Collect rune/
        // ember piece"); place them from disk under the Reforged categories using the same goods-name
        // encoding the bake used (800010/850010 → "Rune/Ember Piece" + star icon). They carry NO event
        // flag — collection is geom-state — so register the placement with goblin::collected for GEOF/
        // WGM graying, like the baked pieces. The finalize geom-dedup drops the baked twin.
        //
        // Match the FULL aegRow (99821/99822), NOT sub==821/822: sub = aegRow%1000 also matches OTHER
        // AEG groups' _821/_822 (AEG023_822, AEG230_821, …) which are unrelated DLC assets — emitting
        // those mislabeled 461 Rune + 131 Ember phantom pieces (e.g. the "Éclat calciné" clusters).
        if (c.aegRow == 99821 || c.aegRow == 99822)
        {
            const bool rune = (c.aegRow == 99821);
            const int pcat = static_cast<int>(rune ? goblin::generated::Category::ReforgedRunePieces
                                                   : goblin::generated::Category::ReforgedEmberPieces);
            from::paramdef::WORLD_MAP_POINT_PARAM_ST d{};
            d.areaNo = c.area;
            d.gridXNo = c.gx;
            d.gridZNo = c.gz;
            d.posX = c.posX;
            d.posZ = c.posZ;
            d.posY = c.posY;  // DX item 7: altitude (block-local Y) for the above/below-player badge
            d.textId1 = (rune ? 800010 : 850010) + 500000000;  // GoodsName "Rune/Ember Piece"
            const uint64_t rid = kRuntimeGeomRowBase + (next_rt++);
            push_marker(/*row_id=*/rid, d, pcat, /*lotId=*/0u, /*lotType=*/0u, Source::DiskMSB);
            // (pieces dedup by IDENTITY via out_piece_keys, not by position — so they are NOT added
            // to out_cells; that keeps the positional material-node dedup from wrongly dropping a
            // baked boss-flag piece that merely sits near a world piece.)
            // geom_slot = MSB InstanceID suffix - 9000 (e.g. "AEG099_821_9003" → 3); -1 if absent.
            int slot = -1;
            const size_t us = c.name.rfind('_');
            if (us != std::string::npos && us + 1 < c.name.size())
            {
                const int suf = std::atoi(c.name.c_str() + us + 1);
                if (suf >= 9000)
                    slot = suf - 9000;
            }
            rt_entries.push_back(goblin::collected::RuntimeEntry{
                rid, c.area, c.gx, c.gz, slot, c.posX, c.posY, c.posZ, c.name, c.modelName});
            out_piece_keys.insert(piece_key(c.area, c.gx, c.gz, c.name.c_str()));
            ++piece_emitted;
            continue;
        }
        uint32_t lot = goblin::aeg_pickup_lot(c.aegRow);  // live param chain (0 = not a pickup)
        if (lot == 0) { ++no_lot; continue; }
        if (treasure_lots.count(lot)) { ++dup; continue; }  // a Treasure ground-item, already placed
        int32_t key = goblin::resolve_loot_item_textid(lot, 1, -1);
        int cat = goblin::item_marker_category(key);
        bool lc = false;
        if (cat < 0) { cat = goblin::classify_item_live(key); lc = (cat >= 0); }  // live fallback (any mod / unbaked item)
        if (cat < 0 || cat >= NUM_CAT) { ++unclassified; continue; }
        from::paramdef::WORLD_MAP_POINT_PARAM_ST d{};
        d.areaNo = c.area;
        d.gridXNo = c.gx;
        d.gridZNo = c.gz;
        d.posX = c.posX;
        d.posZ = c.posZ;
        d.posY = c.posY;  // DX item 7: altitude (block-local Y) for the above/below-player badge
        // Gather nodes are COLLECTED GEOM (GEOF/WGM-tracked, like the Rune/Ember pieces) — NOT
        // event-flag loot. So the marker's graying key must be a synthetic geom row_id REGISTERED with
        // goblin::collected (keyed by tile + part-name + slot), not the lot — `row_id=lot` was never in
        // the tracking maps, so a disk gather marker never grayed. Emit with the synthetic rid + register
        // it below (the lot stays the lotId arg for identity/census). Required for Phase 2 (the
        // MAP_ENTRIES seed that used to track these in goblin_collected::initialize goes away with the bake).
        const uint64_t rid = kRuntimeGeomRowBase + (next_rt++);
        push_marker(/*row_id=*/rid, d, cat, lot, /*lotType=*/1, Source::DiskMSB, lc);  // one per placement
        // Register for GEOF/WGM collected-graying — only when the MSB part name is known (the WGM
        // alive-match keys on it; a nameless placement can't be tracked, its synthetic rid never grays).
        // geom_slot = MSB InstanceID suffix - 9000 ("AEG099_840_9002" → 2), same parse as the pieces.
        if (!c.name.empty())
        {
            int gslot = -1;
            const size_t us = c.name.rfind('_');
            if (us != std::string::npos && us + 1 < c.name.size())
            {
                const int suf = std::atoi(c.name.c_str() + us + 1);
                if (suf >= 9000) gslot = suf - 9000;
            }
            rt_entries.push_back(goblin::collected::RuntimeEntry{
                rid, c.area, c.gx, c.gz, gslot, c.posX, c.posY, c.posZ, c.name, c.modelName});
        }
        // Record this gather-asset's projected cell AND its IDENTITY (tile + MSB part name)
        // so the finalize dedup can drop the baked Material Node twin. Positional cell catches
        // most; IDENTITY catches the ones whose baked position is offset by >0.5u from the live
        // MSB pos (the same near-miss the Rune/Ember pieces hit → the 11 baked-only survivors).
        // Both keyed to the COLLECTIBLE pass only, so an unrelated coincidence never evicts a node.
        out_cells.insert(cell_of(g_buckets[cat].back()));
        if (!c.name.empty())
            out_gather_keys.insert(piece_key(c.area, c.gx, c.gz, c.name.c_str()));
        covered.insert(lot);  // for the baked-row replace (de-dup vs the bake), NOT a skip key
        ++emitted;
        if (verbose) ++per_cat[cat];
    }
    // Register the Rune/Ember Piece AND gather-node placements for GEOF/WGM collected-graying (drained
    // into the tracking maps on the refresh thread — see goblin_collected). One-time per disk build.
    // After Phase 2 (bake deleted) this is the SOLE source of the geom tracking — the MAP_ENTRIES seed
    // in goblin_collected::initialize is gone, so every geom-grayed marker must register here.
    goblin::collected::register_runtime_entries(std::move(rt_entries));

    spdlog::info("[LOOTDISK] collectibles: {} markers emitted ({} assets total, {} non-ERR clutter "
                 "(pots/jars), {} not-a-pickup, {} treasure-dup, {} unclassified, {} rune/ember pieces)",
                 emitted, (int)collectibles.size(), clutter_skip, no_lot, dup, unclassified, piece_emitted);
    g_skip.by_design += clutter_skip;  // non-ERR pot/jar clutter, anti-flood
    g_skip.no_anchor += no_lot;
    g_skip.dedup += dup;
    g_skip.unclassified += unclassified;
    if (verbose)
    {
        for (auto &[cat, n] : per_cat)
            spdlog::info("[LOOTDISK]   collectible category index {} = {} markers "
                         "(19=CraftingMaterials,24=SmithingStones,18=Consumables)", cat, n);
        spdlog::info("[DEBAKE-RECOVER] excluded non-_8xx clutter: {} total; {} are pickups w/ a lot; "
                     "of those {} FLAGGED one-time ({} distinct lots), {} equipment, {} already "
                     "treasure-placed. Widening to flagged would ADD ~{} markers (equip-only ~{}).",
                     clutter_skip, rec_pickup, rec_flagged, (int)rec_flag_lots.size(), rec_equip,
                     rec_dup, rec_flagged - rec_dup, rec_equip);
    }
}

// Enemy-drop markers (config loot_enemy_drops): each placed Enemy part whose
// NPCParamID resolves a NpcParam item lot becomes a marker. Identity/category come
// LIVE from that lot (resolve_loot_item_textid), exactly like the treasure/collectible
// paths — no bake. The position is the enemy placement (same world transform). The
// placed lot is added to `covered` so the baked LootSource::Enemy row it covers is
// dropped below (an enemy provenance guard).
//
// NOTABILITY FILTER (the key to not flooding): a raw "every enemy with a drop lot"
// emit = the 25608-placement universe (mostly Sliver of Meat / Thin Beast Bones
// respawning clutter). The game's own one-time-loot signal cleanly separates the
// notable drops: a lot with a persistent **getItemFlagId** (the game remembers you
// took it — `resolve_loot_flag` already returns 0 for respawning/repeatable loot
// like golden runes / gloveworts whose flag is in the temp range). We emit ONE
// marker per such lot (dedup by lot: a notable item shared across a few placements
// is one obtainable pickup). Datamined offline (tools/datamine_enemy_notability.py +
// sim_enemy_pass.py): every one of the 119 curated baked enemy lots carries a
// persistent flag; this filter yields ~124 notable lots = the baked notable drops
// PLUS ~43 the bake's manual LOOT_CATEGORIES curation missed (Blaidd's/Ronin's
// armour sets, Hoslow's Petal Whip, Patches'/Miriel's Bell Bearing, ...). A clean
// LIVE filter, no porting of the pipeline's ~60 category rules; better coverage than
// the bake. NOTE on the lot-exact provenance guard below: a few baked enemy rows use
// a sibling ItemLotParam row (npcParam → base lot, bake → the item row), so they are
// NOT lot-matched and stay baked — preserving the 3 DLC one-time drops whose flag is
// in a high range resolve_loot_flag treats as repeatable (Blessed Bone Shard, Iris of
// Occultation); the measured duplicate overlap is only ~3. See
// docs/re/windows_enemy_loot_nobake_analysis.md.
static void build_disk_enemy_markers(const std::vector<DiskEnemy> &enemies,
                                     const std::unordered_set<uint32_t> &treasure_lots,
                                     std::unordered_set<uint32_t> &covered,
                                     std::unordered_set<uint32_t> *parsed_enemy_lots = nullptr)
{
    GOBLIN_BENCH("build.disk_enemies");
    int emitted = 0, no_lot = 0, unclassified = 0, dup = 0, respawn = 0, lot_dup = 0;
    const bool verbose = goblin::config::diagLootPos;
    std::unordered_map<int, int> per_cat;  // category → emitted count (diag)
    std::unordered_set<uint32_t> seen;      // dedup by lot (one marker per notable lot)
    for (const DiskEnemy &en : enemies)
    {
        // [ENEMY-MARKERS] de-bake triage: record EVERY parsed enemy's raw itemLotId_enemy (0x30),
        // BEFORE the map-preference pick / filters, so the uncovered-baked-Enemy diag can tell a
        // parsed-but-uncovered lot (map-preferred or filtered → recoverable) from a never-parsed
        // one (MSB scope miss). Diag-only (set passed only when lootFromDiskMsb).
        if (parsed_enemy_lots)
            if (int32_t el = goblin::npc_item_lot_enemy(en.npcParamId); el > 0)
                parsed_enemy_lots->insert((uint32_t)el);
        uint8_t lt = 0;
        uint32_t lot = goblin::npc_loot_lot(en.npcParamId, &lt);  // live NpcParam chain
        if (lot == 0) { ++no_lot; continue; }
        if (treasure_lots.count(lot)) { ++dup; watch_lot(lot, "skip-enemy", "treasure-dup"); continue; }  // already a Treasure ground item
        if (goblin::resolve_loot_flag(lot, lt, 0) == 0) { ++respawn; watch_lot(lot, "skip-enemy", "no-one-time-flag"); continue; }  // not a one-time drop
        if (!seen.insert(lot).second) { ++lot_dup; continue; }  // a notable lot already placed
        int32_t key = goblin::resolve_loot_item_textid(lot, lt, -1);
        int cat = goblin::item_marker_category(key);
        bool lc = false;
        if (cat < 0) { cat = goblin::classify_item_live(key); lc = (cat >= 0); }  // live fallback (any mod / unbaked)
        if (cat < 0 || cat >= NUM_CAT) { ++unclassified; watch_lot(lot, "skip-enemy", "unclassified"); continue; }
        from::paramdef::WORLD_MAP_POINT_PARAM_ST d{};
        d.areaNo = en.area;
        d.gridXNo = en.gx;
        d.gridZNo = en.gz;
        d.posX = en.posX;
        d.posZ = en.posZ;
        d.posY = en.posY;  // DX item 7: altitude badge
        push_marker(/*row_id=*/lot, d, cat, lot, /*lotType=*/lt, Source::DiskMSB, lc);
        covered.insert(lot);  // drops the matching baked LootSource::Enemy row
        ++emitted;
        watch_lot(lot, "emit-enemy", "cat=" + std::to_string(cat));
        if (verbose) ++per_cat[cat];
    }

    // ── Sequence-sibling expansion (multi-item enemy/boss bundles) — shared emit_lot_siblings ──
    // ER awards a boss/enemy a CONTIGUOUS ItemLotParam run base, base+1, … but NpcParam carries only
    // the base. The loop above placed (at most) the base; this walks base+1.. so the bundled drops
    // show too — the m12 Larval Tears + boss armour sit at base+1/+5 of a placed enemy's lot and were
    // silently dropped before this pass existed (tools/_probe_missing_lots.py: 333062001 = 333062000+1,
    // NpcParam 33300662 on c3330_9006; 337000805 = 337000800+5, NpcParam 33700865 on c3370_9102).
    // Runs INDEPENDENT of the base's notability filters (a respawning base whose +1 is a one-time tear
    // still yields the tear). Walks the base's OWN table (lt); a chain stops at the next enemy base
    // (enemy_bases); a sub that's already a Treasure ground item is skipped. Dedup by base + by sub.
    std::unordered_set<uint32_t> enemy_bases;
    for (const DiskEnemy &en : enemies)
    {
        uint8_t blt = 0;
        if (uint32_t l = goblin::npc_loot_lot(en.npcParamId, &blt)) enemy_bases.insert(l);
    }
    SibCounts sib;
    std::unordered_set<uint32_t> sib_seen, walked_bases;
    for (const DiskEnemy &en : enemies)
    {
        uint8_t lt = 0;
        uint32_t lot = goblin::npc_loot_lot(en.npcParamId, &lt);
        if (lot == 0 || !walked_bases.insert(lot).second) continue;  // no lot / base already walked
        from::paramdef::WORLD_MAP_POINT_PARAM_ST at{};
        at.areaNo = en.area;
        at.gridXNo = en.gx;
        at.gridZNo = en.gz;
        at.posX = en.posX;
        at.posZ = en.posZ;
        emit_lot_siblings(lot, at, {{lt, &enemy_bases}}, /*skipA=*/&treasure_lots, /*skipB=*/nullptr,
                          covered, sib_seen, sib, /*lot_tile=*/nullptr, /*tile_pack=*/0,
                          verbose ? &per_cat : nullptr);
    }
    spdlog::info("[LOOTDISK] enemy drops: {} notable markers emitted + {} sequence-siblings ({} "
                 "placements total; filtered: {} npc-has-no-lot, {} treasure-dup, {} no-one-time-flag, "
                 "{} same-lot-dedup, {} unclassified; siblings: {} rune/ember-skip, {} unclassified)",
                 emitted, sib.emitted, (int)enemies.size(), no_lot, dup, respawn, lot_dup, unclassified,
                 sib.runeember, sib.unclassified);
    g_skip.no_anchor += no_lot;
    g_skip.dedup += dup + lot_dup;
    g_skip.by_design += respawn + sib.runeember;  // respawnable base + GEOM-tracked rune/ember siblings
    g_skip.unclassified += unclassified + sib.unclassified;
    if (verbose)
        for (auto &[cat, n] : per_cat)
            spdlog::info("[LOOTDISK]   enemy-drop category index {} = {} markers", cat, n);
}

// EMEVD-scripted award markers (config loot_emevd_drops). load_emevd_awards returns two
// kinds, distinguished by DiskEmevd::lotType: (A) direct template awards (lotType 1,
// ItemLotParam_map — the entity is carried in the event init) and (B) event-1200 boss
// drops (lotType 2, ItemLotParam_enemy — the entity is the boss the setter event
// references). Either way we join the entityId to its MSB Enemy part for the world
// position and resolve the lot LIVE (with the lotType hint, falling back to the other table).
// This places scripted drops the MSB Treasure/Enemy passes structurally CAN'T see —
// field/dungeon boss rewards, scarabs, painting pickups, NPC quest/invasion rewards,
// great runes, larval tears (the 13 templates in tools/extract_all_items.py:646). Each
// placed lot is added to `covered` so the baked LootSource::Emevd row it reproduces is
// dropped below. Dedup by (entity, lot) — the same award can appear across event files,
// and a lot shared by several scripted entities is one obtainable pickup PER entity (its
// own position). See docs/re/windows_enemy_loot_nobake_analysis.md §5b.
static void build_disk_emevd_markers(const std::vector<DiskEmevd> &awards,
                                     const std::vector<DiskEnemy> &enemies,
                                     const std::vector<DiskCollectible> &assets,
                                     const std::unordered_set<uint32_t> &treasure_lots,
                                     std::unordered_set<uint32_t> &covered,
                                     std::unordered_set<std::string> &out_piece_keys,
                                     std::unordered_set<Cell, CellHash> *maps_cells = nullptr)
{
    GOBLIN_BENCH("build.disk_emevd");
    // Enemy-only position map (perTile + ev1200 awards stay strictly entity→ENEMY — an asset-join
    // there re-introduces the ~395 asset-entity-chest over-match, see [[nobake-coverage-scoreboard]]).
    // First occurrence wins, matching the offline join (extract_all_items.py: `eid not in pos`).
    std::unordered_map<uint32_t, const DiskEnemy *> ent_enemy;
    ent_enemy.reserve(enemies.size());
    for (const DiskEnemy &en : enemies)
        if (en.entityId != 0) ent_enemy.emplace(en.entityId, &en);
    // Enemy+asset map for direct template + boss awards (allowAsset). An emevd reward can be anchored
    // to an MSB Asset, not an Enemy — Blaidd's spirit ash on AEG099_090, the scarab-cluster runes on
    // an AEG ground object — which the enemy-only join structurally can't see. Each asset is adapted to
    // a DiskEnemy (npcParamId 0) so the emit path is uniform; asset_enemies backs the ent_any pointers
    // so it must outlive the map. Enemies are added first → an id that is both keeps its enemy pos.
    std::vector<DiskEnemy> asset_enemies;
    asset_enemies.reserve(assets.size());
    for (const DiskCollectible &as : assets)
    {
        if (as.entityId == 0) continue;
        DiskEnemy e;
        e.entityId = as.entityId;
        e.area = as.area; e.gx = as.gx; e.gz = as.gz;
        e.posX = as.posX; e.posZ = as.posZ;
        e.name = as.name;
        asset_enemies.push_back(std::move(e));
    }
    std::unordered_map<uint32_t, const DiskEnemy *> ent_any = ent_enemy;  // copy enemy entries
    for (const DiskEnemy &e : asset_enemies)
        ent_any.emplace(e.entityId, &e);  // emplace → enemy already present wins
    int dbg_asset = 0, dbg_loose = 0, boss_base = 0;  // recovery diag
    // Resolve an award's world anchor: documented entity (in its allowed map), else — for allowAsset
    // (direct/boss) awards only — the nearest loose-anchor window that resolves (the documented entity
    // is a non-positionable logic id; the real anchor sits at another arg — Taylew/Viridian).
    auto resolve_pos = [&](const DiskEmevd &a) -> const DiskEnemy * {
        const auto &m = a.allowAsset ? ent_any : ent_enemy;  // perTile/ev1200 stay enemy-only
        auto it = m.find(a.entityId);
        if (it != m.end())
        {
            if (a.allowAsset && !ent_enemy.count(a.entityId)) ++dbg_asset;  // resolved as an ASSET
            return it->second;
        }
        // Loose-anchor fallback (direct-template + the perTile lot@idx(n-1) variant): the nearest
        // window that resolves IN THE AWARD'S ALLOWED MAP — enemy+asset for direct, enemy-only for
        // perTile (so the asset-chest over-match stays closed).
        for (uint32_t anc : a.anchors)
        {
            auto ai = m.find(anc);
            if (ai != m.end()) { ++dbg_loose; return ai->second; }
        }
        return nullptr;
    };

    int emitted = 0, no_entity = 0, dup = 0, treasure_dup = 0, unclassified = 0;
    int emit_direct = 0, emit_ev1200 = 0;  // by mechanism (lotType 1 = direct, 2 = event-1200)
    int boss_piece_emitted = 0, boss_piece_no_piece = 0;  // boss-reward Rune/Ember Piece recovery
    // Mechanism C (sequence-sibling) diag.
    SibCounts sib;
    const bool verbose = goblin::config::diagLootPos;
    std::unordered_map<int, int> per_cat;  // category → emitted count (diag)
    std::unordered_set<uint64_t> seen;      // dedup by (entity<<32 | lot)
    std::unordered_set<uint32_t> sib_seen;  // sub-lots already emitted (mechanism C dedup)
    std::unordered_set<uint32_t> boss_piece_seen;  // boss-reward Rune/Ember Piece subs (own dedup,
                                                   // NOT shared with sib_seen which the suppressed
                                                   // mechanism-C rune/ember subs pollute)
    // Every award lot is a "base" — a sibling that equals another base is emitted as that
    // base (its own entity/pos), so don't double it in the sibling walk.
    std::unordered_set<uint32_t> base_lots;
    base_lots.reserve(awards.size());
    for (const DiskEmevd &a : awards) base_lots.insert(a.lotId);
    for (const DiskEmevd &a : awards)
    {
        // Resolve the world anchor ONCE (asset-aware for direct/boss awards + loose-anchor fallback).
        // Shared by the boss-piece walk and the normal emit so it's computed/counted a single time.
        const DiskEnemy *pos = resolve_pos(a);
        // Rune/Ember Piece boss-drop recovery — applies to BOTH flag-keyed boss-reward templates
        // (a.bossReward = 90005860/61/80, defeatFlag==EntityID) AND event-1200 awards (lotType 2,
        // small defeatFlag joined to the boss via its setter). Both carry the piece as a base+k
        // ItemLotParam_map chain entry (the bind lot is a BASE; the piece is base+1/+2). Walk the
        // chain for the piece goods (800010 Rune / 850010 Ember) and emit it under the Reforged
        // category at the boss position (gray via the piece's own getItemFlagId), registering the boss
        // part-name identity so the baked c-model twin is dropped by the existing Reforged dedup. A
        // non-piece event-1200 award (e.g. Lhutel) finds no piece in the chain → falls through to the
        // normal award handling below; a bossReward award that finds none is dropped.
        if (a.bossReward || a.lotType == 2)
        {
            bool placed = false;
            if (pos)
            {
                const DiskEnemy *bpos = pos;
                for (uint32_t off = 0; off <= 8 && !placed; ++off)
                {
                    uint32_t sub = a.lotId + off;
                    uint32_t sflag = 0; int32_t skey = 0;
                    if (!goblin::lot_row_in_table(sub, 1, &sflag, &skey)) continue;
                    if (skey < 500000000 || skey >= 600000000) continue;
                    int32_t gid = skey - 500000000;
                    if (gid != 800010 && gid != 850010) continue;
                    if (!boss_piece_seen.insert(sub).second) { placed = true; break; }  // claimed already
                    const bool rune = (gid == 800010);
                    const int pcat = static_cast<int>(rune ? goblin::generated::Category::ReforgedRunePieces
                                                           : goblin::generated::Category::ReforgedEmberPieces);
                    from::paramdef::WORLD_MAP_POINT_PARAM_ST d{};
                    d.areaNo = bpos->area;
                    d.gridXNo = bpos->gx;
                    d.gridZNo = bpos->gz;
                    d.posX = bpos->posX;
                    d.posZ = bpos->posZ;
                    d.textId1 = (rune ? 800010 : 850010) + 500000000;  // GoodsName "Rune/Ember Piece"
                    d.textDisableFlagId1 = sflag;                      // obtain flag → graying
                    push_marker(/*row_id=*/sub, d, pcat, /*lotId=*/0u, /*lotType=*/0u, Source::DiskMSB);
                    // Dedup the baked c-model twin two ways: (1) exact identity (tile + boss part
                    // name) — works when the runtime's chosen entity == the bake's part; (2) a
                    // tile+type key — the ev1200 setter-join can pick a DIFFERENT boss entity than the
                    // bake recorded (so the part names differ), but a dungeon has ONE piece of each
                    // type per tile, so dropping the baked piece of the same type in the same tile is
                    // safe and clears the residual that the exact key leaves as a wrong-spot double.
                    if (!bpos->name.empty())
                        out_piece_keys.insert(piece_key(bpos->area, bpos->gx, bpos->gz, bpos->name.c_str()));
                    out_piece_keys.insert(piece_key(bpos->area, bpos->gx, bpos->gz,
                                                    rune ? "__BOSSRUNE__" : "__BOSSEMBER__"));
                    ++boss_piece_emitted;
                    placed = true;
                }
            }
            // event-1200: the piece is a BONUS alongside the award's OWN drop → always fall through to
            // the normal handling so the award still emits + de-bakes its lot (consuming all lotType-2
            // awards regressed ~135 baked emevd rows). bossReward: historically dropped on no-piece;
            // now it ALSO falls through so the BASE lot emits as a normal marker when it's a notable
            // non-piece item (the boss/scarab drops a piece AND a real reward — Radagon's Scarseal
            // beside its Rune Piece). The notability gate below filters pure-piece / non-item bases
            // (→ unclassified → skipped), so this adds only real boss-base drops + Radagon
            // (tools/_probe_loose_blast.py: 8 collateral, all notable boss drops).
            if (a.bossReward && !placed) ++boss_piece_no_piece;
        }
        if (!pos) { ++no_entity; continue; }  // no enemy/asset/loose-anchor position for this award
        uint64_t key2 = ((uint64_t)a.entityId << 32) | a.lotId;
        if (!seen.insert(key2).second) { ++dup; continue; }
        if (treasure_lots.count(a.lotId)) { ++treasure_dup; continue; }  // physical pickup already placed
        // Resolve identity with the award's lotType hint (1 = ItemLotParam_map / direct,
        // 2 = ItemLotParam_enemy / event-1200 boss drop), falling back to the other table.
        uint8_t lt = a.lotType;
        int32_t key = goblin::resolve_loot_item_textid(a.lotId, lt, -1);
        // Region Map fragment granted by an EMEVD award (the Altus Plateau map dropped by a scarab,
        // 90005300 @m60_39_51): no MSB Treasure, so build_disk_maps_markers misses it, and the loot
        // classifier deliberately skips map goods (→ it would otherwise fall to unclassified and stay
        // baked). Route it to WorldMaps here + record its cell so the finalize cell-dedup drops the
        // baked twin (bake pos == the scarab pos → same cell). Only when the maps pass is active
        // (maps_cells != null), else there's no dedup target and we'd draw a duplicate.
        if (maps_cells && key >= 500000000 && goblin::goods_is_map(key - 500000000))
        {
            const int mcat = static_cast<int>(goblin::generated::Category::WorldMaps);
            from::paramdef::WORLD_MAP_POINT_PARAM_ST md{};
            md.areaNo = pos->area; md.gridXNo = pos->gx; md.gridZNo = pos->gz;
            md.posX = pos->posX; md.posZ = pos->posZ;
            push_marker(/*row_id=*/a.lotId, md, mcat, a.lotId, lt, Source::DiskMSB);
            maps_cells->insert(cell_of(g_buckets[mcat].back()));
            covered.insert(a.lotId);
            ++emitted;
            continue;
        }
        int cat = goblin::item_marker_category(key);
        bool lc = false;
        if (cat < 0) { cat = goblin::classify_item_live(key); lc = (cat >= 0); }
        if (cat < 0 || cat >= NUM_CAT)
        {
            uint8_t other = (uint8_t)(a.lotType == 1 ? 2 : 1);
            int32_t key2b = goblin::resolve_loot_item_textid(a.lotId, other, -1);
            int cat2 = goblin::item_marker_category(key2b);
            bool lc2 = false;
            if (cat2 < 0) { cat2 = goblin::classify_item_live(key2b); lc2 = (cat2 >= 0); }
            if (cat2 >= 0 && cat2 < NUM_CAT) { lt = other; key = key2b; cat = cat2; lc = lc2; }
        }
        if (cat < 0 || cat >= NUM_CAT) { ++unclassified; continue; }
        from::paramdef::WORLD_MAP_POINT_PARAM_ST d{};
        d.areaNo = pos->area;
        d.gridXNo = pos->gx;
        d.gridZNo = pos->gz;
        d.posX = pos->posX;
        d.posZ = pos->posZ;
        push_marker(/*row_id=*/a.lotId, d, cat, a.lotId, lt, Source::DiskMSB, lc);
        covered.insert(a.lotId);  // drops the matching baked LootSource::Emevd row
        ++emitted;
        (a.lotType == 2 ? emit_ev1200 : emit_direct)++;
        if (a.bossReward) ++boss_base;  // bossReward base emitted alongside its piece (Radagon)
        watch_lot(a.lotId, "emit-emevd", "cat=" + std::to_string(cat));
        if (verbose) ++per_cat[cat];

        // ── Mechanism C: sequence-sibling expansion — shared emit_lot_siblings ──
        // The ItemLotParam row sequence base, base+1, base+2, … is one award bundle (e.g. a boss drops
        // Lhutel + Smithing Stone [2], or the Oracle Effigy + 3 Fortunes set). Walk BOTH tables (some
        // bundles cross tables — base in _enemy, reward sub-lots in _map). The _map (tbl 1) chain stops
        // at a treasure base; both skip award bases + treasure ground items already placed elsewhere.
        from::paramdef::WORLD_MAP_POINT_PARAM_ST at{};
        at.areaNo = pos->area;
        at.gridXNo = pos->gx;
        at.gridZNo = pos->gz;
        at.posX = pos->posX;
        at.posZ = pos->posZ;
        emit_lot_siblings(a.lotId, at, {{1, &treasure_lots}, {2, nullptr}},
                          /*skipA=*/&base_lots, /*skipB=*/&treasure_lots,
                          covered, sib_seen, sib, /*lot_tile=*/nullptr, /*tile_pack=*/0,
                          verbose ? &per_cat : nullptr);
    }
    spdlog::info("[LOOTDISK] emevd drops: {} base markers ({} direct + {} event-1200) + {} sequence-"
                 "siblings = {} total ({} awards; filtered: {} entity-not-an-msb-enemy, {} "
                 "(entity,lot)-dedup, {} treasure-dup, {} base-unclassified; siblings: {} rune/ember-"
                 "skipped, {} unclassified)",
                 emitted, emit_direct, emit_ev1200, sib.emitted, emitted + sib.emitted,
                 (int)awards.size(), no_entity, dup, treasure_dup, unclassified, sib.runeember,
                 sib.unclassified);
    g_skip.no_anchor += no_entity;
    g_skip.dedup += dup + treasure_dup;
    g_skip.unclassified += unclassified + sib.unclassified;
    g_skip.by_design += sib.runeember;
    spdlog::info("[LOOTDISK] emevd boss-reward pieces: {} Rune/Ember Pieces emitted (Reforged, at boss "
                 "pos, flag-gray); {} boss awards with no piece in base..base+8",
                 boss_piece_emitted, boss_piece_no_piece);
    spdlog::info("[LOOTDISK] emevd loose-recovery: {} via asset-anchor, {} via loose-anchor window, "
                 "{} boss-base lots emitted (e.g. Radagon's Scarseal beside its piece); {} MSB assets indexed",
                 dbg_asset, dbg_loose, boss_base, (int)asset_enemies.size());
    if (verbose)
        for (auto &[cat, n] : per_cat)
            spdlog::info("[LOOTDISK]   emevd-drop category index {} = {} markers", cat, n);
}

// World features from disk MSBs (config world_features_from_disk). A World feature is an
// AEG asset placement whose model maps to a marker category in the GENERATED
// WORLD_FEATURE_MODELS table (Stakes of Marika, Imp Statues, Hero's Tomb, …) — the SAME
// source tools/generate_*.py baked from, so reading it live reproduces the bake on any mod
// with no per-feature C++. This is ONE generic pass: adding an asset-model World feature =
// ONE row in tools/world_feature_assets.py (regen the table), zero code here.
//
// Per placement: look the asset's aegRow up in the table; if found, emit a marker
// (textId1 from the table for the tooltip; icon/category from the category). An
// `entity_required` row keeps ONLY placements that carry an MSB EntityID — the INTERACTIVE
// instances (Imp seals, Hero's Tomb statues); the same model placed as decoration carries
// none. The row's `flag_rule` resolves the graying flag (so an activated/looted instance
// hides like the bake) LIVE from the mod's files — no committed bake: Imp seals derive it
// arithmetically (+ a seal-suffix filter and a per-suffix key label), Hero's Tomb joins
// `emevd_flags` (entityId → EMEVD activated flag). Each placed 0.5u cell is recorded
// per-category in `out_cells` for (a) disk-internal dedup (a feature in two overlapping LOD0
// tiles → one marker) and (b) the finalize pass (category-wipe vs cell-dedup).
static int build_disk_world_feature_markers(
    const std::vector<DiskCollectible> &assets,
    const std::unordered_map<uint32_t, uint32_t> &emevd_flags,
    std::unordered_map<int, std::unordered_set<Cell, CellHash>> &out_cells)
{
    namespace gen = goblin::generated;
    GOBLIN_BENCH("build.disk_world_features");
    int emitted = 0, dup = 0, no_entity = 0, rule_skipped = 0, no_flag = 0;
    std::unordered_map<int, int> per_cat;
    for (const DiskCollectible &a : assets)
    {
        // Editorial model→feature lookup (tiny table → linear scan).
        const gen::WorldFeatureModel *wf = nullptr;
        for (size_t k = 0; k < gen::WORLD_FEATURE_MODEL_COUNT; ++k)
            if (gen::WORLD_FEATURE_MODELS[k].aeg_row == a.aegRow)
            {
                wf = &gen::WORLD_FEATURE_MODELS[k];
                break;
            }
        if (!wf) continue;
        // Interactive-only features: skip decoration copies of the model (no EntityID).
        if (wf->entity_required && a.entityId == 0) { ++no_entity; continue; }

        // Resolve the graying flag (+ optional label override / reject) from the flag rule.
        uint32_t collect_flag = 0;
        int32_t  text_override = 0;
        switch (wf->flag_rule)
        {
        case gen::FlagRule::ImpSeal:
        {
            // Only the 4 real seal entity-suffixes are interactive seals (matches the bake's
            // filter — also drops the non-seal AEG027_078/079 the entity gate alone let in).
            const uint32_t suffix = a.entityId % 1000;
            if (suffix != 570 && suffix != 575 && suffix != 565 && suffix != 611)
            { ++rule_skipped; continue; }
            // Activation flag = tile_base + suffix (NOT the entity id). m60/m61 (DLC) tile
            // their flags differently. Mirrors tools/generate_imp_statues.py exactly.
            const uint32_t tile_base =
                (a.area == 60 || a.area == 61)
                    ? ((a.area == 60 ? 10u : 20u) * 100000000u + (uint32_t)a.gx * 1000000u +
                       (uint32_t)a.gz * 10000u)
                    : ((uint32_t)a.area * 1000000u + (uint32_t)a.gx * 10000u);
            collect_flag = tile_base + suffix;
            text_override = (suffix == 565) ? 500008186 : 500008000;  // Imbued vs Stonesword Key
            break;
        }
        case gen::FlagRule::HeroTombEmevd:
        {
            auto it = emevd_flags.find(a.entityId);
            if (it != emevd_flags.end()) collect_flag = it->second;
            else ++no_flag;  // statue absent from the EMEVD scan → drawn, just no graying
            break;
        }
        case gen::FlagRule::SealEmevd:
        {
            // Seal Puzzles: the activation flag is the EMEVD template-90006051 flag joined by
            // EntityID. SELF-GATING — AEG099_090 is a common model also placed as decoration; a
            // placement with NO 90006051 binding is not an interactive seal, so SKIP it (stronger
            // than entity_required alone, avoids phantom non-seal markers). Mirrors the bake, which
            // only emitted seals the 90006050/51 template pair referenced (extract_seal_puzzles.py).
            auto it = emevd_flags.find(a.entityId);
            if (it == emevd_flags.end()) { ++rule_skipped; continue; }
            collect_flag = it->second;
            break;
        }
        case gen::FlagRule::None:
            break;  // respawn points (Stakes): no graying flag, by design.
            // NO default: — the switch is exhaustive over FlagRule, so adding a new rule
            // without a case here is a clang-cl -Wswitch warning (compile-time), not a silent
            // no-flag marker at runtime.
        }

        const int cat = static_cast<int>(wf->category);
        from::paramdef::WORLD_MAP_POINT_PARAM_ST d{};
        d.areaNo = a.area;
        d.gridXNo = a.gx;
        d.gridZNo = a.gz;
        d.posX = a.posX;
        d.posZ = a.posZ;
        d.posY = a.posY;  // DX item 7: altitude badge
        d.textId1 = text_override ? text_override : wf->text_id;  // tutorial / FMG / ActionButtonText
        d.textDisableFlagId1 = collect_flag;  // → push_marker collected_flag (graying/census), 0 = never grays
        push_marker(/*row_id=*/0ull, d, cat, /*lotId=*/0u, /*lotType=*/0u, Source::DiskMSB);
        // Dedup by projected cell (same key the baked drop uses). Insert AFTER push so the
        // projection is the marker's own; a duplicate placement → drop the just-pushed copy
        // but KEEP the cell recorded (the baked twin must still be dropped at finalize).
        const Cell cell = cell_of(g_buckets[cat].back());
        if (!out_cells[cat].insert(cell).second)
        {
            g_buckets[cat].pop_back();
            ++dup;
            continue;
        }
        ++emitted;
        ++per_cat[cat];
    }
    spdlog::info("[LOOTDISK] world features: {} markers from disk across {} categories ({} dup-cell, "
                 "{} no-entity, {} rule-rejected, {} interactive-without-emevd-flag)",
                 emitted, (int)per_cat.size(), dup, no_entity, rule_skipped, no_flag);
    for (const auto &[c, n] : per_cat)
        spdlog::info("[LOOTDISK]   world-feature category {} = {} markers", c, n);
    return emitted;
}

// One ShopLineupParam_ST row, raw bytes (row size 0x34 = 52, pinned from the paramdef field walk
// + GetRowSize()==52, tools/_probe_shop_offsets.py). Fields read by validated byte offset:
//   equipId @+0x00 (s32) · sellQuantity @+0x14 (s16) · equipType @+0x17 (u8).
struct RawShopRow { uint8_t b[0x34]; };

// Build the set of marker item-keys for items sold INFINITE-stock (sellQuantity == -1) in the live
// ShopLineupParam — the merchant inventory. The key encoding mirrors resolve_loot_item_textid /
// encode_live_item (item id + category offset), so it compares directly against a baked loot
// marker's resolved key. equipType → ItemLotParam category: 0 weapon→+100M, 1 protector→+200M,
// 2 accessory→+300M, 3 goods→+500M (gem/other skipped — none of the phantom set are gems). Used to
// drop the bake's unmatched-ItemLotParam fallback phantoms (items with NO world placement that the
// bake dumped at the tile corner). Empty on any failure (param absent) → the drop pass no-ops.
static std::unordered_set<int32_t> build_shop_infinite_keys()
{
    std::unordered_set<int32_t> keys;
    try
    {
        auto shop = from::params::get_param<RawShopRow>(L"ShopLineupParam");
        for (auto entry : shop)
        {
            const RawShopRow &r = entry.second;
            int32_t equipId      = *reinterpret_cast<const int32_t *>(r.b + 0x00);
            int16_t sellQuantity = *reinterpret_cast<const int16_t *>(r.b + 0x14);
            uint8_t equipType    = r.b[0x17];
            if (sellQuantity != -1 || equipId <= 0) continue;  // only infinite-stock merchant items
            int32_t off;
            switch (equipType)
            {
                case 0: off = 100000000; break;  // weapon  (ItemLot cat 2)
                case 1: off = 200000000; break;  // protector / armour (cat 3)
                case 2: off = 300000000; break;  // accessory / talisman (cat 4)
                case 3: off = 500000000; break;  // goods (cat 1)
                default: continue;               // gem / ash-of-war etc. — not a phantom category
            }
            keys.insert(equipId + off);
        }
    }
    catch (...) {}
    return keys;
}

// Minimal raw-offset view of a SignPuddleParam row (the no-bake Summoning Pools source).
// Only the fields the marker needs; get_param<T> casts T* over the live row bytes. ⚠ The
// paramdef field NAMES (unknown_0x28 etc.) are LABELS, not byte offsets — the REAL offsets,
// pinned from the paramdef field layout AND a raw-row scan (row 670099: int 45 @ +0x10, pos
// (4.69,1.12,-25.95) @ +0x14; tools/probe_signpuddle_offsets.py), are 0x18 lower. The marker's
// graying flag is the ROW ID itself (670XXX), which the EMEVD sets when the pool is unlocked.
struct SignPuddleRow
{
    uint8_t _pad[0x10];
    int32_t mapRef;  // +0x10  dungeon: area + block*256 ; overworld: >=100000 (position-join)
    float   posX;    // +0x14
    float   posY;    // +0x18
    float   posZ;    // +0x1c
};
static_assert(offsetof(SignPuddleRow, mapRef) == 0x10 && offsetof(SignPuddleRow, posX) == 0x14 &&
                  offsetof(SignPuddleRow, posZ) == 0x1c,
              "SignPuddleParam offsets");

// Minimal raw-offset view of a GestureParam row (the no-bake Loot - Gestures name source).
// Layout: byte 0 = disableParam bitfield, bytes 1-3 = reserve, then s32 itemId @ +0x04 (the
// gesture's goods id; its name lives in GoodsName FMG, copied to PlaceName at the 500M offset).
// itemId@+0x04 pinned EMPIRICALLY vs the raw regulation row (row 0 dataOffset 0x598 → bytes
// 00 00 00 00 | 28 23 00 00 = 0x2328 = 9000 @ +0x04, matches the paramdef-resolved value;
// tools probe). The paramdef field NAMES are labels — this is the validated byte offset.
struct GestureParamRow
{
    uint8_t _pad[0x04];
    int32_t itemId;  // +0x04  gesture goods id → name (textId1 = 500000000 + itemId)
};
static_assert(offsetof(GestureParamRow, itemId) == 0x04, "GestureParam.itemId offset");

// World feature: Summoning Pools (config world_features_from_disk). Each pool is a live
// SignPuddleParam row (no committed bake) — the SAME source tools/generate_summoning_pools.py
// baked from. The row gives the world position (posX/Z) + a map reference: dungeon pools
// (mapRef < 100000) decode their tile arithmetically (area + block*256); overworld pools
// (mapRef >= 100000) carry no tile, so we position-join to the nearest AEG099_015 puddle asset
// (already parsed into `assets` by the disk enumeration) within 10u. The row id is the graying
// flag (textDisableFlagId1 → push_marker collected_flag; the pool's icon hides once unlocked).
// SignPuddleParam has per-context duplicate rows for one physical pool (different rid, near-
// identical pos), so dedup within 3u per tile (matches the bake). Source::Live (param-sourced,
// like bosses); needs live params → empty until the in-game build (try/catch, same as bosses).
static int build_live_summoning_pools(const std::vector<DiskCollectible> &assets,
                                      std::unordered_set<Cell, CellHash> &out_cells)
{
    namespace gen = goblin::generated;
    GOBLIN_BENCH("build.summoning_pools");
    const int cat = static_cast<int>(gen::Category::WorldSummoningPools);
    constexpr uint32_t kPuddleAeg = 99015;  // AEG099_015 = the summoning sign-puddle asset

    // Overworld puddle asset positions for the position-join (dungeon pools skip this).
    struct PuddleAsset { uint8_t area, gx, gz; float x, z; };
    std::vector<PuddleAsset> puddles;
    for (const DiskCollectible &a : assets)
        if (a.aegRow == kPuddleAeg)
            puddles.push_back({a.area, a.gx, a.gz, a.posX, a.posZ});

    // Resolve each row to a tile, collect, then dedup by position before emitting.
    struct Pool { uint64_t rid; uint8_t area, gx, gz; float x, z; };
    std::vector<Pool> pools;
    int unresolved = 0, total = 0;
    try
    {
        for (auto [rid, row] : from::params::get_param<SignPuddleRow>(L"SignPuddleParam"))
        {
            if (rid == 0) continue;
            ++total;
            uint8_t area, gx = 0, gz = 0;
            if (row.mapRef > 0 && row.mapRef < 100000)
            {
                area = static_cast<uint8_t>(row.mapRef % 256);
                const uint32_t block = static_cast<uint32_t>(row.mapRef) / 256;
                if (area != 60 && area != 61 && block > 0) gx = static_cast<uint8_t>(block);
            }
            else
            {
                float best = 10.0f;  // join radius (matches the bake's < 10u)
                const PuddleAsset *bm = nullptr;
                for (const PuddleAsset &p : puddles)
                {
                    const float dx = row.posX - p.x, dz = row.posZ - p.z;
                    const float d = std::sqrt(dx * dx + dz * dz);
                    if (d < best) { best = d; bm = &p; }
                }
                if (!bm) { ++unresolved; continue; }
                area = bm->area; gx = bm->gx; gz = bm->gz;
            }
            pools.push_back({rid, area, gx, gz, row.posX, row.posZ});
        }
    }
    catch (...)
    {
        spdlog::warn("[LOOTDISK] SignPuddleParam not readable — Summoning Pools deferred this build");
        return 0;
    }

    int emitted = 0, dup = 0;
    std::vector<Pool> kept;
    for (const Pool &p : pools)
    {
        // Per-context dup rows: same physical pool, near-identical pos (bake dedup, 3u/tile).
        bool is_dup = false;
        for (const Pool &q : kept)
            if (p.area == q.area && p.gx == q.gx && p.gz == q.gz &&
                std::fabs(p.x - q.x) < 3.0f && std::fabs(p.z - q.z) < 3.0f)
            { is_dup = true; break; }
        if (is_dup) { ++dup; continue; }
        kept.push_back(p);

        from::paramdef::WORLD_MAP_POINT_PARAM_ST d{};
        d.areaNo = p.area;
        d.gridXNo = p.gx;
        d.gridZNo = p.gz;
        d.posX = p.x;
        d.posZ = p.z;
        d.textId1 = 900301690;          // tutorial text "Summoning Pool"
        d.textDisableFlagId1 = static_cast<int>(p.rid);  // graying: rid is the unlock flag
        push_marker(/*row_id=*/p.rid, d, cat, /*lotId=*/0u, /*lotType=*/0u, Source::Live);
        out_cells.insert(cell_of(g_buckets[cat].back()));
        ++emitted;
    }
    spdlog::info("[LOOTDISK] world features: {} Summoning Pools from live SignPuddleParam "
                 "({} rows: {} pos-dedup, {} overworld-unresolved, {} puddle assets)",
                 emitted, total, dup, unresolved, (int)puddles.size());
    return emitted;
}

// World feature: Hostile NPC invaders (config world_features_from_disk). Each is a placed MSB
// Enemy (entityId > 0) whose LIVE NpcParam marks it a NAMED invader — teamType ∈ {24,27} AND
// nameId > 0. The nameId>0 gate is the canonical signal that separates real invaders from mobs
// sharing the team (bloodfiends c4280, dungeon battlemages, scarabs c419x — none have an
// NpcName entry). No bake: identity/filter from live NpcParam, the defeat flag from EMEVD
// template 90005792 (entity→flag in `emevd_flags`), the position from the disk enemy part.
// textId1 = nameId + 700000000 (NpcName FMG, runtime-localized); the defeat flag drives the
// cleared checkmark / hide-on-kill (clearedEventFlagId, like live bosses). Dedup by projected
// cell (invader variants stacked at one trigger spot collapse). Dedicated category → wipe.
static int build_disk_hostile_npc_markers(
    const std::vector<DiskEnemy> &enemies,
    const std::unordered_map<uint32_t, uint32_t> &emevd_flags,
    std::unordered_set<Cell, CellHash> &out_cells)
{
    namespace gen = goblin::generated;
    GOBLIN_BENCH("build.disk_hostile_npc");
    const int cat = static_cast<int>(gen::Category::WorldHostileNPC);
    int emitted = 0, dup = 0, not_invader = 0, no_entity = 0, no_flag = 0;
    // Dedup by the bake's key — per tile + 0.1u-rounded RAW position (stacked invader variants
    // at one trigger spot collapse). Keyed on raw pos (NOT the projected cell): a 0.5u world
    // cell wrongly merges two distinct invaders a few decimetres apart that the engine places
    // separately. out_cells still gets each emitted cell, only as the finalize "ran" signal.
    std::unordered_set<std::string> seen_pos;
    for (const DiskEnemy &e : enemies)
    {
        if (e.entityId == 0) { ++no_entity; continue; }  // placed only (not script-spawned)
        uint8_t team = 0;
        int32_t name = 0;
        if (!goblin::npc_team_and_name(e.npcParamId, &team, &name)) { ++not_invader; continue; }
        if ((team != 24 && team != 27) || name <= 0) { ++not_invader; continue; }
        std::string pk = std::to_string(e.area) + "_" + std::to_string(e.gx) + "_" +
                         std::to_string(e.gz) + "_" + std::to_string((long)std::lround(e.posX * 10.0f)) +
                         "_" + std::to_string((long)std::lround(e.posZ * 10.0f));
        if (!seen_pos.insert(pk).second) { ++dup; continue; }
        uint32_t flag = 0;
        if (auto it = emevd_flags.find(e.entityId); it != emevd_flags.end()) flag = it->second;
        else ++no_flag;  // invader with no 90005792 defeat flag (drawn, just no kill-tracking)

        from::paramdef::WORLD_MAP_POINT_PARAM_ST d{};
        d.areaNo = e.area;
        d.gridXNo = e.gx;
        d.gridZNo = e.gz;
        d.posX = e.posX;
        d.posZ = e.posZ;
        d.posY = e.posY;  // DX item 7: altitude badge
        d.textId1 = name + 700000000;          // NpcName FMG (runtime-localized)
        d.clearedEventFlagId = flag;           // defeated → checkmark / hide (like bosses)
        d.textDisableFlagId1 = flag;           // also the collected_flag graying path
        push_marker(/*row_id=*/e.entityId, d, cat, /*lotId=*/0u, /*lotType=*/0u, Source::DiskMSB);
        out_cells.insert(cell_of(g_buckets[cat].back()));  // finalize "pass ran" signal
        ++emitted;
    }
    spdlog::info("[LOOTDISK] world features: {} Hostile NPC invaders from disk enemies + live NpcParam "
                 "({} dup-cell, {} not-named-invader, {} no-entity, {} no-defeat-flag)",
                 emitted, dup, not_invader, no_entity, no_flag);
    return emitted;
}

// World feature: Spirit Springs (config world_features_from_disk). A spring is a MountJump
// (region subtype 46) / LockedMountJump (54) LAUNCH point, an Others region named
// "FakeSpiritSpringJump" (ERR's manual springs), or a DLC AEG463_200 asset (springs without a
// region). All from disk MSBs — no bake. `regions` is already filtered to those three by the
// parser; AEG463_200 comes from the asset enumeration. Springs carry no flag (a spring never
// "completes" — the unlock flag lives on its Spiritspring Hawk), so no graying. textId1 =
// "Spiritspring Jumping". Dedup by the bake key (area + 1u-rounded pos) so the launch point +
// any co-located asset/region collapse to one icon. Dedicated category → category-wipe.
static int build_disk_spirit_springs_markers(
    const std::vector<DiskRegion> &regions,
    const std::vector<DiskCollectible> &assets,
    std::unordered_set<Cell, CellHash> &out_cells)
{
    namespace gen = goblin::generated;
    GOBLIN_BENCH("build.disk_spirit_springs");
    const int cat = static_cast<int>(gen::Category::WorldSpiritSprings);
    constexpr uint32_t kSpringAeg = 463200;  // AEG463_200 = DLC spirit-spring asset
    int emitted = 0, dup = 0;
    std::unordered_set<std::string> seen;  // bake dedup: (area, round x, round z)
    auto emit = [&](uint8_t area, uint8_t gx, uint8_t gz, float x, float z) {
        std::string k = std::to_string(area) + "_" + std::to_string((long)std::lround(x)) + "_" +
                        std::to_string((long)std::lround(z));
        if (!seen.insert(k).second) { ++dup; return; }
        from::paramdef::WORLD_MAP_POINT_PARAM_ST d{};
        d.areaNo = area;
        d.gridXNo = gx;
        d.gridZNo = gz;
        d.posX = x;
        d.posZ = z;
        d.textId1 = 900301620;  // tutorial text "Spiritspring Jumping"
        push_marker(/*row_id=*/0ull, d, cat, /*lotId=*/0u, /*lotType=*/0u, Source::DiskMSB);
        out_cells.insert(cell_of(g_buckets[cat].back()));
        ++emitted;
    };
    for (const DiskRegion &r : regions)  // pre-filtered to MountJump/Locked/FakeSpiring (+ Kindling)
        if (r.name.rfind("KindlingSpirit_", 0) != 0)  // Kindling SFX regions ride the same vector — skip
            emit(r.area, r.gx, r.gz, r.posX, r.posZ);
    for (const DiskCollectible &a : assets)
        if (a.aegRow == kSpringAeg) emit(a.area, a.gx, a.gz, a.posX, a.posZ);
    spdlog::info("[LOOTDISK] world features: {} Spirit Springs from disk ({} regions + AEG463_200 "
                 "assets; {} pos-dedup)", emitted, (int)regions.size(), dup);
    return emitted;
}

// World feature: Spiritspring Hawks (config world_features_from_disk). Each is a placed MSB Enemy
// of model c4210 (the Stormhawk) with EntityID%10000 ∈ {980,971} — the hawk whose death unlocks a
// sealed spirit spring. No bake: from disk enemies; model identified by the part name (starts with
// "c4210"); the clearedEventFlagId = the hawk's EntityID (set on kill = spring unlocked → checkmark
// / hide). textId1 = "Spiritspring Stormhawk". Dedup by EntityID. Dedicated category → category-wipe.
static int build_disk_spiritspring_hawks_markers(
    const std::vector<DiskEnemy> &enemies,
    std::unordered_set<Cell, CellHash> &out_cells)
{
    namespace gen = goblin::generated;
    GOBLIN_BENCH("build.disk_hawks");
    const int cat = static_cast<int>(gen::Category::WorldSpiritspringHawks);
    int emitted = 0, dup = 0;
    std::unordered_set<uint32_t> seen;  // by EntityID (the bake key)
    for (const DiskEnemy &e : enemies)
    {
        if (e.entityId == 0) continue;
        const uint32_t suf = e.entityId % 10000;
        if (suf != 980 && suf != 971) continue;
        if (e.name.compare(0, 5, "c4210") != 0) continue;  // Stormhawk model (part name prefix)
        if (!seen.insert(e.entityId).second) { ++dup; continue; }
        from::paramdef::WORLD_MAP_POINT_PARAM_ST d{};
        d.areaNo = e.area;
        d.gridXNo = e.gx;
        d.gridZNo = e.gz;
        d.posX = e.posX;
        d.posZ = e.posZ;
        d.posY = e.posY;  // DX item 7: altitude badge
        d.textId1 = 904210304;              // "Spiritspring Stormhawk"
        d.clearedEventFlagId = e.entityId;  // hawk-kill flag = spring unlock (checkmark/hide)
        d.textDisableFlagId1 = e.entityId;  // also the collected_flag graying path
        push_marker(/*row_id=*/e.entityId, d, cat, /*lotId=*/0u, /*lotType=*/0u, Source::DiskMSB);
        out_cells.insert(cell_of(g_buckets[cat].back()));
        ++emitted;
    }
    spdlog::info("[LOOTDISK] world features: {} Spiritspring Hawks from disk enemies "
                 "(c4210, EntityID suffix 980/971; {} dup)", emitted, dup);
    return emitted;
}

// World feature: Maps (config world_features_from_disk). Each region map fragment is a goods
// pickup placed as an MSB Treasure (a map-stele AEG asset). No bake: scan the disk treasures,
// resolve each lot's item, keep the map goods (goods_is_map: sortGroupId ∈ {190,191}). The label
// (map name), inventory icon, and collected flag come LIVE from the lot via push_marker — exactly
// like disk loot but routed to WorldMaps. Dedup by goods id (one icon per map). The 1 EMEVD-granted
// map (Altus Plateau) has no MSB Treasure → the finalize CELL-dedup keeps its baked row, so all 24
// show (23 disk + 1 baked).
static int build_disk_maps_markers(const std::vector<DiskTreasure> &treasures,
                                   std::unordered_set<Cell, CellHash> &out_cells)
{
    namespace gen = goblin::generated;
    GOBLIN_BENCH("build.disk_maps");
    const int cat = static_cast<int>(gen::Category::WorldMaps);
    int emitted = 0, dup = 0;
    std::unordered_set<int32_t> seen;  // by goods id (one icon per map)
    for (const DiskTreasure &t : treasures)
    {
        const int32_t key = goblin::resolve_loot_item_textid(t.lotId, 1, -1);
        if (key < 500000000) continue;             // not a goods item (goods = +500M encoding)
        const int32_t gid = key - 500000000;
        if (!goblin::goods_is_map(gid)) continue;  // not a region map fragment
        if (!seen.insert(gid).second) { ++dup; continue; }
        from::paramdef::WORLD_MAP_POINT_PARAM_ST d{};
        d.areaNo = t.area;
        d.gridXNo = t.gx;
        d.gridZNo = t.gz;
        d.posX = t.posX;
        d.posZ = t.posZ;
        d.posY = t.posY;  // DX item 7: altitude badge
        // textId1 left -1 → push_marker resolves the map's name + icon + collected flag from the lot.
        push_marker(/*row_id=*/t.lotId, d, cat, t.lotId, /*lotType=*/1, Source::DiskMSB);
        out_cells.insert(cell_of(g_buckets[cat].back()));
        ++emitted;
    }
    spdlog::info("[LOOTDISK] world features: {} Maps from disk treasures (sortGroupId 190/191; {} dup)",
                 emitted, dup);
    return emitted;
}

// World feature: Paintings (config world_features_from_disk). Each painting is an MSB part —
// 4 are Asset frames (AEG099_386/388/389/391), 7 are ghost-painter Enemies (c4300/c6320) —
// REFERENCED by an EMEVD painting-collection event (flag 580000-580199). No bake: the
// (entity→flag) map comes from the EMEVD painting scan (parse_emevd_paintings, folded into the
// World-feature flag scan), the position from the matching disk Asset/Enemy part (both carry
// EntityID). textId1 = the painting's GoodsName FMG, DERIVED from the flag exactly like
// tools/generate_paintings.py (so the marker is icon+name-identical to the bake it replaces);
// textDisableFlagId1 = the collection flag (graying/census). Dedup by flag (one icon per
// painting). Dedicated category → category-wipe finalize (the default for a non-asset-table cat).
static int build_disk_painting_markers(
    const std::vector<DiskCollectible> &assets,
    const std::vector<DiskEnemy> &enemies,
    const std::unordered_map<uint32_t, uint32_t> &painting_flags,  // entity → flag
    std::unordered_set<Cell, CellHash> &out_cells)
{
    namespace gen = goblin::generated;
    GOBLIN_BENCH("build.disk_paintings");
    const int cat = static_cast<int>(gen::Category::WorldPaintings);
    // entity → position, from BOTH part types (the 11 split 4 Asset / 7 Enemy). First writer
    // wins (a phase-variant tile reuses the same EntityID). posY unused (map is 2D).
    struct PaintPos { uint8_t area, gx, gz; float x, z; };
    std::unordered_map<uint32_t, PaintPos> entity_pos;
    for (const DiskCollectible &a : assets)
        if (a.entityId) entity_pos.emplace(a.entityId, PaintPos{a.area, a.gx, a.gz, a.posX, a.posZ});
    for (const DiskEnemy &e : enemies)
        if (e.entityId) entity_pos.emplace(e.entityId, PaintPos{e.area, e.gx, e.gz, e.posX, e.posZ});

    int emitted = 0, no_pos = 0, dup = 0;
    std::unordered_set<uint32_t> seen;  // by flag (one icon per painting)
    for (const auto &pf : painting_flags)
    {
        const uint32_t entity = pf.first, flag = pf.second;
        auto it = entity_pos.find(entity);
        if (it == entity_pos.end()) { ++no_pos; continue; }  // painting entity not in a parsed _00 tile
        if (!seen.insert(flag).second) { ++dup; continue; }
        const PaintPos &p = it->second;
        // GoodsName FMG id, derived from the flag (mirrors tools/generate_paintings.py):
        //   base (580000-580099): goods 8200    + (flag-580000)/10
        //   DLC  (580100-580199): goods 2008200 + (flag-580100)/10
        const int32_t goods = (flag >= 580100) ? (2008200 + (int32_t)(flag - 580100) / 10)
                                               : (8200 + (int32_t)(flag - 580000) / 10);
        from::paramdef::WORLD_MAP_POINT_PARAM_ST d{};
        d.areaNo = p.area;
        d.gridXNo = p.gx;
        d.gridZNo = p.gz;
        d.posX = p.x;
        d.posZ = p.z;
        d.textId1 = 500000000 + goods;   // painting name (GoodsName FMG) — same value the bake stores
        d.textDisableFlagId1 = flag;     // collected → graying/census (push_marker collected_flag path)
        push_marker(/*row_id=*/entity, d, cat, /*lotId=*/0u, /*lotType=*/0u, Source::DiskMSB);
        out_cells.insert(cell_of(g_buckets[cat].back()));
        ++emitted;
    }
    spdlog::info("[LOOTDISK] world features: {} Paintings from EMEVD events + disk parts "
                 "({} no-position, {} dup-flag)", emitted, no_pos, dup);
    return emitted;
}

// World feature: Gestures (config world_features_from_disk). Each gesture pickup is an MSB Asset
// REFERENCED by a common-template-90005570 EMEVD init (the gesture-spawn template): the event
// carries (flag, gestureParam, entity). No bake: `refs` come from the EMEVD gesture scan
// (parse_emevd_gestures, folded into the World-feature flag scan); the position from the matching
// disk Asset; the NAME from GestureParam[gestureParam].itemId read LIVE (a goods id → GoodsName
// FMG at the 500M offset). textDisableFlagId1 = the gesture-learned flag (graying/census). Dedup
// by entity (one icon per placed asset). Dedicated category → category-wipe finalize.
static int build_disk_gesture_markers(
    const std::vector<DiskCollectible> &assets,
    const std::vector<msbe::GestureRef> &refs,
    std::unordered_set<Cell, CellHash> &out_cells)
{
    namespace gen = goblin::generated;
    GOBLIN_BENCH("build.disk_gestures");
    const int cat = static_cast<int>(gen::Category::LootGestures);

    // gestureParam → itemId, read LIVE from GestureParam (no bake). Empty until the in-game build
    // (try/catch, like Summoning Pools / bosses) — a gesture with no live itemId still draws, just
    // nameless (which the engine hides), so the live param is required for the marker to render.
    std::unordered_map<uint32_t, int32_t> gparam_item;
    try
    {
        for (auto [rid, row] : from::params::get_param<GestureParamRow>(L"GestureParam"))
            if (row.itemId > 0) gparam_item.emplace((uint32_t)rid, row.itemId);
    }
    catch (...) {}

    // entity → Asset position (the gesture assets are all type-13 Assets). First writer wins.
    struct GPos { uint8_t area, gx, gz; float x, z; };
    std::unordered_map<uint32_t, GPos> entity_pos;
    for (const DiskCollectible &a : assets)
        if (a.entityId) entity_pos.emplace(a.entityId, GPos{a.area, a.gx, a.gz, a.posX, a.posZ});

    int emitted = 0, no_pos = 0, dup = 0, no_name = 0;
    std::unordered_set<uint32_t> seen;  // by entity (one icon per placed asset)
    for (const msbe::GestureRef &g : refs)
    {
        auto it = entity_pos.find(g.entityId);
        if (it == entity_pos.end()) { ++no_pos; continue; }  // entity not a placed _00 asset
        if (!seen.insert(g.entityId).second) { ++dup; continue; }
        auto ni = gparam_item.find(g.gestureParam);
        const int32_t item_id = (ni != gparam_item.end()) ? ni->second : 0;
        if (item_id <= 0) ++no_name;  // drawn, but nameless (live param not ready / unknown row)
        const GPos &p = it->second;
        from::paramdef::WORLD_MAP_POINT_PARAM_ST d{};
        d.areaNo = p.area;
        d.gridXNo = p.gx;
        d.gridZNo = p.gz;
        d.posX = p.x;
        d.posZ = p.z;
        d.textId1 = item_id > 0 ? (500000000 + item_id) : 0;  // gesture goods name (GoodsName FMG)
        d.textDisableFlagId1 = g.flag;                         // learned → graying/census
        push_marker(/*row_id=*/g.entityId, d, cat, /*lotId=*/0u, /*lotType=*/0u, Source::DiskMSB);
        out_cells.insert(cell_of(g_buckets[cat].back()));
        ++emitted;
    }
    spdlog::info("[LOOTDISK] world features: {} Gestures from EMEVD events + disk assets "
                 "({} no-position, {} dup-entity, {} nameless; {} live GestureParam rows)",
                 emitted, no_pos, dup, no_name, (int)gparam_item.size());
    return emitted;
}

// World feature: Great Runes (config world_features_from_disk). Each of the 6 demigod great runes
// is shown at its boss's location and grays when the boss is defeated (= rune obtained — great runes
// are auto-awarded on demigod death and can't be dropped/sold). No bake: the position AND the graying
// flag come LIVE from the matching boss marker (build_live_bosses → WorldMapPointParam), joined by the
// boss's clearedEventFlagId. Runs AFTER build_live_bosses (needs its markers); category-wipe finalize.
//
// The rune→boss link is the one irreducibly editorial bit (which demigod drops which rune), but it was
// DERIVED, not hand-curated: EquipParamGoods.goodsType==15 identifies EXACTLY these 6 runes; each rune's
// GoodsName shares its demigod's proper noun with the boss name; the lone collision (Radahn = Starscourge
// vs the DLC "Consort of Miquella") resolves by "great runes are base-game" → exclude DLC-area bosses.
// That offline match produced the {rune_goods_id → boss_clearedEventFlagId} pairs below, so the runtime
// needs only a NUMERIC flag-join — no FMG strings, locale-independent, build-order safe.
//
// TODO(zero-manual-table): to drop even these 6 pairs, do the name-match LIVE — iterate EquipParamGoods
// (goodsType==15), resolve each rune's GoodsName + each live boss's name via the game MsgRepository, match
// on the shared proper noun (excluding DLC areas), then join. Deferred: the marker-build pass runs BEFORE
// the DLL's expanded-FMG is built (lookup_text would miss), so it needs a direct MsgRepository read +
// locale-robust matching — not worth the fragility for 6 markers ER is very unlikely to ever extend with
// a new great-rune demigod.
static int build_great_rune_markers(std::unordered_set<Cell, CellHash> &out_cells)
{
    namespace gen = goblin::generated;
    GOBLIN_BENCH("build.great_runes");
    const int cat = static_cast<int>(gen::Category::KeyGreatRunes);
    struct RuneLink { int32_t rune_id; int cleared_flag; };
    static constexpr RuneLink GREAT_RUNES[] = {
        {191, 510010},  // Godrick's Great Rune  — Godrick the Grafted
        {192, 510300},  // Radahn's Great Rune   — Starscourge Radahn (base; NOT the DLC Consort)
        {193, 510040},  // Morgott's Great Rune  — Morgott, the Omen King
        {194, 510220},  // Rykard's Great Rune   — Rykard, Lord of Blasphemy
        {195, 510120},  // Mohg's Great Rune     — Mohg, Lord of Blood
        {196, 510200},  // Malenia's Great Rune  — Malenia, Blade of Miquella
    };
    const auto &bosses = g_buckets[static_cast<int>(gen::Category::WorldBosses)];
    int emitted = 0, no_boss = 0;
    for (const RuneLink &r : GREAT_RUNES)
    {
        // The live boss marker carrying this cleared flag → its position is the rune's location.
        const Marker *boss = nullptr;
        for (const Marker &m : bosses)
            if (m.cleared_flag == r.cleared_flag) { boss = &m; break; }
        if (!boss) { ++no_boss; continue; }  // boss absent from the live param this build (title screen)
        from::paramdef::WORLD_MAP_POINT_PARAM_ST d{};
        d.areaNo = static_cast<uint8_t>(boss->raw_area);
        d.gridXNo = static_cast<uint8_t>(boss->raw_gx);
        d.gridZNo = static_cast<uint8_t>(boss->raw_gz);
        d.posX = boss->raw_px;
        d.posZ = boss->raw_pz;
        d.textId1 = 500000000 + r.rune_id;      // GoodsName (the great rune's name)
        d.textDisableFlagId1 = r.cleared_flag;  // boss defeated → rune obtained → gray (same flag as the boss)
        push_marker(/*row_id=*/(uint64_t)r.rune_id, d, cat, /*lotId=*/0u, /*lotType=*/0u, Source::Live);
        out_cells.insert(cell_of(g_buckets[cat].back()));
        ++emitted;
    }
    spdlog::info("[LOOTDISK] world features: {} Great Runes from live boss positions ({} boss-not-live)",
                 emitted, no_boss);
    return emitted;
}

// Build every category's marker cache in ONE pass over MAP_ENTRIES (9k rows), then the WorldBosses
// bucket LIVE from the param. Same world-projection + group classification as the grace layer.
// NOTE (2026-06-23): a "World-* categories live from WorldMapPointParam by row-id range" experiment
// was REVERTED — the deployed ERR regulation's live WorldMapPointParam holds only 740 rows (217
// textId2==5100 bosses + ~523 structural iconId=83/textId=-1 nav points); the Stakes/SummoningPools/
// SpiritSprings/Maps/etc. rows the bake expects are NOT present live. So only bosses (textId2==5100)
// and graces (BonfireWarpParam) are live-portable; every other category stays baked.
// Runs exactly once — wrapped by ensure_buckets()/std::call_once.
// Item stacking: identify loot markers of the SAME item sitting within kStackRadius of each other (in
// MSB-local world space, same area) and ANNOTATE them — one representative per group carries the whole
// group's per-member collected state in `stacked`, the rest are flagged `stack_member`. This is
// NON-DESTRUCTIVE (the bucket keeps every marker) and config-INDEPENDENT, run once per build: the
// stack_identical_items toggle is then a pure RENDER decision (draw the rep with a summed " xN" and
// hide the members, or draw them all individually) — instant, no bucket rebuild, like require-fragment.
// Build-time + projection-independent, so it works UNDERGROUND (Siofra) where render-time tile
// clustering can't. Only G_LOOT-section categories (gather nodes, crafting materials, consumables) —
// never bosses/graces/NPCs (unique waypoints). Grouping is connected-components within the radius, so a
// transitive 1m chain still links a spread-out cluster (the Siofra Formic Rock nodes span ~6m but
// pairwise-bridge under 5m). See docs/plans/item_stacking_plan.md.
void annotate_item_stacks()
{
    constexpr float kStackRadius = 5.0f;            // metres in MSB-local (px,pz)
    constexpr float kR2 = kStackRadius * kStackRadius;
    int grouped_total = 0, group_count = 0;
    for (int c = 0; c < (int)g_buckets.size(); ++c)
    {
        if (goblin::ui::category_section(c) != /*G_LOOT*/ 2) continue;
        auto &bucket = g_buckets[c];
        const int n = (int)bucket.size();
        if (n < 2) continue;
        std::vector<int> parent(n);
        for (int i = 0; i < n; ++i) parent[i] = i;
        auto find = [&parent](int a) {
            while (parent[a] != a) { parent[a] = parent[parent[a]]; a = parent[a]; }
            return a;
        };
        // O(n^2) within a bucket — loot buckets are small (tens to low hundreds).
        for (int i = 0; i < n; ++i)
        {
            const Marker &a = bucket[i];
            if (a.name_id == 0) continue;                       // need a real item identity to compare
            for (int j = i + 1; j < n; ++j)
            {
                const Marker &b = bucket[j];
                if (b.name_id != a.name_id) continue;           // same item only
                if (b.raw_area != a.raw_area) continue;         // same MSB area
                // Distance in FULL area-local coords (grid*256 + pos). raw_px/raw_pz are block-local
                // (0..256 within one grid tile), so comparing them directly stacks items in DIFFERENT
                // grid tiles that happen to share a local offset — e.g. a Trina's Lily at Fort Haight
                // wrongly merging with one at Mistwood Ruins (same overworld area, different grids).
                const float ax = a.raw_gx * 256.0f + a.raw_px, az = a.raw_gz * 256.0f + a.raw_pz;
                const float bx = b.raw_gx * 256.0f + b.raw_px, bz = b.raw_gz * 256.0f + b.raw_pz;
                const float dx = bx - ax, dz = bz - az;
                if (dx * dx + dz * dz <= kR2) parent[find(i)] = find(j);
            }
        }
        // Gather members per component root, then annotate: first member = representative (records all
        // members' collected state in `stacked`); the rest are flagged `stack_member` (hidden when the
        // toggle is on). NOTHING is summed or erased — render computes the stack total from `stacked`.
        std::unordered_map<int, std::vector<int>> comps;
        for (int i = 0; i < n; ++i) comps[find(i)].push_back(i);
        for (auto &kv : comps)
        {
            const std::vector<int> &members = kv.second;
            if (members.size() < 2) continue;                   // a lone marker isn't a stack
            Marker &rep = bucket[members[0]];
            rep.stacked.clear();
            for (int mi : members)
            {
                rep.stacked.push_back({bucket[mi].row_id, bucket[mi].collected_flag, bucket[mi].count});
                if (mi != members[0]) bucket[mi].stack_member = true;
            }
            ++group_count;
            grouped_total += (int)members.size();
        }
    }
    if (group_count)
        spdlog::info("[ITEMSTACK] {} co-located identical-item markers in {} groups (r={}m)",
                     grouped_total, group_count, (int)kStackRadius);
}

// Off-page altitude reference: for every g_buckets marker (loot/bosses/…), record the block-local Y of
// the NEAREST grace in its SAME area (live BonfireWarpParam). The altitude badge uses this when the
// marker is on a page the player isn't on — there the player's Y is in a different frame, but a grace
// in the marker's own area shares its block-local frame. Same-area only; areas with no grace get no
// off-page badge. XZ distance uses full area-local coords (grid*256 + pos). Run once at build, after
// capture_live_graces (dllmain orders it before the build). See offpage_altitude_via_grace_plan.md.
void assign_grace_altitude_refs()
{
    const auto &graces = goblin::live_graces();
    if (graces.empty()) return;
    struct G { float x, z, y; };
    std::unordered_map<int, std::vector<G>> by_area;   // areaNo → graces (area-local x/z, block-local y)
    for (const auto &g : graces)
        by_area[(int)g.areaNo].push_back({g.gridXNo * 256.0f + g.posX,
                                          g.gridZNo * 256.0f + g.posZ, g.posY});
    int assigned = 0;
    for (auto &bucket : g_buckets)
        for (Marker &m : bucket)
        {
            if (m.raw_area < 0) continue;                       // no live area → no reference
            auto it = by_area.find(m.raw_area);
            if (it == by_area.end()) continue;                  // no grace in this area
            const float mx = m.raw_gx * 256.0f + m.raw_px, mz = m.raw_gz * 256.0f + m.raw_pz;
            float best = 1e30f, bestY = 0.0f;
            for (const G &g : it->second)
            {
                const float dx = g.x - mx, dz = g.z - mz;
                const float d2 = dx * dx + dz * dz;
                if (d2 < best) { best = d2; bestY = g.y; }
            }
            m.ref_grace_y = bestY;
            m.has_ref_grace = true;
            ++assigned;
        }
    if (assigned)
        spdlog::info("[ALT-REF] {} markers referenced to a same-area grace (off-page altitude)", assigned);
}

void build_buckets_impl()
{
    GOBLIN_BENCH("build.buckets");
    namespace gen = goblin::generated;
    g_skip = SkipTally{};  // reset the [SKIPPED] tally for this build

    // Disk-MSB loot source (opt-in): build the loot markers from the mod's real
    // files first + collect which item-lot ids they place, so the baked rows
    // those lots cover are dropped below (the disk owns the treasure slice;
    // EMEVD-granted + enemy lots have no MSB part → they stay baked).
    std::unordered_set<uint32_t> disk_lots;
    std::unordered_map<uint32_t, uint32_t> disk_lot_tile;  // lotId → packed tile (diag)
    std::vector<uint32_t> dropped_dummy_lots;              // DummyAsset lots we dropped
    // Enemy-drop lots the disk enemy pass placed (lootEnemyDrops). Kept SEPARATE from
    // disk_lots: baked enemy rows are lotType 2, dropped by their own provenance guard
    // below — never via the lotType-1 treasure-replace path.
    std::unordered_set<uint32_t> enemy_disk_lots;
    // [ENEMY-MARKERS] diag (lootFromDiskMsb): every parsed enemy's raw itemLotId_enemy — lets the
    // uncovered-baked-Enemy triage split "parsed but uncovered" (recoverable) from "not parsed".
    std::unordered_set<uint32_t> parsed_enemy_lots;  // _00 enemies' raw itemLotId_enemy (0x30) — for the split diag
    // EMEVD-scripted award lots the disk EMEVD pass placed (lootEmevdDrops). Like the
    // enemy guard, kept SEPARATE: baked Emevd rows are dropped by their own provenance
    // guard below (LootSource::Emevd), never via the lotType-1 treasure-replace.
    std::unordered_set<uint32_t> emevd_disk_lots;
    // Projected cells of the disk COLLECTIBLE placements (gather assets) — the finalize
    // geom dedup drops a baked Material/Rune/Ember marker that sits on one of these. Only
    // the collectible pass re-emits gather assets, so keying on it (not all disk markers)
    // stops an unrelated treasure/enemy coincidence from evicting a baked piece.
    std::unordered_set<Cell, CellHash> collectible_cells;
    // Projected cells of the disk-placed World features, keyed by category — the finalize
    // pass uses them to drop the baked twins (category-wipe for dedicated categories like
    // Stakes/Imp; cell-dedup for shared ones like Hero's Tomb in WorldInteractables).
    // worldFeaturesFromDisk. See build_disk_world_feature_markers.
    std::unordered_map<int, std::unordered_set<Cell, CellHash>> world_feature_cells;
    // (tile, object_name) of every disk-placed Rune/Ember Piece — drops the baked twin by IDENTITY
    // (position-independent; see piece_key). Built by the collectible pass, consumed in the baked loop.
    std::unordered_set<std::string> piece_disk_keys;
    // (tile, object_name) of every disk-placed gather node — drops the baked Material Node twin by
    // IDENTITY when the positional cell-dedup misses it (baked pos offset >0.5u from the live MSB).
    std::unordered_set<std::string> gather_disk_keys;
    // Kindling Spirits (Phase 2, no-bake): emitted as markers DIRECTLY from the disk SFX regions
    // ("KindlingSpirit_000N") — no longer sourced from the static bake. Each marker's row_id = the
    // constant entity-id slot key (goblin::kindling::region_row_id), which goblin_kindling's slot
    // table (built from the same constants) uses for graying. This set holds the region names emitted
    // so the baked loop drops the baked twin; a baked row with no disk twin re-keys to the same
    // entity-id row_id, keeping graying correct when the disk pass is off / a region is missing.
    std::unordered_set<std::string> kindling_disk_names;
    // Cross-tile LOD treasures (lot → disk position): a few MSB Treasures live ONLY in an overworld
    // LOD supertile (bound to a "m{AA}_{BB}_{CC}_00-AEG…" cross-tile part), which the _00-only
    // load_disk_treasures misses. Most are covered as a contiguous-sibling of a _00 base; the few past
    // a chain gap stay baked residual. The baked loop RE-SOURCES such a residual row's position from
    // here + flips source→DiskMSB (kindling pattern: no new marker → sibling-covered lots untouched).
    std::unordered_map<uint32_t, DiskTreasure> lod_treasure_pos;
    if (disk_source_enabled())
    {
        // One disk read pass for all sources. Each out-vector requested only when on.
        // The EMEVD pass needs the enemy placements too (for the EntityID → position
        // join), so parse enemies whenever the enemy OR emevd source is on.
        std::vector<DiskCollectible> disk_collectibles;
        std::vector<DiskEnemy> disk_enemies;
        std::vector<DiskRegion> disk_regions;
        // QuestNpcLayer resolves its pin position via entity_world_pos(), which reads the
        // g_entity_pos cache built below from these SAME disk_enemies/disk_collectibles
        // vectors — so the enemy/asset enumeration must run whenever that layer is active,
        // independent of the unrelated loot toggles (otherwise quest pins would silently
        // stop resolving whenever the user has all 3 loot sources off).
        const bool wantQuestNpcs = goblin::config::showCategory[static_cast<int>(gen::Category::WorldQuestNPC)];
        // World features need enemies too (Hostile NPC invaders ride the enemy enumeration).
        const bool wantEnemies = goblin::config::lootEnemyDrops || goblin::config::lootEmevdDrops ||
                                 goblin::config::worldFeaturesFromDisk || wantQuestNpcs;
        // World features (Stakes) are AEG asset placements → they ride the SAME asset
        // enumeration as collectibles (disk_collectibles), so request it when either is on.
        // The EMEVD pass also needs asset positions (asset-anchored awards: Blaidd's spirit ash,
        // the scarab-cluster runes) → request it when emevd drops are on too.
        const bool wantAssets = goblin::config::lootCollectibles || goblin::config::worldFeaturesFromDisk ||
                                goblin::config::lootEmevdDrops || wantQuestNpcs;
        // Spirit Springs are POINT regions → request the region enumeration for world features.
        const bool wantRegions = goblin::config::worldFeaturesFromDisk;
        std::vector<DiskTreasure> treasures = load_disk_treasures(
            &dropped_dummy_lots,
            wantAssets ? &disk_collectibles : nullptr,
            wantEnemies ? &disk_enemies : nullptr,
            wantRegions ? &disk_regions : nullptr);
        if (goblin::config::lootFromDiskMsb)
            build_disk_loot_markers(treasures, disk_lots, disk_lot_tile);
        // Cross-tile LOD treasures (non-_00 supertiles), indexed by lot for the baked-residual
        // re-source below. Only lots NOT covered by the _00 pass/sibling-walk (∉ disk_lots) are
        // kept — a covered lot is already drawn, so re-sourcing it would double-place. First
        // occurrence wins (one position per lot). Only when the treasure source is on.
        if (goblin::config::lootFromDiskMsb)
            for (DiskTreasure &t : load_lod_treasures())
                if (!disk_lots.count(t.lotId))
                    lod_treasure_pos.emplace(t.lotId, t);
        // All MSB Treasure lots (the ground-item pickup assets) — used to skip
        // double-placing in the collectible/enemy/emevd passes. Built from the parsed
        // treasures directly, so it's correct even when loot_from_disk_msb is off.
        std::unordered_set<uint32_t> treasure_lots;
        if (goblin::config::lootCollectibles || wantEnemies)
            for (const DiskTreasure &t : treasures) treasure_lots.insert(t.lotId);
        if (goblin::config::lootCollectibles)
            build_disk_collectible_markers(disk_collectibles, treasure_lots, disk_lots,
                                           collectible_cells, piece_disk_keys, gather_disk_keys);
        // Runtime quest-NPC table (mod-agnostic EMEVD 90005702 mine): join to the hand-authored
        // QUEST_BROWSER by concluded==fail_flag. Runtime NPCs with NO hand entry (modded / not yet
        // authored) become minimal FALLBACK browser rows (name + live state, no step prose) — the
        // quest analogue of the circle fallback. One-time on this disk worker.
        if (wantQuestNpcs)
        {
            std::vector<QuestNpcRuntime> qnpcs = load_quest_npcs();
            // Flag-coverage (the only join possible here — NAMES are resolved at render, since
            // npc_team_and_name reads live NpcParam which isn't ready on this early worker).
            std::unordered_set<uint32_t> handFail;
            for (size_t qi = 0; qi < gen::QUEST_BROWSER_COUNT; ++qi)
                if (gen::QUEST_BROWSER[qi].fail_flag) handFail.insert(gen::QUEST_BROWSER[qi].fail_flag);
            // entity → npcParamId from THIS pass's disk enemies (wantEnemies ⊇ wantQuestNpcs).
            std::unordered_map<uint32_t, uint32_t> ent2param;
            for (const DiskEnemy &en : disk_enemies)
                if (en.entityId) ent2param.emplace(en.entityId, en.npcParamId);
            // Candidates = runtime NPCs NOT covered by a hand fail_flag, with a valid _q99 and a
            // named-enemy placement (npcParamId) so the render side can resolve a name. Name +
            // secondary name-coverage + the [concluded] state happen in the Quest Browser.
            std::vector<QuestFallbackNpc> cand;
            int covered = 0, filtered = 0;
            for (const QuestNpcRuntime &q : qnpcs)
            {
                if (q.concluded == 0 || q.concluded >= 100000u) { ++filtered; continue; } // 10-digit = entity-death variant
                if (handFail.count(q.concluded)) { ++covered; continue; }                 // hand entry handles it
                QuestFallbackNpc n;
                for (uint32_t e : q.entities)
                {
                    auto it = ent2param.find(e);
                    if (it == ent2param.end() || !it->second) continue;
                    if (!n.pinEntity) n.pinEntity = e;  // first placeable entity → map pin
                    if (std::find(n.npcParamIds.begin(), n.npcParamIds.end(), it->second) == n.npcParamIds.end())
                        n.npcParamIds.push_back(it->second);
                }
                if (n.npcParamIds.empty()) { ++filtered; continue; }  // no enemy placement → can't name it
                n.concluded = q.concluded; n.regLo = q.regLo; n.regHi = q.regHi;
                cand.push_back(std::move(n));
            }
            spdlog::info("[QUESTNPC] runtime: {} NPCs → {} fallback-candidates, {} flag-covered, {} filtered "
                         "(names + name-coverage resolved at render)",
                         (int)qnpcs.size(), (int)cand.size(), covered, filtered);
            set_quest_fallback_npcs(std::move(cand));
        }
        if (goblin::config::worldFeaturesFromDisk)
        {
            // EMEVD-sourced graying flags (entity → flag) for Hero's Tomb (template 90005683)
            // AND Hostile NPC defeat (90005792) — one event-dir scan feeds both passes. The
            // SAME scan also harvests painting collection events (entity → flag 580000-580199)
            // into `painting_flags`, so the painting pass needs no second event-dir read.
            std::unordered_map<uint32_t, uint32_t> painting_flags;
            std::vector<msbe::GestureRef> gesture_refs;
            std::unordered_map<uint32_t, uint32_t> world_feature_flags =
                load_emevd_world_feature_flags(&painting_flags, &gesture_refs);
            // LOD-only asset features: a few World-feature models (the Snow Town seal-release statues
            // AEG110_029) are placed ONLY in non-_00 LOD supertiles, which the _00 asset enumeration in
            // load_disk_treasures skips. Scan those models out of the non-_00 tiles and append to
            // disk_collectibles BEFORE the world-feature pass — AFTER build_disk_collectible_markers
            // (above), so they never count as loot collectibles. The wanted set = the editorial rows
            // flagged lod_scan (tools/world_feature_assets.py). The asset analogue of the emevd
            // load_lod_award_entities recovery; the other world-feature passes filter by their own model
            // so these added assets are inert to them.
            std::unordered_set<uint32_t> lod_feature_models;
            for (size_t k = 0; k < gen::WORLD_FEATURE_MODEL_COUNT; ++k)
                if (gen::WORLD_FEATURE_MODELS[k].lod_scan)
                    lod_feature_models.insert(gen::WORLD_FEATURE_MODELS[k].aeg_row);
            for (DiskCollectible &c : load_lod_feature_assets(lod_feature_models))
                disk_collectibles.push_back(std::move(c));
            build_disk_world_feature_markers(disk_collectibles, world_feature_flags, world_feature_cells);
            // Summoning Pools: bespoke live-SignPuddleParam pass (param feature, not an asset
            // model). Rides the same category-wipe finalize via world_feature_cells.
            build_live_summoning_pools(
                disk_collectibles,
                world_feature_cells[static_cast<int>(gen::Category::WorldSummoningPools)]);
            // Hostile NPC invaders: disk enemy placements filtered by live NpcParam (named
            // invader = teamType ∈ {24,27} ∧ nameId>0), defeat flag from EMEVD 90005792.
            build_disk_hostile_npc_markers(
                disk_enemies, world_feature_flags,
                world_feature_cells[static_cast<int>(gen::Category::WorldHostileNPC)]);
            // Spirit Springs: POINT regions (MountJump/Locked/FakeSpiring) + AEG463_200 assets.
            build_disk_spirit_springs_markers(
                disk_regions, disk_collectibles,
                world_feature_cells[static_cast<int>(gen::Category::WorldSpiritSprings)]);
            // Kindling Spirits (Phase 2, no-bake): emit each disk SFX region ("KindlingSpirit_000N")
            // as its own marker. row_id = the constant entity-id slot key so goblin_kindling grays it
            // (its slot table is built from the same constants, decoupled from MAP_ENTRIES). The baked
            // WorldKindlingSpirits twin is dropped in the baked loop below.
            for (const DiskRegion &r : disk_regions)
            {
                if (r.name.rfind("KindlingSpirit_", 0) != 0) continue;
                uint64_t rid = goblin::kindling::region_row_id(r.name.c_str());
                if (!rid) continue;
                from::paramdef::WORLD_MAP_POINT_PARAM_ST d{};
                d.areaNo = r.area;
                d.gridXNo = r.gx;
                d.gridZNo = r.gz;
                d.posX = r.posX;
                d.posZ = r.posZ;
                d.posY = r.posY;  // DX item 7: altitude badge
                // Label = "Kindling Spirit", the awarded incantation's GoodsName (Goods 6610 + the
                // 500M GoodsName offset), exactly what the bake's WorldMapPointParam row used. Without
                // it the marker is anonymous → empty tooltip + the F1 item-search can't match it.
                d.textId1 = 500006610;  // 6610 (Goods "Kindling Spirit") + GOODS_NAME_OFFSET
                push_marker(rid, d, static_cast<int>(gen::Category::WorldKindlingSpirits),
                            /*lotId=*/0u, /*lotType=*/0u, Source::DiskMSB);
                kindling_disk_names.insert(r.name);
            }
            // Spiritspring Hawks: c4210 disk enemies, EntityID suffix 980/971, flag = EntityID.
            build_disk_spiritspring_hawks_markers(
                disk_enemies,
                world_feature_cells[static_cast<int>(gen::Category::WorldSpiritspringHawks)]);
            // Maps: region map fragments (goods sortGroupId 190/191) placed as MSB treasures.
            build_disk_maps_markers(
                treasures, world_feature_cells[static_cast<int>(gen::Category::WorldMaps)]);
            // Paintings: MSB parts (Asset AEG099_38x/39x or ghost-painter Enemy c4300/c6320)
            // referenced by an EMEVD painting event (flag 580000-580199). entity→flag from the
            // painting EMEVD scan; position from the matching disk asset/enemy part.
            build_disk_painting_markers(
                disk_collectibles, disk_enemies, painting_flags,
                world_feature_cells[static_cast<int>(gen::Category::WorldPaintings)]);
            // Gestures: MSB Asset referenced by a gesture-spawn EMEVD event (template 90005570);
            // name from GestureParam.itemId (live), position from the disk asset.
            build_disk_gesture_markers(
                disk_collectibles, gesture_refs,
                world_feature_cells[static_cast<int>(gen::Category::LootGestures)]);
        }
        if (goblin::config::lootEnemyDrops)
            build_disk_enemy_markers(disk_enemies, treasure_lots, enemy_disk_lots,
                                     goblin::config::lootFromDiskMsb ? &parsed_enemy_lots : nullptr);
        if (goblin::config::lootEmevdDrops)
        {
            // The event-1200 (mechanism B) join needs the MSB enemy EntityIDs to resolve
            // which boss entity a setter event references — both as a global set (the
            // flag==EntityID boss-reward shortcut) AND grouped per tile (the setter→boss
            // candidate resolution must stay map-scoped, see load_emevd_awards).
            std::unordered_set<uint32_t> known_entities;
            std::unordered_map<uint32_t, std::unordered_set<uint32_t>> entities_by_tile;
            known_entities.reserve(disk_enemies.size());
            for (const DiskEnemy &en : disk_enemies)
                if (en.entityId)
                {
                    known_entities.insert(en.entityId);
                    uint32_t tile = ((uint32_t)en.area << 16) | ((uint32_t)en.gx << 8) | en.gz;
                    entities_by_tile[tile].insert(en.entityId);
                }
            std::vector<DiskEmevd> awards = load_emevd_awards(known_entities, entities_by_tile);
            // Cause-B recovery (LOD-scope): a DIRECT template award whose entity is NOT a _00
            // enemy lives only as an overworld LOD proxy in a non-_00 tile (e.g. lot 2045460500's
            // c5170 in m61_11_11_02). The _00-only enemy scan missed it → the award never placed
            // (entity-not-an-msb-enemy) → the baked Emevd row stayed residual. Scan the non-_00
            // tiles for JUST those missing entities and append them to disk_enemies — consumed ONLY
            // by the emevd join below (the enemy pass + known_entities/boss resolution are already
            // built, so this adds no LOD phantoms there).
            // Includes bossReward awards: the explicit bossEntity@16 of an overworld field-boss bind
            // can live ONLY in a LOD supertile (the cross-tile c4503 entity 1054560800 in m60_13_14_02,
            // bind in m60_54_56) — the _00 scan misses it, so it's recovered here exactly like a direct
            // LOD-scope award. The 8 other entity@16 binds resolve in _00 and never reach this set.
            std::unordered_set<uint32_t> missing_award_entities;
            for (const DiskEmevd &a : awards)
                if (a.lotType == 1 && a.entityId && !known_entities.count(a.entityId))
                    missing_award_entities.insert(a.entityId);
            for (DiskEnemy &e : load_lod_award_entities(missing_award_entities))
                disk_enemies.push_back(std::move(e));
            // entity_world_pos() cache for QuestNpcLayer. Enemy entities first, then
            // collectible/asset entities for any entityId not already an enemy (matches
            // the documented enemy->asset precedence) -- same disk_enemies/disk_collectibles
            // this pass already built, no extra parse.
            for (const auto &en : disk_enemies)
            {
                if (!en.entityId) continue;
                int ga = 0; float wx = 0.0f, wz = 0.0f;
                if (!goblin::marker_world_pos(en.area, en.gx, en.gz, en.posX, en.posZ, ga, wx, wz)) continue;
                g_entity_pos[en.entityId] = {wx, wz, goblin::marker_group_from(en.area, ga)};
            }
            for (const auto &as : disk_collectibles)
            {
                if (!as.entityId || g_entity_pos.count(as.entityId)) continue;
                int ga = 0; float wx = 0.0f, wz = 0.0f;
                if (!goblin::marker_world_pos(as.area, as.gx, as.gz, as.posX, as.posZ, ga, wx, wz)) continue;
                g_entity_pos[as.entityId] = {wx, wz, goblin::marker_group_from(as.area, ga)};
            }

            build_disk_emevd_markers(awards, disk_enemies, disk_collectibles, treasure_lots,
                                     emevd_disk_lots, piece_disk_keys,
                                     goblin::config::worldFeaturesFromDisk
                                         ? &world_feature_cells[static_cast<int>(gen::Category::WorldMaps)]
                                         : nullptr);
        }
    }

    // Diagnostic sets: which baked lotIds exist as map-loot (lotType 1) vs as ANY
    // lot (1 or 2). Lets us explain the disk-only lots below (in the disk but not
    // the bake's treasure slice): new MSB loot the bake missed, or loot the bake
    // filed as an enemy drop (lotType 2). Only built when the disk source is on.
    const bool diag = goblin::config::lootFromDiskMsb;
    // Per-lot enumeration (disk-only + RECOVER-LATER lines) is verbose; gate it
    // behind diag_loot_pos so normal feature use only emits the summary totals.
    const bool verbose = goblin::config::diagLootPos;
    std::unordered_set<uint32_t> baked_lot1, baked_any;

    int replaced = 0;
    int replaced_piece = 0;  // baked Rune/Ember Pieces dropped because the disk pass placed them
    int replaced_matnode = 0; // baked Material Nodes dropped by IDENTITY (gather offset near-miss)
    int replaced_kindling = 0; // baked Kindling twins dropped because the disk SFX-region pass placed them
    int lod_treasure_pos_count = 0; // baked treasure markers re-sourced to a cross-tile LOD position
    int replaced_enemy = 0;  // baked Enemy rows dropped because the disk enemy pass covers them
    int replaced_emevd = 0;  // baked Emevd rows dropped because the disk EMEVD pass covers them
    int dropped_merchant = 0; // baked loot rows dropped as merchant phantoms (shop-infinite item, no disk twin)
    int debake_gap = 0;  // Treasure-sourced baked rows the disk did NOT cover (de-bake blocker)
    // [DEBAKE-GAP] cause classification (diag): the baked Treasure rows carry NO object_name
    // (nullptr — corpse-asset identity lives only in the offline pipeline), so we sample the
    // CAUSES at runtime via four cheap cuts: (1) sequence-sibling = disk_lots holds a base just
    // below this lot → a multi-lot/ItemLotParam-chain treasure the parser under-read (a CLEAN
    // lever if it dominates); (2) baked-as-enemy = the bake also filed this lot as a lotType-2
    // drop (mis-tagged source); (3) per-category and (4) per-area histograms (corpse loot is
    // ~65% m60 overworld per the offline data-mine). If none dominates → diffuse corpse residual.
    int gap_sibling = 0;        // disk-covered base at lotId-1..lotId-N (multi-lot candidate)
    int gap_baked_as_enemy = 0; // lot also present in the bake as a lotType-2 enemy drop
    std::map<int, int> gap_by_cat;   // category enum -> uncovered count
    std::map<int, int> gap_by_area;  // areaNo (m<area>) -> uncovered count
    int enemy_markers = 0;  // [ENEMY-MARKERS] baked enemy-drop rows NOT covered by the disk pass
    int enemy_parsed_uncov = 0;  // …of those: enemy WAS parsed (lot in parsed_enemy_lots) → recoverable
    int enemy_not_parsed = 0;    // …enemy NOT parsed (MSB scope miss: dungeon / non-_00 tile / filtered out)
    std::map<int, int> enemy_uncov_by_area;  // areaNo histogram of the uncovered enemy slice
    // [RESIDUAL-SRC] full triage of the surviving baked-loot residual by provenance: every baked
    // row that REACHES push_marker (not replaced by any disk pass) is tallied as cat*4+loot_source.
    // The DEBAKE-GAP (Treasure) + ENEMY-MARKERS (Enemy) diags only cover 2 of the 4 sources; this
    // closes the gap (Emevd + Unknown) so a glance shows which lever recovers each leftover category.
    std::map<int, int> resid_by_cat_src;  // key = category*4 + (int)loot_source
    // Pre-pass: fully populate baked_lot1/baked_any BEFORE the main loop so the DEBAKE-GAP
    // baked-as-enemy classification (and the disk-only summary below) see the COMPLETE baked
    // lot sets — building them incrementally inside the loop would leave them partial mid-row.
    if (diag)
        for (size_t i = 0; i < gen::MAP_ENTRY_COUNT; ++i)
        {
            const gen::MapEntry &e = gen::MAP_ENTRIES[i];
            if (e.lotId == 0) continue;
            if (e.lotType == 1) baked_lot1.insert(e.lotId);
            if (e.lotType != 0) baked_any.insert(e.lotId);
        }
    // Merchant-phantom drop set (config drop_merchant_phantoms): items sold infinite-stock in the
    // live ShopLineupParam. The bake's unmatched-ItemLotParam fallback (extract_all_items.py) places
    // a phantom marker at the tile corner (0,0,0) for items with NO world placement that ARE sold by
    // a merchant (ERR's shop sells most weapons / gloveworts / etc.). Built once; a baked loot row
    // whose resolved item key is in here AND reached this loop (no disk pass reproduced it) is dropped.
    const std::unordered_set<int32_t> shop_inf_keys =
        goblin::config::dropMerchantPhantoms ? build_shop_infinite_keys()
                                             : std::unordered_set<int32_t>{};
    if (goblin::config::dropMerchantPhantoms)
        spdlog::info("[LOOTDISK] merchant-phantom drop: {} infinite-stock shop item keys (live ShopLineupParam)",
                     (int)shop_inf_keys.size());
    for (size_t i = 0; i < gen::MAP_ENTRY_COUNT; ++i)
    {
        const gen::MapEntry &e = gen::MAP_ENTRIES[i];
        int c = static_cast<int>(e.category);
        if (c < 0 || c >= NUM_CAT)
            continue;
        // Bosses come LIVE from the param now (build_live_bosses) — skip any baked WorldBosses
        // rows so they don't double the live ones (the bake is being retired for this category).
        if (e.category == gen::Category::WorldBosses)
            continue;
        // Quest NPCs: QuestNpcLayer (quest_npc_layer.cpp) is the sole producer of
        // WorldQuestNPC markers now (entity_id + live EMEVD flag driven, not this bake) —
        // drop any baked rows here so it doesn't double-draw, same as WorldBosses above.
        if (e.category == gen::Category::WorldQuestNPC)
            continue;
        // Rune/Ember Piece IDENTITY dedup: the disk collectible pass placed this exact AEG world
        // piece (matched by tile + part name, NOT position — robust to the m11_10 offset anomaly).
        // Drop the baked twin; the disk marker carries the real MSB position + runtime geom graying.
        // Boss-flag pieces (c-model object_names) are absent from piece_disk_keys → correctly kept.
        if (!piece_disk_keys.empty() &&
            (e.category == gen::Category::ReforgedRunePieces ||
             e.category == gen::Category::ReforgedEmberPieces) &&
            ((e.object_name &&
              piece_disk_keys.count(piece_key(e.data.areaNo, e.data.gridXNo, e.data.gridZNo, e.object_name))) ||
             piece_disk_keys.count(piece_key(e.data.areaNo, e.data.gridXNo, e.data.gridZNo,
                 e.category == gen::Category::ReforgedRunePieces ? "__BOSSRUNE__" : "__BOSSEMBER__"))))
        {
            ++replaced_piece;
            continue;
        }
        // Material Node IDENTITY dedup: same idea as the pieces above, and now the PRIMARY dedup
        // for this category. The disk collectible pass re-places every AEG gather node; matching
        // by (tile + MSB part name) catches ALL of them (runtime: 1455/1455) — strictly better
        // than the finalize positional cell-dedup, which left 11 offset near-misses (baked pos
        // >0.5u off the live MSB pos) as baked-only residual. Safe by construction (only matches an
        // object the gather pass PROVABLY emitted; the disk marker owns the real position + the
        // item's real per-item category). The finalize positional pass stays as a fallback for any
        // gather node whose MSB part name is empty (→ not in gather_disk_keys).
        if (!gather_disk_keys.empty() && e.object_name &&
            e.category == gen::Category::LootMaterialNodes &&
            gather_disk_keys.count(piece_key(e.data.areaNo, e.data.gridXNo, e.data.gridZNo, e.object_name)))
        {
            ++replaced_matnode;
            continue;
        }
        // Kindling Spirits (Phase 2): the markers come from the disk SFX regions directly (emitted
        // above). Drop the baked twin when the disk pass placed this spirit. If the disk pass is off
        // or this region was missing, keep the baked marker but RE-KEY its row_id to the same constant
        // entity-id slot (goblin::kindling::region_row_id) so graying still matches goblin_kindling's
        // slot table (which is no longer MAP_ENTRIES-derived). Fully decouples the category from the bake.
        if (e.category == gen::Category::WorldKindlingSpirits)
        {
            if (e.object_name && kindling_disk_names.count(e.object_name))
            {
                ++replaced_kindling;
                continue;  // disk twin owns it
            }
            uint64_t rid = e.object_name ? goblin::kindling::region_row_id(e.object_name) : 0;
            if (rid)
            {
                push_marker(rid, e.data, c, /*lotId=*/0u, /*lotType=*/0u, Source::Baked);
                continue;
            }
            // unparseable object_name (shouldn't happen) → fall through to generic handling
        }
        // Disk loot owns this lot → drop the baked placement (lotId-coverage replace).
        // Only map-loot lots (lotType 1); enemy drops (lotType 2) are untouched.
        // PROVENANCE GUARD: only drop a baked row the disk can legitimately reproduce — a
        // Treasure-sourced placement. Enemy/Emevd rows have no MSB Treasure (the disk source
        // never derives them), so a lotId collision with a disk treasure must NOT evict them.
        // Unknown = a pre-provenance bake (field not yet regenerated) → keep the prior
        // behaviour (treat as replaceable) so this change is inert until the data is rebuilt.
        const bool disk_replaceable = e.loot_source == gen::LootSource::Treasure ||
                                      e.loot_source == gen::LootSource::Unknown;
        if (!disk_lots.empty() && e.lotType == 1 && e.lotId != 0 && disk_replaceable &&
            disk_lots.count(e.lotId))
        {
            ++replaced;
            continue;
        }
        // Enemy-drop disk source (loot_enemy_drops) owns this lot → drop the baked
        // LootSource::Enemy row (lotType 2). Its own guard, separate from the lotType-1
        // treasure-replace: the live Parts.Enemies + NpcParam pass reproduces this drop.
        if (!enemy_disk_lots.empty() && e.loot_source == gen::LootSource::Enemy &&
            e.lotId != 0 && enemy_disk_lots.count(e.lotId))
        {
            ++replaced_enemy;
            continue;
        }
        // EMEVD-scripted disk source (loot_emevd_drops) owns this lot → drop the baked row the
        // EMEVD pass reproduced (live event\*.emevd.dcx template/per-tile-enemy award + EntityID→
        // MSB-Enemy join). Drops both LootSource::Emevd AND Unknown rows: the per-tile enemy-death
        // awards (the uncovered Golden Runes etc.) are baked as src=Unknown (pre-provenance), and
        // Unknown is replaceable by design (same as the treasure guard above) — a globally-unique
        // lotId the EMEVD pass placed is the SAME pickup, so the baked twin must go (else a double).
        if (!emevd_disk_lots.empty() &&
            (e.loot_source == gen::LootSource::Emevd || e.loot_source == gen::LootSource::Unknown) &&
            e.lotId != 0 && emevd_disk_lots.count(e.lotId))
        {
            ++replaced_emevd;
            continue;
        }
        // An EMEVD-baked lot can ALSO be an MSB Treasure — the chest/ground item IS the scripted
        // award (e.g. the m31_8 chest chain 31080700+, the m61 scarab+Crystal-Tear pairs, the
        // Reforged m60_36_49 chest 1036490010..014). The emevd pass deliberately SKIPS such a lot
        // (treasure_dup → not in emevd_disk_lots) because the treasure pass already drew it (it's in
        // disk_lots, base + its sequence-sibling chain). But the PROVENANCE GUARD above won't evict an
        // Emevd-sourced row via disk_lots, so the baked Emevd twin survived as a residual + a DOUBLE
        // marker. ItemLotParam_map ids are GLOBALLY UNIQUE, so a disk treasure carrying this exact lot
        // is the SAME single pickup — drop the redundant baked Emevd twin. (lotType-1 only: an
        // ItemLotParam_enemy collision would be a different table / different drop.)
        if (!disk_lots.empty() && e.loot_source == gen::LootSource::Emevd &&
            e.lotType == 1 && e.lotId != 0 && disk_lots.count(e.lotId))
        {
            ++replaced_emevd;
            continue;
        }
        // Merchant-phantom drop (config drop_merchant_phantoms): this baked loot row reached here =
        // NO disk pass (treasure / asset / enemy / emevd) reproduced it. If its item is sold
        // infinite-stock by a live merchant, it is the bake's unmatched-ItemLotParam fallback
        // phantom — an item with no world placement that the bake put at the tile corner (0,0,0), so
        // the player flies to an empty spot (proven: tools/_probe_find_item + _probe_resid_shop;
        // the 16 treasure-fallback + the orphan-enemy survivors are all shop-infinite, 0 world
        // placement). Drop it. Gated on a live shop key (any mod); the resolved key matches the
        // marker's own item key (resolve_loot_item_textid). lotId != 0 keeps World features safe.
        if (!shop_inf_keys.empty() && e.lotId != 0)
        {
            int32_t mkey = goblin::resolve_loot_item_textid(e.lotId, e.lotType, e.data.textId1);
            if (mkey != 0 && shop_inf_keys.count(mkey))
            {
                ++dropped_merchant;
                if (verbose)
                    spdlog::info("[MERCHANT-PHANTOM] drop cat=\"{}\" lot={} lt={} key={} m{}_{}_{}",
                                 goblin::markers::category_name(static_cast<gen::Category>(c)),
                                 e.lotId, (int)e.lotType, mkey, e.data.areaNo, e.data.gridXNo,
                                 e.data.gridZNo);
                continue;
            }
        }
        // Cross-tile LOD treasure RE-SOURCE: this baked row survived every disk-coverage drop above
        // (its lot ∉ disk_lots — the contiguous-sibling walk didn't reach it past a chain gap), but
        // the lot IS an MSB Treasure that lives only in an overworld LOD supertile (bound to a
        // "m{AA}_{BB}_{CC}_00-AEG…" cross-tile part), which the _00-only load_disk_treasures missed.
        // Read its real position from that LOD part and tag the marker DiskMSB (kindling pattern: keep
        // the baked row_id/lot/category, just disk-source the position — no new marker, so the
        // sibling-covered LOD lots, dropped above, are never doubled). lotType-1 (map) only.
        if (!lod_treasure_pos.empty() && e.lotType == 1 && e.lotId != 0)
        {
            auto lit = lod_treasure_pos.find(e.lotId);
            if (lit != lod_treasure_pos.end())
            {
                from::paramdef::WORLD_MAP_POINT_PARAM_ST d = e.data;
                d.areaNo = lit->second.area;
                d.gridXNo = lit->second.gx;
                d.gridZNo = lit->second.gz;
                d.posX = lit->second.posX;
                d.posZ = lit->second.posZ;
                push_marker(e.row_id, d, c, e.lotId, e.lotType, Source::DiskMSB);
                ++lod_treasure_pos_count;
                continue;
            }
        }
        // [DEBAKE-GAP] diag (de-bake readiness): a Treasure-sourced row that reaches HERE was
        // NOT replaced — its lot isn't in disk_lots, so the disk source does not reproduce it.
        // These are exactly the rows that would be LOST if the bake's Treasure slice is dropped
        // (#11 final switch): multi-lot treasures (parser reads one itemLotId), DummyAsset edge
        // cases, or rows mis-tagged 'treasure' that are really EMEVD-positioned. Characterize
        // this list (drive it to zero or explicitly accept it) BEFORE de-baking. Gated on diag
        // (lootFromDiskMsb, so disk_lots is populated); per-row detail under diag_loot_pos.
        if (diag && e.loot_source == gen::LootSource::Treasure && e.lotType == 1 && e.lotId != 0)
        {
            ++debake_gap;
            // Sequence-sibling probe: is a disk-covered base just below this lot? ItemLotParam
            // treasure chains run base..base+k (e.g. smithing-stone 200X0..200X9); if the MSB
            // Treasure carried only the base, the bake's expanded siblings reach here while the
            // base sits in disk_lots. A wide window (16) covers the long stone/rune sequences.
            bool sib = false;
            for (uint32_t d = 1; d <= 16 && !sib; ++d)
                if (e.lotId > d && disk_lots.count(e.lotId - d)) sib = true;
            if (sib) ++gap_sibling;
            const bool as_enemy = baked_any.count(e.lotId) && !baked_lot1.count(e.lotId);
            if (as_enemy) ++gap_baked_as_enemy;
            ++gap_by_cat[c];
            ++gap_by_area[e.data.areaNo];
            if (verbose)
            {
                int32_t key = goblin::resolve_loot_item_textid(e.lotId, 1, -1);
                spdlog::info("[DEBAKE-GAP] uncovered Treasure lot {} @ m{}_{}_{} key={} cat={}{}{} "
                             "(baked-only; disk did not place it)",
                             e.lotId, e.data.areaNo, e.data.gridXNo, e.data.gridZNo, key, c,
                             sib ? " [sibling-of-disk-base]" : "",
                             as_enemy ? " [also-baked-as-enemy]" : "");
            }
        }
        // [ENEMY-MARKERS] diag (de-bake census of the enemy slice): enumerate the baked
        // enemy-drop rows that REACH HERE — i.e. the ones the disk enemy pass did NOT cover
        // (an enemy DEBAKE-GAP). Provenance Enemy is ALWAYS lotType 2 (ItemLotParam_enemy).
        // With loot_enemy_drops ON, the MSB Parts.Enemies → NPCParamID → NpcParam pass
        // reproduces most of them live (dropped by the enemy guard above); whatever survives
        // is the residual the live pass can't place (npc with no lot, spawn-disabled, or a
        // lot the bake categorised differently). With the pass OFF, this lists all 119 (the
        // whole baked enemy slice). The loaded-loot walker (MapIns+0x460 node) does NOT
        // pre-locate enemy drops — it only holds SPAWNED/opened loot — so the resident
        // Parts.Enemies pass is the right tool. Gated on diag_loot_pos.
        if (diag && e.loot_source == gen::LootSource::Enemy)
        {
            ++enemy_markers;
            // Recovery-lever classification: is this uncovered baked Enemy lot the enemy-lot of an
            // enemy the disk pass actually PARSED? If yes → the pass saw the enemy but didn't cover
            // this lot (npc_loot_lot map-preferred the 0x34 lot, or a filter dropped it) = RECOVERABLE
            // by also covering itemLotId_enemy. If no → the enemy isn't in the parsed MSB set at all
            // (dungeon / non-_00 scope, or part-filtered) = needs an MSB-scope fix, not a lot tweak.
            const bool parsed = e.lotId != 0 && parsed_enemy_lots.count(e.lotId) != 0;
            if (parsed) ++enemy_parsed_uncov; else ++enemy_not_parsed;
            ++enemy_uncov_by_area[e.data.areaNo];
            if (verbose)
            {
                int32_t key = goblin::resolve_loot_item_textid(e.lotId, e.lotType, -1);
                spdlog::info("[ENEMY-MARKERS] uncovered lot={} m{}_{}_{} cat={} key={} npc={} [{}]",
                             e.lotId, e.data.areaNo, e.data.gridXNo, e.data.gridZNo,
                             static_cast<int>(e.category), key,
                             e.object_name ? e.object_name : "(none baked)",
                             parsed ? "parsed-but-uncovered(map-pref/filtered)" : "not-parsed(scope)");
            }
        }
        // [RESIDUAL-SRC] survivor reached here = not replaced by any disk pass. Tally lot-backed
        // (or formerly-lot) loot rows by category+source for the full residual triage below.
        if (diag && (e.lotId != 0 || e.loot_source != gen::LootSource::Unknown))
        {
            ++resid_by_cat_src[c * 4 + static_cast<int>(e.loot_source)];
            // [RESIDUAL-ROW] per-row dump (diag_loot_pos) of EVERY surviving baked loot row, uniform
            // across sources — feeds the offline by-family orphan-lot batch probe (tools/_probe_enemy_
            // residual.py). Supersedes the source-specific [DEBAKE-GAP]/[ENEMY-MARKERS] per-row dumps
            // for a single grep-able feed: cat name (family = prefix), source, lot+lotType, tile, key.
            if (goblin::config::diagLootPos && e.lotId != 0)
            {
                static const char *SRCN[4] = {"unknown", "treasure", "enemy", "emevd"};
                int32_t rkey = goblin::resolve_loot_item_textid(e.lotId, e.lotType, e.data.textId1);
                spdlog::info("[RESIDUAL-ROW] cat=\"{}\" src={} lot={} lt={} m{}_{}_{} key={}",
                             goblin::markers::category_name(static_cast<gen::Category>(c)),
                             SRCN[static_cast<int>(e.loot_source) & 3], e.lotId, (int)e.lotType,
                             e.data.areaNo, e.data.gridXNo, e.data.gridZNo, rkey);
            }
        }
        push_marker(e.row_id, e.data, c, e.lotId, e.lotType);
    }
    if (goblin::config::lootCollectibles)
        spdlog::info("[LOOTDISK] replaced {} baked Rune/Ember Pieces with disk placements (identity dedup)",
                     replaced_piece);
    if (goblin::config::lootCollectibles)
        spdlog::info("[LOOTDISK] replaced {} baked Material Nodes by identity (tile+part-name; primary "
                     "dedup — catches the offset near-misses the positional pass left)", replaced_matnode);
    if (goblin::config::worldFeaturesFromDisk)
        spdlog::info("[LOOTDISK] kindling: {} markers emitted from disk SFX regions, {} baked twins "
                     "dropped (DiskMSB; graying via constant entity-id slot key)",
                     (int)kindling_disk_names.size(), replaced_kindling);
    if (goblin::config::lootFromDiskMsb)
        spdlog::info("[LOOTDISK] re-sourced {} baked treasure markers to cross-tile LOD positions "
                     "(DiskMSB; uncovered LOD-only treasures past a sibling-chain gap)", lod_treasure_pos_count);
    if (goblin::config::lootEnemyDrops)
        spdlog::info("[LOOTDISK] replaced {} baked enemy rows with disk enemy placements", replaced_enemy);
    if (goblin::config::lootEmevdDrops)
        spdlog::info("[LOOTDISK] replaced {} baked emevd rows with disk EMEVD placements", replaced_emevd);
    if (goblin::config::dropMerchantPhantoms)
        spdlog::info("[LOOTDISK] dropped {} baked merchant-phantom markers (item sold infinite-stock in "
                     "ShopLineupParam, no disk twin — bake fallback at tile corner)", dropped_merchant);
    g_skip.merchant_phantom = dropped_merchant;
    if (diag)
    {
        spdlog::info("[ENEMY-MARKERS] {} baked enemy-drop rows NOT covered by the disk enemy pass "
                     "(loot_enemy_drops {}); {} covered+replaced",
                     enemy_markers, goblin::config::lootEnemyDrops ? "ON" : "OFF", replaced_enemy);
        // Split of the uncovered enemy slice. INVESTIGATED EXHAUSTIVELY 2026-06-25 (see
        // [[msbe-enemy-loot-offsets]]): the residual (35 on ERR) is "not-parsed" = the baked lot is
        // referenced by NO NpcParam.itemLotId at all — confirmed against EVERY parsed enemy, the FULL
        // NpcParam table, the paramdef-authoritative offline scan, AND the vanilla regulation (all 0/35).
        // So these are NOT NpcParam enemy drops in any version — the bake MIS-LABELED corpse/EMEVD-scripted
        // loot as LootSource::Enemy (mapgenie shows them on "bodies"). The NpcParam enemy pass is COMPLETE;
        // the residual is a bake mis-tag, reproduced elsewhere by the treasure/emevd passes or phantom.
        std::string area_hist;
        for (const auto &kv : enemy_uncov_by_area)
            area_hist += 'm' + std::to_string(kv.first) + '=' + std::to_string(kv.second) + ' ';
        spdlog::info("[ENEMY-MARKERS] uncovered split: {} parsed-but-uncovered + {} not-parsed "
                     "(= bake mis-labeled non-NpcParam loot, not recoverable here) | by area: {}",
                     enemy_parsed_uncov, enemy_not_parsed, area_hist);
    }
    if (diag)
    {
        spdlog::info("[LOOTDISK] replaced {} baked lot rows with disk placements", replaced);
        // De-bake readiness gauge: how much of the baked Treasure slice the disk does NOT yet
        // reproduce. This must reach 0 (or be an explicitly-accepted residue) before the
        // Treasure slice can be dropped from the committed bake. Per-row list above under
        // diag_loot_pos. See docs / [[handoff-loot-from-real-files]] #11.
        spdlog::info("[DEBAKE-GAP] {} baked Treasure rows NOT covered by the disk source "
                     "(would be lost if the treasure slice is de-baked; set diag_loot_pos for the list)",
                     debake_gap);
        // Cause sample (does one lever dominate?): sequence-sibling = recoverable via an
        // ItemLotParam-chain walk (the multi-lot-treasure hypothesis); baked-as-enemy = a
        // source mis-tag; per-category + per-area show whether the residual is concentrated
        // (corpse loot, ~m60) or diffuse. If sibling is small and m60 dominates → accept the
        // residual; if sibling is large → build the chain walk for the treasure slice.
        spdlog::info("[DEBAKE-GAP] cause sample: {} sequence-sibling-of-disk-base (multi-lot/chain "
                     "candidate), {} also-baked-as-enemy (source mis-tag), {} neither",
                     gap_sibling, gap_baked_as_enemy,
                     debake_gap - gap_sibling - gap_baked_as_enemy);
        {
            std::string cat_hist, area_hist;
            for (const auto &kv : gap_by_cat)
            {
                cat_hist += goblin::markers::category_name(static_cast<gen::Category>(kv.first));
                cat_hist += '=' + std::to_string(kv.second) + ' ';
            }
            for (const auto &kv : gap_by_area)
                area_hist += 'm' + std::to_string(kv.first) + '=' + std::to_string(kv.second) + ' ';
            spdlog::info("[DEBAKE-GAP] by category: {}", cat_hist);
            spdlog::info("[DEBAKE-GAP] by area: {}", area_hist);
        }
        // [RESIDUAL-SRC] the COMPLETE surviving-baked-loot triage by provenance — every leftover
        // baked loot row split by source, so the recovery lever per category is obvious:
        //   Treasure → de-bake-gap (corpse loot absent from the mod's loot linkage; ~accepted).
        //   Enemy    → the disk Parts.Enemies + NpcParam pass (loot_enemy_drops) didn't place it.
        //   Emevd    → the disk EMEVD award pass (loot_emevd_drops) didn't reproduce it.
        //   Unknown  → pre-provenance bake row (field not regenerated) — needs a data rebuild.
        {
            const char *SRC[4] = {"unknown", "treasure", "enemy", "emevd"};
            int tot[4] = {0, 0, 0, 0};
            std::map<int, std::array<int, 4>> per_cat;  // category -> per-source split
            for (const auto &kv : resid_by_cat_src)
            {
                const int cat = kv.first / 4, src = kv.first % 4;
                per_cat[cat][src] += kv.second;
                tot[src] += kv.second;
            }
            spdlog::info("[RESIDUAL-SRC] surviving baked loot by source: unknown={} treasure={} "
                         "enemy={} emevd={} (total={})",
                         tot[0], tot[1], tot[2], tot[3], tot[0] + tot[1] + tot[2] + tot[3]);
            for (const auto &kv : per_cat)
                spdlog::info("[RESIDUAL-SRC]   {}: unk={} trea={} enem={} emev={}",
                             goblin::markers::category_name(static_cast<gen::Category>(kv.first)),
                             kv.second[0], kv.second[1], kv.second[2], kv.second[3]);
        }
        // Disk-only lots (placed by the disk but NOT in the bake's lotType==1 slice).
        // Split: filed-as-enemy (in baked_any → the bake had it as a lotType 2 drop)
        // vs absent-from-bake (genuinely new MSB loot the bake missed). Log a sample
        // [lotId @ tile → resolved item key] so the values can be eyeballed in-game.
        int only_enemy = 0, only_absent = 0, shown = 0;
        for (uint32_t lot : disk_lots)
        {
            if (baked_lot1.count(lot)) continue;  // this lot IS in the bake's slice
            const bool as_enemy = baked_any.count(lot) != 0;
            (as_enemy ? only_enemy : only_absent)++;
            if (verbose && shown < 25)
            {
                uint32_t tk = disk_lot_tile.count(lot) ? disk_lot_tile[lot] : 0;
                int32_t key = goblin::resolve_loot_item_textid(lot, 1, -1);
                spdlog::info("[LOOTDISK]   disk-only lot {} @ m{}_{}_{} key={} ({})", lot,
                             (tk >> 16) & 0xff, (tk >> 8) & 0xff, tk & 0xff, key,
                             as_enemy ? "baked-as-enemy" : "absent-from-bake");
                ++shown;
            }
        }
        spdlog::info("[LOOTDISK] disk-only lots: {} total ({} baked-as-enemy, {} absent-from-bake) "
                     "— vanilla/unmodified-map loot stays baked",
                     only_enemy + only_absent, only_enemy, only_absent);

        // RECOVER-LATER record: INERT DummyAsset placements we dropped (no entity
        // binding) whose lotId the bake STILL provides (lotType 1). The 2 reachable
        // dummies that carry an EntityID (4910/15000990) are now KEPT + emitted by
        // the disk source, so they no longer appear here — only entity-LESS dummies
        // the bake nonetheless shows remain (e.g. 2046460000, which the offline
        // pipeline itself flags unreachable; it has no MSB entity so it can't be
        // recovered via the +0x60 entity offset — an EMEVD/cut special).
        // See docs/re/windows_msbe_dummyasset_unreachable_re_findings.md.
        // True recover-later = bake shows it (baked_lot1) AND the disk does NOT
        // (not in disk_lots). A dropped dummy whose lotId ALSO has an Asset twin
        // is in disk_lots → the disk still emits it when the bake is gone → not
        // lost. Excluding disk_lots is what separates the real residue from the
        // lots that merely have a dummy duplicate alongside a live Asset.
        std::unordered_set<uint32_t> seen_dummy;
        int recover = 0;
        for (uint32_t lot : dropped_dummy_lots)
        {
            if (!baked_lot1.count(lot) || disk_lots.count(lot) ||
                !seen_dummy.insert(lot).second)
                continue; // inert, disk already emits an Asset twin, or already logged
            ++recover;
            if (verbose)
            {
                int32_t key = goblin::resolve_loot_item_textid(lot, 1, -1);
                spdlog::info("[LOOTDISK]   RECOVER-LATER inert-dummy lot {} key={} "
                             "(entity-less; bake-backed today, lost when the bake is dropped)",
                             lot, key);
            }
        }
        spdlog::info("[LOOTDISK] recover-later (entity-less dropped dummy, still baked) lots: {} "
                     "— reachable dummies w/ an EntityID are now disk-emitted", recover);
    }
    build_live_bosses();

    // Great Runes: the 6 demigod runes shown at their LIVE boss positions (joined by cleared flag).
    // Must run after build_live_bosses (reads its markers); feeds the world-feature category-wipe below.
    if (goblin::config::worldFeaturesFromDisk)
        build_great_rune_markers(world_feature_cells[static_cast<int>(gen::Category::KeyGreatRunes)]);

    // ── Finalize dedup: drop baked geom markers the disk collectible pass re-placed ──
    // The lotId-coverage replace (in the loop above) removes a baked TREASURE row when a
    // disk lot owns it — but the geom/SFX-tracked categories (Material Nodes / Rune /
    // Ember Pieces) carry lotId=0, so that path structurally CAN'T see them. When the disk
    // collectible pass (loot_collectibles) re-emits the SAME physical AEG gather node —
    // same world spot, but classified per-item into a DIFFERENT category — the baked copy
    // double-draws on the map and double-counts in the scoreboard.
    //
    // Rather than a per-model guard (which only catches today's _8xx scope and silently
    // re-regresses if the disk scope ever widens), dedup by POSITION at finalize: drop any
    // Baked marker in the geom categories that lands on a cell the collectible pass placed
    // (collectible_cells). This auto-covers any future double-count of this kind with no new
    // code, and only ever removes a baked marker a gather-asset placement PROVABLY re-drew at
    // the same spot. Keyed to the collectible pass (NOT all disk markers) so an unrelated
    // treasure/enemy that merely coincides can't evict a baked piece; scoped to the 3 lotId=0
    // geom categories (the exact complement of the lotId-replace). See [[aeg-collectible-source]].
    {
        int deduped = 0;
        // Material Nodes only — Rune/Ember Pieces dedup by IDENTITY in the baked loop (piece_disk_keys).
        const gen::Category kGeom[] = {gen::Category::LootMaterialNodes};
        for (gen::Category gc : kGeom)
        {
            auto &bucket = g_buckets[static_cast<int>(gc)];
            const size_t before = bucket.size();
            bucket.erase(std::remove_if(bucket.begin(), bucket.end(),
                                        [&](const Marker &m) {
                                            return m.source == Source::Baked &&
                                                   collectible_cells.count(cell_of(m));
                                        }),
                         bucket.end());
            deduped += static_cast<int>(before - bucket.size());
        }
        if (goblin::config::lootCollectibles)
            spdlog::info("[LOOTDISK] finalize dedup: dropped {} baked geom markers the disk collectible "
                         "pass already covers (position-keyed; geom categories only)", deduped);
    }

    // ── Finalize: drop the baked twins of every World-feature category the disk pass populated ──
    // Two modes, per the generated table's category_wipe flag:
    //  • category_wipe (dedicated category — Stakes, Imp): drop EVERY baked row of the category.
    //    The disk _00 pass is AUTHORITATIVE; the bake is NOT a reliable oracle — generate_*.py
    //    scans every tile incl. LOD-coarse _02 (proxies at a 128/256 offset — [[runtime-msb-
    //    resident-plan]] says parse _00 only) AND its dedup key omits gx,gz, so the baked rows are
    //    inflated with offset LOD-phantoms (Stakes: 226 in _00 vs 250 non-_00, bake 439 but disk
    //    world-cells 219). A positional dedup leaves the phantoms drawn at wrong spots → wipe all
    //    (same pattern as live bosses). Safe: all 651 _00 MSBs parse (0 fail/0 KRAK) and a ground
    //    feature always lives in a _00 tile.
    //  • cell-dedup (SHARED category — Hero's Tomb shares WorldInteractables with Seal Puzzles):
    //    drop only the baked twin sitting on a disk-placed cell, KEEPING the sibling features the
    //    disk pass doesn't reproduce. Cross-tile-remapped statues may miss the cell → a harmless
    //    baked duplicate stays (the rare edge case generate_hero_tomb_statues.py re-tiles).
    if (goblin::config::worldFeaturesFromDisk)
    {
        for (auto &[cat, cells] : world_feature_cells)
        {
            if (cells.empty()) continue;
            // The feature's finalize mode = the table row's category_wipe (a category is
            // single-mode). A category NOT in the asset table is a bespoke dedicated pass that
            // OWNS its category (Summoning Pools, live-param) → default category-wipe.
            bool wipe = true;
            for (size_t k = 0; k < gen::WORLD_FEATURE_MODEL_COUNT; ++k)
                if (static_cast<int>(gen::WORLD_FEATURE_MODELS[k].category) == cat)
                {
                    wipe = gen::WORLD_FEATURE_MODELS[k].category_wipe;
                    break;
                }
            // Maps stay cell-dedup (not category-wipe): the disk treasure pass covers 23/24 + the
            // emevd pass now recovers the 1 EMEVD-granted map (Altus Plateau, scarab-dropped) and
            // records its cell, so all 24 disk cells are present → every baked twin drops, baked→0.
            // Cell-dedup (vs wipe) keeps it safe if a future map lacks any disk source.
            if (cat == static_cast<int>(gen::Category::WorldMaps))
                wipe = false;
            auto &bucket = g_buckets[cat];
            const size_t before = bucket.size();
            if (wipe)
                bucket.erase(std::remove_if(bucket.begin(), bucket.end(),
                                            [](const Marker &m) { return m.source == Source::Baked; }),
                             bucket.end());
            else
                bucket.erase(std::remove_if(bucket.begin(), bucket.end(),
                                            [&](const Marker &m) {
                                                return m.source == Source::Baked && cells.count(cell_of(m));
                                            }),
                             bucket.end());
            spdlog::info("[LOOTDISK] world features: category {} dropped {} baked twins ({})",
                         cat, (int)(before - bucket.size()), wipe ? "category-wipe" : "cell-dedup");
        }
    }

    // Item stacking: ANNOTATE co-located identical-item loot groups (e.g. a Formic Rock node cluster).
    // Always run, config-independent — the stack_identical_items toggle is a render decision, so the
    // grouping must exist regardless and the toggle stays instant (no rebuild). After all finalize
    // passes so it sees the final marker set.
    annotate_item_stacks();

    // Off-page altitude: tag each marker with the nearest same-area grace's Y so the altitude badge
    // works on pages the player isn't on (grace replaces the out-of-frame player Y as the reference).
    assign_grace_altitude_refs();

    // [NONAME] diag: loot markers whose item key RESOLVED (icon works) but whose FMG name is EMPTY —
    // typically ERR-custom items missing from the (vanilla-derived) name preload. These now render
    // "Unknown item" + their count; this dump lists their keys/locations so the real names can be wired.
    if (goblin::config::diagLootFlags)
    {
        // Dedup by name_id: thousands of ammo markers share one key (Arrow = 150000000 everywhere), so
        // print each DISTINCT unresolved key once — that surfaces rare keys (e.g. an ERR-custom item)
        // instead of burying them under ammo. name_id==0 = no key at all (logged separately as a count).
        std::unordered_map<int32_t, int> distinct;   // unresolved name_id → instance count
        int zero_key = 0;
        for (int c = 0; c < (int)g_buckets.size(); ++c)
            for (const Marker &m : g_buckets[c])
            {
                if (m.name_id == 0) { ++zero_key; continue; }
                if (!goblin::lookup_text_utf8(m.name_id).empty()) continue;
                if (distinct.find(m.name_id) == distinct.end())
                    spdlog::info("[NONAME] name_id={} (key-100M={}) cat_bucket={} lot_backed={} lot={} loc='{}'",
                                 m.name_id, m.name_id - 100000000, c, (int)m.lot_backed, m.lotId,
                                 goblin::lookup_text_utf8(m.loc_pname));
                distinct[m.name_id]++;
            }
        if (!distinct.empty() || zero_key)
            spdlog::info("[NONAME] {} DISTINCT unresolved keys; {} markers with name_id==0 (no key). "
                         "Ammo keys 15xxxxxxx are the known FMG gap; non-ammo = ERR-custom suspects.",
                         (int)distinct.size(), zero_key);
    }

    // ── [SKIPPED] shown vs skipped: disk placements parsed but NOT drawn, by reason ─
    // The inverse of [COVERAGE]: of everything the passes parsed from the mod's files, how many
    // became markers vs were filtered, grouped by WHY. Each pass added its local filtered counts to
    // g_skip at its log line; dummy_inert comes from the load. Gated by diag_loot_pos.
    if (goblin::config::diagLootPos)
    {
        g_skip.dummy_inert = (int)dropped_dummy_lots.size();
        int drawn = 0;
        for (int c = 0; c < NUM_CAT; ++c) drawn += (int)g_buckets[c].size();
        const int skipped = g_skip.unclassified + g_skip.dedup + g_skip.no_anchor +
                            g_skip.by_design + g_skip.merchant_phantom + g_skip.dummy_inert;
        spdlog::info("[SKIPPED] disk placements parsed but NOT drawn (by reason):");
        spdlog::info("[SKIPPED]   unclassified={} (item type unknown → no category)", g_skip.unclassified);
        spdlog::info("[SKIPPED]   dedup={} (already placed by another pass)", g_skip.dedup);
        spdlog::info("[SKIPPED]   no_anchor={} (no lot / no positionable MSB entity)", g_skip.no_anchor);
        spdlog::info("[SKIPPED]   by_design={} (rune/ember GEOM-tracked, anti-flood clutter, "
                     "respawnable no-flag enemy drops)", g_skip.by_design);
        spdlog::info("[SKIPPED]   merchant_phantom={} (shop-∞ bake fallback, dropped)", g_skip.merchant_phantom);
        spdlog::info("[SKIPPED]   dummy_inert={} (cut DummyAsset, no EntityID)", g_skip.dummy_inert);
        spdlog::info("[SKIPPED] TOTAL skipped={} vs drawn={} (drawn = all g_buckets markers; "
                     "unclassified/no_anchor = real coverage gaps, dedup/by_design/phantom/inert = correct)",
                     skipped, drawn);
    }

    // ── [WATCH] finalize-survival: did each watched lot reach the final buckets? ────
    // Closes the loop on the per-pass [WATCH] emit/skip lines: a watched lot that emitted but is
    // ABSENT here was dropped by a downstream finalize (cell-dedup / category-wipe / replace). For
    // the Group-B treasures (Ghostflame Torch / Burred Bolt) this distinguishes "on the underground
    // page, just unchecked" (survived) from "parsed then dropped" (absent). Gated by diag_loot_pos.
    if (goblin::config::diagLootPos && !kWatchLots.empty())
    {
        for (uint32_t lot : kWatchLots)
        {
            int found_cat = -1, count = 0;
            for (int c = 0; c < NUM_CAT && found_cat < 0; ++c)
                for (const Marker &m : g_buckets[c])
                    if (m.lotId == lot) { found_cat = c; ++count; }
            if (found_cat >= 0)
                spdlog::info("[WATCH] lot={} SURVIVED in cat=\"{}\" (×{})", lot,
                             goblin::markers::category_name(static_cast<gen::Category>(found_cat)), count);
            else
                spdlog::info("[WATCH] lot={} ABSENT from final buckets (never emitted, or dropped at "
                             "finalize)", lot);
        }
    }

    // ── [BAKED-RESIDUAL] bulk diag: the AUTHORITATIVE no-bake residual ─────────────
    // One self-describing line per SURVIVING Baked marker across EVERY category, scanned from
    // g_buckets AFTER all passes + the world-feature finalize — so it reflects what the renderer
    // actually draws from the static bake, not the pre-finalize MAP_ENTRIES rows. This supersedes the
    // offline goblin_map_data parse (which mis-identified residuals: it doesn't see runtime
    // recategorize/drop, and a row's textId1 ≠ its live-resolved item). The name is resolved LIVE
    // (lookup_text_utf8 of the marker's live key) so each residual is human-identifiable with no
    // offline step — covers loot AND world features (the old [RESIDUAL-ROW] was loot-only). Gated by
    // diag_loot_pos. Companion offline tool: tools/_probe_residual_recover.py (recovery verdicts).
    if (goblin::config::diagLootPos)
    {
        int total = 0;
        for (int c = 0; c < NUM_CAT; ++c)
        {
            int baked = 0;
            for (const Marker &m : g_buckets[c])
            {
                if (m.source != Source::Baked) continue;
                ++baked;
                std::string nm = m.name_id > 0 ? goblin::lookup_text_utf8(m.name_id) : std::string();
                spdlog::info("[BAKED-RESIDUAL-ROW] cat=\"{}\" name=\"{}\" lot={} lt={} row_id={} "
                             "flag={} cleared={} m{}_{}_{} pos=({:.1f},{:.1f})",
                             goblin::markers::category_name(static_cast<gen::Category>(c)),
                             nm.c_str(), m.lotId, (int)m.lotType, (unsigned long long)m.row_id,
                             m.collected_flag, m.cleared_flag,
                             m.raw_area, m.raw_gx, m.raw_gz, m.raw_px, m.raw_pz);
            }
            if (baked)
                spdlog::info("[BAKED-RESIDUAL] cat=\"{}\" baked={}",
                             goblin::markers::category_name(static_cast<gen::Category>(c)), baked);
            total += baked;
        }
        spdlog::info("[BAKED-RESIDUAL] TOTAL surviving baked markers = {} (authoritative, post-finalize, "
                     "all categories)", total);
    }

    // ── [COVERAGE] no-bake scoreboard ────────────────────────────────────────────
    // Per category: how many markers came from the static bake (Baked) vs the active
    // mod's REAL disk MSBs (DiskMSB) vs live game params (Live), + how many needed the
    // live category fallback (classify_item_live = an item the baked table didn't know).
    // This is the "what still depends on the bake" view that drives the no-bake migration:
    // big BAKED-ONLY rows are the next categories to move off the bake. Logged once/build.
    {
        // r.drawn   = RAW markers actually pushed to g_buckets (one per placement) = what
        //             the renderer draws. This is baked+disk+live.
        // r.census  = the ImGui badge denominator (completable spots), computed the SAME way
        //             as refresh_overlay_census so the two agree: distinct collect/cleared
        //             flags for flag-based categories, the row count for the geom/SFX-tracked
        //             piece+kindling categories (each row is its own collectible), 0 for
        //             graces (no badge). drawn >> census wherever many markers share a flag
        //             or are respawnable (flag-less, EXCLUDED from the badge).
        // r.flag_*  = collect-flag COVERAGE: how many markers carry a collect/cleared flag
        //             (flagged, can be collect-tracked) vs none. Of the flag-less, respawn =
        //             lot-backed respawnable gather (no permanent done-state by design),
        //             nonloot = everything else (NPC/stake/spring/region — TYPES with no
        //             collect flag at all, the rows that can never gray/complete).
        // flag_live/disk/baked = of the flagged markers, where the marker (and thus its live-resolved
        // collect flag) came from — live read vs disk parse vs baked fallback. Shows collect-flag QUALITY:
        // live = drift-proof, baked = stale-risk (#3).
        struct CovRow { int c, baked, disk, live, lc, drawn, census, flagged, respawn, nonloot,
                            flag_live, flag_disk, flag_baked; };
        std::vector<CovRow> rows;
        int tB = 0, tD = 0, tL = 0, tC = 0;
        for (int c = 0; c < NUM_CAT; ++c)
        {
            CovRow r{c, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            const bool piece = c == static_cast<int>(gen::Category::ReforgedRunePieces) ||
                               c == static_cast<int>(gen::Category::ReforgedEmberPieces) ||
                               c == static_cast<int>(gen::Category::LootMaterialNodes);
            const bool kind  = c == static_cast<int>(gen::Category::WorldKindlingSpirits);
            const bool grace = c == static_cast<int>(gen::Category::WorldGraces);
            std::unordered_set<int> census_flags;  // distinct collect/cleared flags (flag-based census)
            for (const Marker &m : g_buckets[c])
            {
                switch (m.source)
                {
                case Source::Baked:   ++r.baked; break;
                case Source::DiskMSB: ++r.disk;  break;
                case Source::Live:    ++r.live;  break;
                }
                if (m.live_classified) ++r.lc;
                const int flag = m.collected_flag ? m.collected_flag : m.cleared_flag;
                if (flag)
                {
                    ++r.flagged;
                    switch (m.source)
                    {
                    case Source::Live:    ++r.flag_live;  break;
                    case Source::DiskMSB: ++r.flag_disk;  break;
                    default:              ++r.flag_baked; break;
                    }
                    census_flags.insert(flag);
                }
                else if (m.lot_backed)
                    ++r.respawn;   // respawnable gather node — flag-less by design
                else
                    ++r.nonloot;   // NPC/stake/spring/region — no collect flag (TYPE gap)
            }
            r.drawn = r.baked + r.disk + r.live;
            // Match refresh_overlay_census's badge denominator exactly.
            r.census = grace ? 0 : (piece || kind) ? r.drawn : static_cast<int>(census_flags.size());
            tB += r.baked; tD += r.disk; tL += r.live; tC += r.lc;
            if (r.drawn) rows.push_back(r);
        }
        std::sort(rows.begin(), rows.end(),
                  [](const CovRow &a, const CovRow &b) { return a.baked > b.baked; });
        spdlog::info("[COVERAGE] no-bake scoreboard (markers by provenance, baked-heavy first):");
        for (const CovRow &r : rows)
        {
            const char *status = (r.disk == 0 && r.live == 0) ? "BAKED-ONLY"
                                 : (r.baked == 0) ? (r.disk ? "disk" : "live")
                                                  : "partial";
            spdlog::info("[COVERAGE]   {:<24} baked={:<4} disk={:<4} live={:<4} live-cls={:<3} total={:<4} [{}]",
                         goblin::markers::category_name(static_cast<gen::Category>(r.c)),
                         r.baked, r.disk, r.live, r.lc, r.drawn, status);
        }
        // Census-vs-drawn + collect-flag coverage, keyed by the same category name so the
        // py generator can merge it into the same scoreboard row. drawn = real markers; census
        // = completable spots the badge shows; flag = flagged/drawn (rest = respawn + nonloot).
        spdlog::info("[COVERAGE-CENSUS] drawn vs badge census + collect-flag coverage "
                     "(flag-src = where the flagged markers' live collect-flag came from):");
        for (const CovRow &r : rows)
            spdlog::info("[COVERAGE-CENSUS]   {:<24} drawn={:<4} census={:<4} flagged={:<4} "
                         "respawn={:<4} nonloot={:<4} flag-live={:<4} flag-disk={:<4} flag-baked={:<4} total={:<4}",
                         goblin::markers::category_name(static_cast<gen::Category>(r.c)),
                         r.drawn, r.census, r.flagged, r.respawn, r.nonloot,
                         r.flag_live, r.flag_disk, r.flag_baked, r.drawn);
        spdlog::info("[COVERAGE] TOTAL baked={} disk={} live={} live-classified={} "
                     "(graces counted live separately in GraceLayer)",
                     tB, tD, tL, tC);
    }
    // ── [ITEMCLASS] live-fallback classification census ──────────────────────────
    // Every DISTINCT item that reached its category via the LIVE TAIL fallback (classify_item_live —
    // i.e. item_marker_category's confident taxonomy + curated exceptions returned -1, so only the
    // default/catch-all applied). This is the drift-prone surface: a mis-classified item — like the
    // ERR high-NG rune variants that fell to Crafting Materials before goods 82909-82919 was routed
    // to GoldenRunes — shows up here in the wrong bucket. tools/item_classification.py →
    // docs/item_classification.md turns it into a versioned regression guard (git diff catches a
    // future move). Items the confident classifier (taxonomy/exceptions) handles don't appear here.
    // Sorted by key for a stable diff. Unconditional + bounded (only the live-classified tail).
    {
        std::map<int32_t, std::pair<int, int>> itemclass;  // item key (name_id) → (category, count)
        for (int c = 0; c < NUM_CAT; ++c)
            for (const Marker &m : g_buckets[c])
                if (m.live_classified && m.name_id > 0)
                {
                    auto &e = itemclass[m.name_id];
                    e.first = c;
                    ++e.second;
                }
        spdlog::info("[ITEMCLASS] live-fallback classifications ({} distinct items; the catch-all tail surface):",
                     itemclass.size());
        for (const auto &[key, cv] : itemclass)
            spdlog::info("[ITEMCLASS] key={} cat=\"{}\" count={}", key,
                         goblin::markers::category_name(static_cast<gen::Category>(cv.first)), cv.second);
    }
    // ── Representative item-icon per category (live, no bake) ─────────────────────
    // For each ITEM category pick the most-common item key in its bucket and resolve that item's
    // REAL inventory iconId (item_real_icon_id → EquipParam.iconId). That iconId is the category's
    // representative game icon; the renderer draws it (harvested from the 00_Solo atlas) once
    // resident, falling back to the baked atlas otherwise. Drift-proof: no hardcoded item ids — the
    // representative follows whatever the live/disk pass actually placed (any mod/DLC).
    {
        int wired = 0;
        for (int c = 0; c < NUM_CAT; ++c)
        {
            std::map<int32_t, int> freq;  // item key → marker count
            for (const Marker &m : g_buckets[c])
                if (m.name_id > 0)
                    ++freq[m.name_id];
            // Most-common key (ties: lowest key, for a stable pick across runs).
            int32_t best_key = 0; int best_n = 0;
            for (const auto &[k, n] : freq)
                if (n > best_n) { best_n = n; best_key = k; }
            const int icon = best_key ? goblin::item_real_icon_id(best_key) : -1;
            goblin::worldmap::set_category_rep_icon(c, icon);
            if (icon > 0)
            {
                ++wired;
                goblin::gpu_want_item(icon);  // central registry → pump keeps it resident
                spdlog::info("[CATICON] cat=\"{}\" rep_key={} iconId={} (from {} markers)",
                             goblin::markers::category_name(static_cast<gen::Category>(c)),
                             best_key, icon, best_n);
            }
            // World-feature categories with a named map symbol (boss …) → want the symbol too, so
            // the central pump keeps it force-resident (it draws via native_map_point_icon_by_name).
            if (const char *sym = goblin::worldmap::category_gpu_icon_name(c))
            {
                char img[96];
                snprintf(img, sizeof(img), "img://%s", sym);
                goblin::gpu_want_symbol(img);
            }
        }
        spdlog::info("[CATICON] representative item-icons resolved for {} / {} categories", wired, NUM_CAT);
    }
}

// Disk-loot build is run ONCE on a background WORKER thread (not std::call_once):
// the map dir can resolve late (CreateFileW discovery), and parsing 651 MSBs
// (~0.7s) on the render/init thread is a visible hitch. The worker builds into
// g_buckets, then publishes with a release store of g_disk_built; readers
// (markers()/census) acquire-load it and return EMPTY until the build completes —
// so g_buckets is never read mid-write (after Found it's immutable). No hitch at
// discovery OR init; markers just appear a beat after the dir is known.
std::atomic<bool> g_disk_built{false};
std::atomic<bool> g_disk_running{false};    // a build worker is actively mutating g_buckets
std::atomic<bool> g_rebuild_pending{false}; // a fresh build was requested (e.g. a config toggle)

// Spawn THE single build worker if none is running. g_buckets and the build's shared maps have
// exactly one mutator at a time: the CAS on g_disk_running guarantees a second caller (a rapid
// second toggle, or a kick racing a rebuild) never starts a parallel build — the bug behind the
// item-stack-toggle crash, where two workers append to g_buckets / rehash the same unordered_map
// concurrently. A request that arrives mid-build sets g_rebuild_pending and the running worker
// loops once more, so the latest config still wins. The worker is the ONLY thing that clears +
// refills g_buckets (never the render/UI thread), and it holds g_disk_built=false for the whole
// mutation so readers (markers()) get the empty set instead of a half-written one.
void start_build_worker()
{
    bool expected = false;
    if (!g_disk_running.compare_exchange_strong(expected, true))
        return;  // a worker is already running; it will pick up g_rebuild_pending
    std::thread([] {
        for (;;)
        {
            g_rebuild_pending.store(false, std::memory_order_release);
            g_disk_built.store(false, std::memory_order_release);   // readers return empty during the build
            for (auto &b : g_buckets) b.clear();                    // sole mutator: clear then refill
            g_entity_pos.clear();
            build_buckets_impl();
            g_disk_built.store(true, std::memory_order_release);
            if (g_rebuild_pending.load(std::memory_order_acquire))
                continue;                                           // toggled again mid-build → redo
            g_disk_running.store(false, std::memory_order_release);
            // Re-check after releasing: a request that landed in the gap would otherwise be lost.
            if (!g_rebuild_pending.load(std::memory_order_acquire))
                return;
            bool e2 = false;
            if (!g_disk_running.compare_exchange_strong(e2, true))
                return;  // another caller re-acquired the worker slot; let it handle the request
        }
    }).detach();
}

void kick_disk_build()
{
    if (g_disk_built.load(std::memory_order_acquire)) return;
    start_build_worker();
}

// Ensure the buckets are (being) built. Disk source OFF → the bake is the source,
// built synchronously (std::call_once). Disk source ON → the disk map dir is
// REQUIRED: kick the background build once it's Found (ancestor-walk or the
// CreateFileW observer); Searching/Failed leave the buckets empty so the overlay
// shows its "maps not found" state instead of stale/bake markers.
void ensure_buckets()
{
    if (!disk_source_enabled())
    {
        std::call_once(g_build_once, build_buckets_impl);
        return;
    }
    ensure_map_dir_resolved();
    if (disk_loot_state() == DiskLootState::Found)
        kick_disk_build();  // async — no render/init-thread hitch
}

// True once the disk markers are safe to read. Always true when the disk source
// is off (the call_once build is synchronous). Acquire-pairs with the worker's
// release store so a true result guarantees g_buckets is fully written.
bool disk_markers_ready()
{
    return !disk_source_enabled() || g_disk_built.load(std::memory_order_acquire);
}
} // namespace

void prebuild_markers()
{
    // Wire CreateFileW discovery → kick the worker the instant the dir is Found,
    // so the fallback build doesn't wait for the next overlay tick (~7s).
    set_build_trigger(&kick_disk_build);
    ensure_buckets();
}

// Force a fresh bucket build so a config change that affects bucket CONTENT (e.g.
// stackIdenticalItems) takes effect without a map reload. Disk source only — the bake path builds
// via call_once and can't re-run; that's fine, the disk source is the live one. Thread-safe against
// rapid re-toggles: it does NOT touch g_buckets itself (that's the worker's job) and never starts a
// parallel build — it just drops the ready latch so readers stop, flags a rebuild, and asks the
// single worker machinery to (re)build. If a worker is mid-build, it loops once more for this request.
void rebuild_markers()
{
    if (!disk_source_enabled()) return;
    g_rebuild_pending.store(true, std::memory_order_release);  // ask for a fresh build
    g_disk_built.store(false, std::memory_order_release);      // readers return empty until it lands
    start_build_worker();                                      // start one iff none running
}

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
    ensure_buckets(); // built at setup_mod prebuild (or kicked async for the disk source)
    static const std::vector<Marker> kEmpty;
    if (!disk_markers_ready()) return kEmpty;  // disk build still running on the worker
    return g_buckets[cat_];
}

void refresh_overlay_census()
{
    namespace gen = goblin::generated;
    GOBLIN_BENCH("refresh.overlay_census");
    ensure_buckets(); // ensure the overlay markers exist (one-time, thread-safe)
    if (!disk_markers_ready()) return;  // disk build on the worker — census next frame

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
            int respawning = 0;  // lot-backed nodes with NO collect flag (getItemFlagId==0):
                                 // respawnable gather clutter (fireflies/stones, isEnableRepick).
                                 // They have no permanent "done" state by design, so they are
                                 // EXCLUDED from the completion counter — counting them would
                                 // inflate "remaining" forever (they can never reach looted).
            for (const Marker &m : g_buckets[c])
            {
                const int flag = m.collected_flag ? m.collected_flag : m.cleared_flag;
                if (!flag)
                {
                    if (m.lot_backed) ++respawning;  // respawning node → not a completion spot
                    continue;  // else not a counted item (NPC, spirit spring, stake, …)
                }
                all.insert(flag);
                if (goblin::ui::read_event_flag((uint32_t)flag))
                    taken.insert(flag);
            }
            total = (int)all.size();   // completable spots only (distinct persistent flags)
            looted = (int)taken.size();
            // The respawning nodes are intentionally absent from total/looted (excluded from
            // the "spots remaining" tally). Surface how many were dropped for transparency.
            if (!s_logged_once && respawning > 0)
                spdlog::info("[CENSUS] cat {:2} '{}' excluded {} respawning (flag-less) node(s) "
                             "from the completion counter",
                             c, goblin::markers::category_name(static_cast<gen::Category>(c)),
                             respawning);
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

bool entity_world_pos(uint32_t entity_id, float &worldX, float &worldZ, int &group)
{
    auto it = g_entity_pos.find(entity_id);
    if (it == g_entity_pos.end())
        return false;
    worldX = it->second.wx;
    worldZ = it->second.wz;
    group = it->second.group;
    return true;
}
} // namespace goblin::worldmap
