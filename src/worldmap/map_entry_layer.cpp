#include "map_entry_layer.hpp"

#include "category_meta.hpp"
#include "loot_disk.hpp"       // disk-MSB loot source (DiskTreasure / load_disk_treasures)
#include "goblin_config.hpp"   // config::lootFromDiskMsb
#include "goblin_map_data.hpp" // MAP_ENTRIES / MAP_ENTRY_COUNT / MapEntry / Category
#include "goblin_world_feature_models.hpp" // WORLD_FEATURE_MODELS (asset-model World features)
#include "goblin_inject.hpp"   // marker_world_pos / category_visible / read_event_flag / census
#include "goblin_logic.hpp"    // map_fragment_flag
#include "goblin_collected.hpp" // is_original_row_collected (piece graying)
#include "goblin_kindling.hpp"  // is_row_collected (kindling graying)
#include "goblin_markers.hpp"   // category_name (census log)
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
#include <mutex>
#include <unordered_set>

namespace goblin::worldmap
{
namespace
{
constexpr int NUM_CAT = static_cast<int>(goblin::generated::Category::WorldInteractables) + 1;

std::array<std::vector<Marker>, NUM_CAT> g_buckets;
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
    // Real inventory iconId for item/loot markers → native GPU icon (ItemIconProvider).
    // -1 for non-items (boss/grace/NPC keys miss) → they keep the category atlas icon.
    // Keyed by the live-resolved identity (same key as the label).
    m.icon_id = goblin::item_icon_id(name_id);
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
    g_buckets[c].push_back(m);
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
    for (const DiskTreasure &t : treasures)
    {
        from::paramdef::WORLD_MAP_POINT_PARAM_ST d{};
        d.areaNo = t.area;
        d.gridXNo = t.gx;
        d.gridZNo = t.gz;
        d.posX = t.posX;
        d.posZ = t.posZ;
        // Identity/category from the LIVE lot (lotType 1 = ItemLotParam_map). textId1
        // stays -1 and the flags 0, so push_marker resolves the item + pickup flag
        // live (the lot-backed path) — exactly like a baked lot-backed row.
        int32_t key = goblin::resolve_loot_item_textid(t.lotId, 1, -1);
        int c = goblin::item_marker_category(key);
        bool lc = false;
        if (c < 0) { c = goblin::classify_item_live(key); lc = (c >= 0); }  // live fallback (any mod / unbaked item)
        if (c < 0 || c >= NUM_CAT)
        {
            ++unclassified;  // item type genuinely unknown → no bucket
            continue;
        }
        push_marker(/*row_id=*/t.lotId, d, c, t.lotId, /*lotType=*/1, Source::DiskMSB, lc);
        covered.insert(t.lotId);
        lot_tile.emplace(t.lotId, pack_tile(t.area, t.gx, t.gz)); // first tile per lot
        ++emitted;
    }
    spdlog::info("[LOOTDISK] emitted {} disk loot markers, {} lots covered, {} unclassified",
                 emitted, (int)covered.size(), unclassified);
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
                                           std::unordered_set<std::string> &out_piece_keys)
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
                rid, c.area, c.gx, c.gz, slot, c.posX, c.posY, c.posZ, c.name});
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
        push_marker(/*row_id=*/lot, d, cat, lot, /*lotType=*/1, Source::DiskMSB, lc);  // one per placement
        // Record this gather-asset's projected cell so the finalize dedup can drop a
        // baked geom marker sitting on it — keyed to the COLLECTIBLE pass only, so an
        // unrelated treasure/enemy that merely coincides never evicts a baked piece.
        out_cells.insert(cell_of(g_buckets[cat].back()));
        covered.insert(lot);  // for the baked-row replace (de-dup vs the bake), NOT a skip key
        ++emitted;
        if (verbose) ++per_cat[cat];
    }
    // Register the Rune/Ember Piece placements for GEOF/WGM collected-graying (drained into the
    // tracking maps on the refresh thread — see goblin_collected). One-time per disk build.
    goblin::collected::register_runtime_entries(std::move(rt_entries));

    spdlog::info("[LOOTDISK] collectibles: {} markers emitted ({} assets total, {} non-ERR clutter "
                 "(pots/jars), {} not-a-pickup, {} treasure-dup, {} unclassified, {} rune/ember pieces)",
                 emitted, (int)collectibles.size(), clutter_skip, no_lot, dup, unclassified, piece_emitted);
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
                                     std::unordered_set<uint32_t> &covered)
{
    GOBLIN_BENCH("build.disk_enemies");
    int emitted = 0, no_lot = 0, unclassified = 0, dup = 0, respawn = 0, lot_dup = 0;
    const bool verbose = goblin::config::diagLootPos;
    std::unordered_map<int, int> per_cat;  // category → emitted count (diag)
    std::unordered_set<uint32_t> seen;      // dedup by lot (one marker per notable lot)
    for (const DiskEnemy &en : enemies)
    {
        uint8_t lt = 0;
        uint32_t lot = goblin::npc_loot_lot(en.npcParamId, &lt);  // live NpcParam chain
        if (lot == 0) { ++no_lot; continue; }
        if (treasure_lots.count(lot)) { ++dup; continue; }  // already a Treasure ground item
        if (goblin::resolve_loot_flag(lot, lt, 0) == 0) { ++respawn; continue; }  // not a one-time drop
        if (!seen.insert(lot).second) { ++lot_dup; continue; }  // a notable lot already placed
        int32_t key = goblin::resolve_loot_item_textid(lot, lt, -1);
        int cat = goblin::item_marker_category(key);
        bool lc = false;
        if (cat < 0) { cat = goblin::classify_item_live(key); lc = (cat >= 0); }  // live fallback (any mod / unbaked)
        if (cat < 0 || cat >= NUM_CAT) { ++unclassified; continue; }
        from::paramdef::WORLD_MAP_POINT_PARAM_ST d{};
        d.areaNo = en.area;
        d.gridXNo = en.gx;
        d.gridZNo = en.gz;
        d.posX = en.posX;
        d.posZ = en.posZ;
        push_marker(/*row_id=*/lot, d, cat, lot, /*lotType=*/lt, Source::DiskMSB, lc);
        covered.insert(lot);  // drops the matching baked LootSource::Enemy row
        ++emitted;
        if (verbose) ++per_cat[cat];
    }
    spdlog::info("[LOOTDISK] enemy drops: {} notable markers emitted ({} placements total; filtered: "
                 "{} npc-has-no-lot, {} treasure-dup, {} no-one-time-flag, {} same-lot-dedup, "
                 "{} unclassified)",
                 emitted, (int)enemies.size(), no_lot, dup, respawn, lot_dup, unclassified);
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
                                     const std::unordered_set<uint32_t> &treasure_lots,
                                     std::unordered_set<uint32_t> &covered)
{
    GOBLIN_BENCH("build.disk_emevd");
    // EntityID → enemy placement (position anchor). First occurrence wins, matching the
    // offline join (extract_all_items.py: `eid not in entity_to_pos`).
    std::unordered_map<uint32_t, const DiskEnemy *> ent_to_pos;
    ent_to_pos.reserve(enemies.size());
    for (const DiskEnemy &en : enemies)
        if (en.entityId != 0) ent_to_pos.emplace(en.entityId, &en);

    int emitted = 0, no_entity = 0, dup = 0, treasure_dup = 0, unclassified = 0;
    int emit_direct = 0, emit_ev1200 = 0;  // by mechanism (lotType 1 = direct, 2 = event-1200)
    // Mechanism C (sequence-sibling) diag.
    int sib_emitted = 0, sib_runeember = 0, sib_unclassified = 0;
    const bool verbose = goblin::config::diagLootPos;
    std::unordered_map<int, int> per_cat;  // category → emitted count (diag)
    std::unordered_set<uint64_t> seen;      // dedup by (entity<<32 | lot)
    std::unordered_set<uint32_t> sib_seen;  // sub-lots already emitted (mechanism C dedup)
    // Every award lot is a "base" — a sibling that equals another base is emitted as that
    // base (its own entity/pos), so don't double it in the sibling walk.
    std::unordered_set<uint32_t> base_lots;
    base_lots.reserve(awards.size());
    for (const DiskEmevd &a : awards) base_lots.insert(a.lotId);
    for (const DiskEmevd &a : awards)
    {
        auto it = ent_to_pos.find(a.entityId);
        if (it == ent_to_pos.end()) { ++no_entity; continue; }  // entity not an MSB enemy part
        uint64_t key2 = ((uint64_t)a.entityId << 32) | a.lotId;
        if (!seen.insert(key2).second) { ++dup; continue; }
        if (treasure_lots.count(a.lotId)) { ++treasure_dup; continue; }  // physical pickup already placed
        const DiskEnemy *pos = it->second;
        // Resolve identity with the award's lotType hint (1 = ItemLotParam_map / direct,
        // 2 = ItemLotParam_enemy / event-1200 boss drop), falling back to the other table.
        uint8_t lt = a.lotType;
        int32_t key = goblin::resolve_loot_item_textid(a.lotId, lt, -1);
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
        if (verbose) ++per_cat[cat];

        // ── Mechanism C: sequence-sibling expansion ──
        // The ItemLotParam row sequence base, base+1, base+2, … is one award bundle (e.g. a
        // boss drops Lhutel + Smithing Stone [2], or the Oracle Effigy + 3 Fortunes set). The
        // bake only emits the base; we walk the contiguous sub-lots and emit each notable one
        // at the SAME position. Walk BOTH ItemLotParam tables (some bundles cross tables — the
        // base is in _enemy, the reward sub-lots in _map), stopping at the first gap per table.
        // The _map branch additionally stops at a treasure base lot (otherwise consecutive _map
        // rows are unrelated placements). Skip rune/ember pieces (already on the map via the
        // Reforged GEOM category) and anything already placed.
        constexpr uint32_t kMaxSib = 50;
        for (uint8_t tbl = 1; tbl <= 2; ++tbl)
        {
            for (uint32_t off = 1; off <= kMaxSib; ++off)
            {
                uint32_t sub = a.lotId + off;
                if (tbl == 1 && treasure_lots.count(sub)) break;  // another treasure → chain ends
                uint32_t sflag = 0;
                int32_t skey = 0;
                if (!goblin::lot_row_in_table(sub, tbl, &sflag, &skey)) break;  // gap → chain ends
                if (sflag == 0 || skey == 0) continue;  // exists, but not a notable item drop
                if (base_lots.count(sub) || treasure_lots.count(sub)) continue;  // placed elsewhere
                if (!sib_seen.insert(sub).second) continue;  // already emitted as a sibling
                // ERR Rune/Ember Pieces already show via the Reforged GEOM category at their real
                // scattered locations; their lot bleeds into a boss/scarab sub-lot chain here, which
                // would draw a DUPLICATE at the wrong (boss) position. Suppress by goods id — they
                // are NOT in the baked ITEM_ICONS classifier (GEOM-tracked), so the category check
                // misses them. ERR-specific ids (no-op on other profiles): 800010 Rune, 850010 Ember.
                if (skey >= 500000000 && skey < 600000000)
                {
                    int32_t gid = skey - 500000000;
                    if (gid == 800010 || gid == 850010) { ++sib_runeember; continue; }
                }
                int scat = goblin::item_marker_category(skey);
                bool slc = false;
                if (scat < 0) { scat = goblin::classify_item_live(skey); slc = (scat >= 0); }
                if (scat < 0 || scat >= NUM_CAT) { ++sib_unclassified; continue; }
                from::paramdef::WORLD_MAP_POINT_PARAM_ST sd{};
                sd.areaNo = pos->area;
                sd.gridXNo = pos->gx;
                sd.gridZNo = pos->gz;
                sd.posX = pos->posX;
                sd.posZ = pos->posZ;
                push_marker(/*row_id=*/sub, sd, scat, sub, /*lotType=*/tbl, Source::DiskMSB, slc);
                covered.insert(sub);  // drops the matching baked LootSource::Emevd sub-lot row
                ++sib_emitted;
                if (verbose) ++per_cat[scat];
            }
        }
    }
    spdlog::info("[LOOTDISK] emevd drops: {} base markers ({} direct + {} event-1200) + {} sequence-"
                 "siblings = {} total ({} awards; filtered: {} entity-not-an-msb-enemy, {} "
                 "(entity,lot)-dedup, {} treasure-dup, {} base-unclassified; siblings: {} rune/ember-"
                 "skipped, {} unclassified)",
                 emitted, emit_direct, emit_ev1200, sib_emitted, emitted + sib_emitted,
                 (int)awards.size(), no_entity, dup, treasure_dup, unclassified, sib_runeember,
                 sib_unclassified);
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
        case gen::FlagRule::None:
        default:
            break;
        }

        const int cat = static_cast<int>(wf->category);
        from::paramdef::WORLD_MAP_POINT_PARAM_ST d{};
        d.areaNo = a.area;
        d.gridXNo = a.gx;
        d.gridZNo = a.gz;
        d.posX = a.posX;
        d.posZ = a.posZ;
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

// Build every category's marker cache in ONE pass over MAP_ENTRIES (9k rows), then the WorldBosses
// bucket LIVE from the param. Same world-projection + group classification as the grace layer.
// NOTE (2026-06-23): a "World-* categories live from WorldMapPointParam by row-id range" experiment
// was REVERTED — the deployed ERR regulation's live WorldMapPointParam holds only 740 rows (217
// textId2==5100 bosses + ~523 structural iconId=83/textId=-1 nav points); the Stakes/SummoningPools/
// SpiritSprings/Maps/etc. rows the bake expects are NOT present live. So only bosses (textId2==5100)
// and graces (BonfireWarpParam) are live-portable; every other category stays baked.
// Runs exactly once — wrapped by ensure_buckets()/std::call_once.
void build_buckets_impl()
{
    GOBLIN_BENCH("build.buckets");
    namespace gen = goblin::generated;

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
    if (disk_source_enabled())
    {
        // One disk read pass for all sources. Each out-vector requested only when on.
        // The EMEVD pass needs the enemy placements too (for the EntityID → position
        // join), so parse enemies whenever the enemy OR emevd source is on.
        std::vector<DiskCollectible> disk_collectibles;
        std::vector<DiskEnemy> disk_enemies;
        // World features need enemies too (Hostile NPC invaders ride the enemy enumeration).
        const bool wantEnemies = goblin::config::lootEnemyDrops || goblin::config::lootEmevdDrops ||
                                 goblin::config::worldFeaturesFromDisk;
        // World features (Stakes) are AEG asset placements → they ride the SAME asset
        // enumeration as collectibles (disk_collectibles), so request it when either is on.
        const bool wantAssets = goblin::config::lootCollectibles || goblin::config::worldFeaturesFromDisk;
        std::vector<DiskTreasure> treasures = load_disk_treasures(
            &dropped_dummy_lots,
            wantAssets ? &disk_collectibles : nullptr,
            wantEnemies ? &disk_enemies : nullptr);
        if (goblin::config::lootFromDiskMsb)
            build_disk_loot_markers(treasures, disk_lots, disk_lot_tile);
        // All MSB Treasure lots (the ground-item pickup assets) — used to skip
        // double-placing in the collectible/enemy/emevd passes. Built from the parsed
        // treasures directly, so it's correct even when loot_from_disk_msb is off.
        std::unordered_set<uint32_t> treasure_lots;
        if (goblin::config::lootCollectibles || wantEnemies)
            for (const DiskTreasure &t : treasures) treasure_lots.insert(t.lotId);
        if (goblin::config::lootCollectibles)
            build_disk_collectible_markers(disk_collectibles, treasure_lots, disk_lots,
                                           collectible_cells, piece_disk_keys);
        if (goblin::config::worldFeaturesFromDisk)
        {
            // EMEVD-sourced graying flags (entity → flag) for Hero's Tomb (template 90005683)
            // AND Hostile NPC defeat (90005792) — one event-dir scan feeds both passes.
            std::unordered_map<uint32_t, uint32_t> world_feature_flags =
                load_emevd_world_feature_flags();
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
        }
        if (goblin::config::lootEnemyDrops)
            build_disk_enemy_markers(disk_enemies, treasure_lots, enemy_disk_lots);
        if (goblin::config::lootEmevdDrops)
        {
            // The event-1200 (mechanism B) join needs the MSB enemy EntityIDs to resolve
            // which boss entity a setter event references.
            std::unordered_set<uint32_t> known_entities;
            known_entities.reserve(disk_enemies.size());
            for (const DiskEnemy &en : disk_enemies)
                if (en.entityId) known_entities.insert(en.entityId);
            std::vector<DiskEmevd> awards = load_emevd_awards(known_entities);
            build_disk_emevd_markers(awards, disk_enemies, treasure_lots, emevd_disk_lots);
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
    int replaced_enemy = 0;  // baked Enemy rows dropped because the disk enemy pass covers them
    int replaced_emevd = 0;  // baked Emevd rows dropped because the disk EMEVD pass covers them
    int debake_gap = 0;  // Treasure-sourced baked rows the disk did NOT cover (de-bake blocker)
    int enemy_markers = 0;  // [ENEMY-MARKERS] baked enemy-drop rows NOT covered by the disk pass
    for (size_t i = 0; i < gen::MAP_ENTRY_COUNT; ++i)
    {
        const gen::MapEntry &e = gen::MAP_ENTRIES[i];
        int c = static_cast<int>(e.category);
        if (c < 0 || c >= NUM_CAT)
            continue;
        if (diag && e.lotId != 0)
        {
            if (e.lotType == 1) baked_lot1.insert(e.lotId);
            if (e.lotType != 0) baked_any.insert(e.lotId);
        }
        // Bosses come LIVE from the param now (build_live_bosses) — skip any baked WorldBosses
        // rows so they don't double the live ones (the bake is being retired for this category).
        if (e.category == gen::Category::WorldBosses)
            continue;
        // Rune/Ember Piece IDENTITY dedup: the disk collectible pass placed this exact AEG world
        // piece (matched by tile + part name, NOT position — robust to the m11_10 offset anomaly).
        // Drop the baked twin; the disk marker carries the real MSB position + runtime geom graying.
        // Boss-flag pieces (c-model object_names) are absent from piece_disk_keys → correctly kept.
        if (!piece_disk_keys.empty() && e.object_name &&
            (e.category == gen::Category::ReforgedRunePieces ||
             e.category == gen::Category::ReforgedEmberPieces) &&
            piece_disk_keys.count(piece_key(e.data.areaNo, e.data.gridXNo, e.data.gridZNo, e.object_name)))
        {
            ++replaced_piece;
            continue;
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
        // EMEVD-scripted disk source (loot_emevd_drops) owns this lot → drop the baked
        // LootSource::Emevd row (lotType 1). Its own guard: the live event\*.emevd.dcx
        // template-award parse + EntityID→MSB-Enemy join reproduces this scripted drop.
        if (!emevd_disk_lots.empty() && e.loot_source == gen::LootSource::Emevd &&
            e.lotId != 0 && emevd_disk_lots.count(e.lotId))
        {
            ++replaced_emevd;
            continue;
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
            if (verbose)
            {
                int32_t key = goblin::resolve_loot_item_textid(e.lotId, 1, -1);
                spdlog::info("[DEBAKE-GAP] uncovered Treasure lot {} @ m{}_{}_{} key={} "
                             "(baked-only; disk did not place it)",
                             e.lotId, e.data.areaNo, e.data.gridXNo, e.data.gridZNo, key);
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
        if (goblin::config::diagLootPos && e.loot_source == gen::LootSource::Enemy)
        {
            ++enemy_markers;
            int32_t key = goblin::resolve_loot_item_textid(e.lotId, e.lotType, -1);
            spdlog::info("[ENEMY-MARKERS] uncovered lot={} m{}_{}_{} cat={} key={} npc={}",
                         e.lotId, e.data.areaNo, e.data.gridXNo, e.data.gridZNo,
                         static_cast<int>(e.category), key,
                         e.object_name ? e.object_name : "(none baked)");
        }
        push_marker(e.row_id, e.data, c, e.lotId, e.lotType);
    }
    if (goblin::config::lootCollectibles)
        spdlog::info("[LOOTDISK] replaced {} baked Rune/Ember Pieces with disk placements (identity dedup)",
                     replaced_piece);
    if (goblin::config::lootEnemyDrops)
        spdlog::info("[LOOTDISK] replaced {} baked enemy rows with disk enemy placements", replaced_enemy);
    if (goblin::config::lootEmevdDrops)
        spdlog::info("[LOOTDISK] replaced {} baked emevd rows with disk EMEVD placements", replaced_emevd);
    if (goblin::config::diagLootPos)
        spdlog::info("[ENEMY-MARKERS] {} baked enemy-drop rows NOT covered by the disk enemy pass "
                     "(loot_enemy_drops {}); {} covered+replaced",
                     enemy_markers, goblin::config::lootEnemyDrops ? "ON" : "OFF", replaced_enemy);
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

    // [PIECE-RESIDUAL] diag: of the baked AEG world Pieces (AEG099_821/822), which were NOT placed
    // by the disk pass (identity key absent from piece_disk_keys) — the true migration gap (disk
    // didn't parse the tile, or a name mismatch). Boss-flag pieces (c-model names) are excluded —
    // they correctly stay baked (EMEVD event 1200, no AEG twin). Runs once/build; capped list.
    if (goblin::config::lootCollectibles)
    {
        int rune_world = 0, rune_miss = 0, ember_world = 0, ember_miss = 0, shown = 0;
        for (size_t i = 0; i < gen::MAP_ENTRY_COUNT; ++i)
        {
            const gen::MapEntry &e = gen::MAP_ENTRIES[i];
            const bool isR = e.category == gen::Category::ReforgedRunePieces;
            const bool isE = e.category == gen::Category::ReforgedEmberPieces;
            if ((!isR && !isE) || !e.object_name)
                continue;
            if ((isR && std::strncmp(e.object_name, "AEG099_821", 10) != 0) ||
                (isE && std::strncmp(e.object_name, "AEG099_822", 10) != 0))
                continue;  // boss-flag piece (c-model) — correctly stays baked
            (isR ? rune_world : ember_world)++;
            if (piece_disk_keys.count(
                    piece_key(e.data.areaNo, e.data.gridXNo, e.data.gridZNo, e.object_name)))
                continue;  // disk placed it (identity match) — dropped by the baked-loop dedup
            (isR ? rune_miss : ember_miss)++;
            if (shown++ < 40)
                spdlog::info("[PIECE-RESIDUAL] {} {} m{}_{}_{} — no disk twin (disk didn't place this name)",
                             isR ? "Rune" : "Ember", e.object_name, e.data.areaNo, e.data.gridXNo,
                             e.data.gridZNo);
        }
        spdlog::info("[PIECE-RESIDUAL] AEG world Pieces NOT disk-covered: Rune {}/{}, Ember {}/{} "
                     "(boss-flag pieces excluded; identity-keyed)",
                     rune_miss, rune_world, ember_miss, ember_world);
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
        struct CovRow { int c, baked, disk, live, lc, drawn, census, flagged, respawn, nonloot; };
        std::vector<CovRow> rows;
        int tB = 0, tD = 0, tL = 0, tC = 0;
        for (int c = 0; c < NUM_CAT; ++c)
        {
            CovRow r{c, 0, 0, 0, 0, 0, 0, 0, 0, 0};
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
        spdlog::info("[COVERAGE-CENSUS] drawn vs badge census + collect-flag coverage:");
        for (const CovRow &r : rows)
            spdlog::info("[COVERAGE-CENSUS]   {:<24} drawn={:<4} census={:<4} flagged={:<4} "
                         "respawn={:<4} nonloot={:<4} total={:<4}",
                         goblin::markers::category_name(static_cast<gen::Category>(r.c)),
                         r.drawn, r.census, r.flagged, r.respawn, r.nonloot, r.drawn);
        spdlog::info("[COVERAGE] TOTAL baked={} disk={} live={} live-classified={} "
                     "(graces counted live separately in GraceLayer)",
                     tB, tD, tL, tC);
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
std::atomic<bool> g_disk_kicked{false};
void kick_disk_build()
{
    if (g_disk_built.load(std::memory_order_acquire)) return;
    bool expected = false;
    if (!g_disk_kicked.compare_exchange_strong(expected, true)) return;  // one worker only
    std::thread([] {
        build_buckets_impl();
        g_disk_built.store(true, std::memory_order_release);
    }).detach();
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
} // namespace goblin::worldmap
