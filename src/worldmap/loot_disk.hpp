#pragma once
// Disk-MSB loot source — derives treasure loot placements from the ACTIVE mod's
// real map/MapStudio/*.msb.dcx files (no committed bake). Gated by config
// loot_from_disk_msb. The parse chain (msbe_parser) + the position transform are
// RE'd + disk-validated to 99.3% exact-match vs the bake:
//   docs/re/windows_resident_msbe_layout_re_findings.md (the chain)
//   docs/re/windows_msbe_position_transform_validation.md (world = grid*256 + pos)
// The block-local Part+0x20 position here is exactly the bake's x/z, so the
// runtime reuses the SAME marker_world_pos transform downstream (no new RE).
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "msbe_parser.hpp"  // msbe::GestureRef (World-feature gesture refs from the EMEVD scan)

namespace goblin::worldmap
{
// One positioned treasure read from a disk MSB: an itemLotId at a block-local
// position on a named tile. A single lotId may appear MORE THAN ONCE (one per
// MSB Treasure event / part) — each is its own entry (emit each).
struct DiskTreasure
{
    uint32_t lotId = 0;
    uint8_t  area = 0, gx = 0, gz = 0;  // from the m{AA}_{BB}_{CC}_00 filename
    float    posX = 0.0f, posY = 0.0f, posZ = 0.0f;  // Part+0x20 (block-local; Y for the altitude badge)
};

// One placed AEG collectible asset read from a disk MSB. The item is resolved
// LIVE by the caller: goblin::aeg_pickup_lot(aegRow) →
// AssetEnvironmentGeometryParam.pickUpItemLotParamId → ItemLotParam_map → goods.
struct DiskCollectible
{
    uint32_t aegRow = 0;                // AEG{A}_{B} → A*1000+B (param row id)
    uint32_t entityId = 0;             // MSB EntityID (0 if unset) — World-feature interactive split
    uint8_t  area = 0, gx = 0, gz = 0;  // from the tile filename
    float    posX = 0.0f, posY = 0.0f, posZ = 0.0f;  // Part+0x20 (block-local; = bake x/y/z)
    std::string name;                   // full MSB part name, e.g. "AEG099_821_9003" (geom tracking)
    std::string modelName;              // ACTUAL model (msbe::Asset::modelName) — ERR substitutes
                                        // some gather models; GEOF graying buckets by THIS, not name
};

// One placed Enemy read from a disk MSB. The drop lot is resolved LIVE by the
// caller: goblin::npc_loot_lot(npcParamId) → NpcParam.itemLotId_map/_enemy →
// ItemLotParam → goods. Position = the enemy part (same transform as treasures).
struct DiskEnemy
{
    uint32_t npcParamId = 0;            // MSB Enemy part's NPCParamID (NpcParam row id)
    uint32_t entityId = 0;             // MSB EntityID (the EMEVD-award join key; 0 if unset)
    uint8_t  area = 0, gx = 0, gz = 0;  // from the tile filename
    float    posX = 0.0f, posY = 0.0f, posZ = 0.0f;  // Part+0x20 (block-local; Y for the altitude badge)
    std::string name;                   // part name (starts with the model, e.g. "c4210_9000")
                                        // — the no-bake Spiritspring Hawk model filter (SSO, cheap)
};

// One placed spirit-spring Region read from a disk MSB (worldFeaturesFromDisk). subtype
// 46=MountJump / 54=LockedMountJump launch point, or -1=Others named "FakeSpiritSpringJump".
// Position = the region's block-local pos (same transform as parts). See msbe::Region.
struct DiskRegion
{
    int32_t  subtype = 0;
    uint8_t  area = 0, gx = 0, gz = 0;  // from the tile filename
    float    posX = 0.0f, posY = 0.0f, posZ = 0.0f;  // region+0x14 (block-local; = bake x/y/z)
    std::string name;                   // for diag / future name-based filters
};

// One EMEVD template item-award (loot_emevd_drops): a bank-2000 event init carries
// (entityId, lotId). The lot is an ItemLotParam_map row (lotType 1); the position comes
// from the MSB Enemy part with this entityId (join via DiskEnemy::entityId). Parsed from
// the mod's event\*.emevd.dcx by load_emevd_awards(). See msbe::parse_emevd.
struct DiskEmevd
{
    uint32_t entityId = 0;
    uint32_t lotId = 0;
    // Which ItemLotParam the lot lives in for the LIVE identity resolve: 1 = _map
    // (mechanism A, direct template awards), 2 = _enemy (mechanism B, the event-1200
    // boss drops resolve via NpcParam.itemLotId_enemy's table). The caller resolves with
    // this hint, falling back to the other table if it yields no item.
    uint8_t  lotType = 1;
    // True when this award is a boss-reward template (90005860/61/80): its lot is a BASE whose
    // ItemLotParam chain (base+1/+2) holds a Rune/Ember Piece. The marker pass walks the chain +
    // emits the piece under the Reforged category (the rune/ember sibling suppression is lifted
    // for these). See build_disk_emevd_markers + [[nobake-coverage-scoreboard]].
    bool     bossReward = false;
    // True for awards whose anchor MAY be an MSB Asset, not just an Enemy: direct template awards
    // (some carry an asset-anchored reward — Blaidd's spirit ash, the scarab-cluster runes) and
    // boss rewards. FALSE for perTile enemy-death awards, which stay strictly entity→ENEMY: an
    // asset-join there re-introduces the ~395 asset-entity-chest over-match (see [[nobake-coverage-
    // scoreboard]]). The join (build_disk_emevd_markers) picks the enemy-only vs enemy+asset position
    // map by this flag.
    bool     allowAsset = false;
    // Loose-anchor fallback windows (direct awards only) — see msbe::EmevdAward::anchors. The join
    // tries `entityId` first, then these in order when it doesn't resolve to a position.
    std::vector<uint32_t> anchors;
};

// True when any disk-MSB source is enabled (treasure loot OR collectibles); both
// share the same map-dir discovery + parse pass.
bool disk_source_enabled();

// Record the DLL's own mod folder (parent of MapForGoblins.dll) for map-dir
// auto-detect. Call once at init, before load_disk_treasures().
void set_mod_folder(const std::filesystem::path &p);

// Resolve the map dir (config loot_msb_dir, else auto-detect) and parse every
// _00 MSB in it, returning all POSITIONED treasures (partIndex >= 0) on a real
// Asset part OR a reachable DummyAsset (one with an EntityID/EntityGroupID — an
// EMEVD can activate it). Only INERT DummyAsset (cut, no entity) placements are
// dropped; their lotIds are appended to droppedDummyLots (when non-null) so the
// caller can flag any "recover-later" lot the bake still backs. (The 3 reachable
// dummies are now emitted here — no longer bake-dependent.) Logs [LOOTDISK]
// per-map (debug) + totals. Empty when no dir.
// When `collectibles` is non-null, also enumerate AEG collectible assets into it.
// When `enemies` is non-null, also enumerate Enemy placements into it.
// When `regions` is non-null, also enumerate spirit-spring POINT regions into it.
// All sources share one disk read + parse pass.
std::vector<DiskTreasure> load_disk_treasures(std::vector<uint32_t> *droppedDummyLots = nullptr,
                                              std::vector<DiskCollectible> *collectibles = nullptr,
                                              std::vector<DiskEnemy> *enemies = nullptr,
                                              std::vector<DiskRegion> *regions = nullptr);

// Parse every event\*.emevd.dcx in the active mod (sibling of the resolved map\MapStudio
// dir) and return the EMEVD item-award references the runtime can position:
//   (A) direct template awards            → (entityId, lotId, lotType 1)
//   (B) event-1200 boss drops             → (entityId, lotId, lotType 2)
// (A) is the bank-2000 template inits (entity carried in the init). (B) joins
// common.emevd's RunEvent(1200,[flag,lot]) with a per-map SetEventFlag(flag,1) event whose
// referenced entity is one of `knownEntities` (the MSB enemy EntityIDs, boss-preferred).
// The caller joins each entityId to an MSB Enemy position (DiskEnemy::entityId) to place
// the marker. Empty when no event dir is found. See docs §5b (mechanisms A + B).
//
// `entitiesByTile` (tile key area<<16|gx<<8|gz → that tile's MSB enemy EntityIDs) scopes the
// setter→boss candidate resolution to the setter's OWN map, matching the offline bake's
// per-map `valid_entities`. Without it a global intersection picks a lower-numbered boss-like
// EntityID from a different dungeon that coincides with a 4-byte window in the event, mislocating
// the per-dungeon Rune Piece (see [[nobake-coverage-scoreboard]]). `knownEntities` is still used
// for the flag==EntityID boss-reward shortcut (a globally-unique id, no map needed).
std::vector<DiskEmevd> load_emevd_awards(
    const std::unordered_set<uint32_t> &knownEntities,
    const std::unordered_map<uint32_t, std::unordered_set<uint32_t>> &entitiesByTile);

// Targeted non-_00 scan for EMEVD-award entities that are NOT _00 enemies. Some direct
// template awards reference an entity that exists only as an overworld LOD proxy in a
// non-_00 tile (e.g. lot 2045460500's c5170 lives in m61_11_11_02, not any _00 tile), so the
// _00-only enemy enumeration in load_disk_treasures misses it and the award never places.
// This parses the non-_00 tiles' Enemy section ONLY for the given entity ids, returning each
// as a DiskEnemy at its LOD-tile position (= the position the bake used). Caller appends the
// result to disk_enemies for the EMEVD join ONLY (after build_disk_enemy_markers + known_entities
// are built, so it adds no LOD phantoms to the enemy pass or the boss/setter resolution). Stops
// early once every wanted id is resolved. Empty when `wanted` is empty or no dir is resolved.
std::vector<DiskEnemy> load_lod_award_entities(const std::unordered_set<uint32_t> &wanted);

// Targeted non-_00 scan for World-feature ASSETS whose model lives ONLY in LOD supertiles (the
// asset analogue of load_lod_award_entities). Some asset-model features — the Snow Town
// seal-release statues AEG110_029 — exist solely as cross-tile proxies in a non-_00 supertile
// (m60_24_28_01, part name "m60_48_57_00-AEG110_029_…"), so the _00-only asset enumeration in
// load_disk_treasures misses them and the world-feature pass never places them. This parses the
// non-_00 tiles' Asset section ONLY for the given aegRows, returning each as a DiskCollectible whose
// marker tile comes from the CROSS-TILE part-name prefix "m{AA}_{BB}_{CC}_00-…" (= the fine tile the
// supertile stores the part relative to; the bake parses the same prefix). Caller appends the result
// to disk_collectibles BEFORE build_disk_world_feature_markers (after build_disk_collectible_markers,
// so these never count as loot collectibles). `wanted` = the WORLD_FEATURE_MODELS rows with lod_scan;
// set lod_scan ONLY on models with no _00 placements (else _00 + LOD copies double-count). Empty when
// `wanted` is empty or no dir is resolved.
std::vector<DiskCollectible> load_lod_feature_assets(const std::unordered_set<uint32_t> &wanted);

// Targeted non-_00 scan for cross-tile LOD TREASURES — the treasure analogue of
// load_lod_feature_assets. A handful of MSB Treasure events (18 across ERR, all Caelid m60)
// exist ONLY in an overworld LOD supertile, bound to a cross-tile asset part named
// "m{AA}_{BB}_{CC}_00-AEG…" (e.g. lot 1040540050 Dragonwound Grease lives only in m60_10_13_02 /
// part m60_40_54_00-AEG099_620_9000). The _00-only load_disk_treasures misses them; most are
// covered anyway as the contiguous-sibling of a _00 base, but a few sit past a chain gap and stay
// baked residual (the oracle's RECOVER-TREASURE class, tools/_probe_residual_recover.py). This
// returns each as a DiskTreasure whose marker tile comes from the cross-tile part-name PREFIX (= the
// fine tile the supertile stores the part relative to; the bake parses the same prefix). The caller
// uses it to RE-SOURCE a baked residual treasure's position from disk (kindling pattern) — it does
// NOT emit new markers, so the sibling-covered lots are untouched (no double-place). Empty when no
// dir is resolved. Skips GameEditionDisable parts via the same path as the _00 treasure scan.
std::vector<DiskTreasure> load_lod_treasures();

// Parse the active mod's event\*.emevd.dcx and return the World-feature graying flags:
// (entityId -> activated event flag) for each EMEVD flag-template (Hero's Tomb statue
// 90005683, Hostile NPC defeat 90005792). The World-feature disk pass joins an interactive
// asset's/enemy's EntityID to its flag so an activated/defeated instance grays/hides like the
// bake did — no committed bake. Empty when no event dir is found. Shares resolve_event_dir +
// the Oodle/KRAK path with load_emevd_awards.
//
// `paintings_out` (optional): filled in the SAME scan with the painting events (entityId ->
// collection flag 580000-580199; parse_emevd_paintings) so the painting disk pass needs no
// second ~550-file event-dir read. nullptr = skip the extra per-file parse.
// `gestures_out` (optional): likewise filled with the gesture-spawn refs (template 90005570;
// parse_emevd_gestures) — one msbe::GestureRef per call — so the gesture pass shares this scan.
// `portals_out` (optional): likewise filled with the sending-gate entity ids (warp template
// 90005605, entity@arg[2]; parse_emevd_portal_gates), deduped — the Portal disk pass shares this scan.
std::unordered_map<uint32_t, uint32_t> load_emevd_world_feature_flags(
    std::unordered_map<uint32_t, uint32_t> *paintings_out = nullptr,
    std::vector<msbe::GestureRef> *gestures_out = nullptr,
    std::unordered_set<uint32_t> *portals_out = nullptr);

// One quest NPC, mined at runtime from the ACTIVE install's emevds (mod-agnostic; the runtime
// port of tools/extract_quest_npcs.py). `concluded` = its _q99 fail flag; [regLo,regHi] = its
// coarse state register; `entities` = its MSB placements (positions/names come from the MSB +
// NpcParam runtime passes). See load_quest_npcs.
struct QuestNpcRuntime
{
    uint32_t concluded = 0, regLo = 0, regHi = 0;
    std::vector<uint32_t> entities;
};
// Scan every event\*.emevd.dcx in the active install for the ENGINE-standard 90005702 quest-NPC
// handler and group by (concluded, regLo, regHi). Same file-scan + Oodle/KRAK path as
// load_emevd_awards. Mod-agnostic, no bake. Empty when no event dir is found.
std::vector<QuestNpcRuntime> load_quest_npcs();

// ── Map-dir discovery state (F1 error + CreateFileW fallback) ──────────────────
// With loot_from_disk_msb on, the map dir is resolved by ancestor-walk at init
// (Found). If that finds nothing we fall back to OBSERVING the game's own map
// opens via the CreateFileW hook (Searching): the first *.msb.dcx open reveals
// the real dir, loader-agnostic (the [MAPOPEN] essai proved ME3 opens each map
// by CreateFileW with the resolved mod path). If no map opens within an in-game
// timeout the state goes Failed → the overlay shows a red error and builds no
// markers (the disk source is REQUIRED when the feature is on).
enum class DiskLootState { Disabled, Found, Searching, Failed };

// Resolve the map dir once (ancestor-walk). Found → cache + state Found; empty →
// state Searching (the CreateFileW observer completes it). Idempotent / cheap.
void ensure_map_dir_resolved();

// Current state. Lazily flips Searching→Failed once the in-game timeout elapses
// (no per-frame tick needed — evaluated on access).
DiskLootState disk_loot_state();

// The resolved (or last-searched) MapStudio dir, for the build + the error text.
std::filesystem::path disk_loot_dir();

// Called by the CreateFileW observer for every *.msb.dcx the game opens. While
// the dir is not yet Found, captures its map\MapStudio parent → flips to Found.
void on_map_opened_path(const wchar_t *full_path);

// Registered by the marker layer at init: invoked once, the instant the map dir
// flips to Found via CreateFileW discovery, so the worker build kicks immediately
// instead of waiting for the next overlay tick (~7s). Set before any map opens.
void set_build_trigger(void (*fn)());

// Read + DCX-decompress an arbitrary game data file, given relative to the active
// install/mod root (e.g. "menu/hi/01_common.sblytbnd.dcx"). Resolves the root via the
// SAME ancestor-walk as the map dir (mod overlay first, then the UXM-unpacked game),
// reads the file, and Oodle/zlib-decompresses it (no-op if it isn't a DCX). Returns the
// raw decompressed bytes (BND4/TPF/…), or empty on any failure. INDEPENDENT of the loot
// map-dir state — the generic no-bake "give me a real game file" primitive, reusable by
// the item-icon layout now and the item-icon DDS sheets (00_Solo.tpfbhd) later. Does
// disk I/O — call off the engine thread / once.
std::vector<uint8_t> read_game_file_decompressed(const std::string &rel_path);

// Loose-only variant: resolves the mod overlay / UXM-unpacked file and
// decompresses it, but does NOT fall back to the packed dvdbnd. Empty if the
// file isn't present loose. Lets callers prefer a mod's own file over vanilla.
std::vector<uint8_t> read_loose_file_decompressed(const std::string &rel_path);

// Extract one or more item-icon ATLAS SHEET DDS images from the menu texture pack
// menu/hi/01_common.tpf.dcx (read via read_game_file_decompressed, so loose mod overlay
// first, then the packed dvdbnd). The pack decompresses to a ~194 MB PC TPF holding the
// SB_Icon_* sheets (BC7 / DX10, 4096x2048); this reads + decompresses it ONCE and copies out
// each requested sheet's DDS bytes, then frees the big buffer. Returns sheetName -> DDS blob
// (a full "DDS " file, ready for create_tex_from_dds_mem); a name absent from the pack is
// omitted from the map. `names` are bare sheet names from the layout's imagePath, e.g.
// {"SB_Icon_00", "SB_Icon_ERR_Gem_01"}. Empty when the pack can't be read. Heavy (decompress
// + copies) — call once off the engine thread. This is GAP #2: the no-bake item-icon pixels.
std::map<std::string, std::vector<uint8_t>>
read_item_icon_sheets(const std::vector<std::string> &names);
} // namespace goblin::worldmap
