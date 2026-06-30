#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// MSBE (.msb) parser — sources loot placement (Treasure events) straight from the active
// mod's real map files, no committed bake. Chain (RE'd live + disk-validated, exact-match to
// items_database.json, see docs/re/windows_resident_msbe_layout_re_findings.md):
//   Events.Treasure(type==4) -> {partIndex@td+0x08, itemLotId@td+0x10}
//     -> PARTS[partIndex] -> {nameOffset@+0x00, position vec3@+0x20}
//     -> join live ItemLotParam (resolve_loot_item_textid).
// Two offset bases: disk .msb (entry-internal offsets ENTRY-RELATIVE) vs resident RAM blob
// (relocated to ABSOLUTE VAs). DCX wrapper: ERR loose maps = DCX_DFLT (zlib); KRAK = Oodle.
namespace goblin::msbe {

struct Treasure
{
    uint32_t    itemLotId = 0;    // ItemLotParam_map row id (lotType 1)
    int32_t     partIndex = -1;   // index into PARTS; -1 = 0xFFFFFFFF (item-glow / no part)
    std::string partName;         // e.g. "AEG099_990_9002" ("" when partIndex < 0)
    float       pos[3] = {0, 0, 0}; // BLOCK-LOCAL position (valid only when partName set)
    // MSBE part-type enum at PART entry +0x0c (validated on disk vs the offline
    // unreachable list): 13 = Asset (reachable placement), 9 = DummyAsset
    // (disabled/cut placeholder — 305/312 of the pipeline's "unreachable" lots).
    // -1 when partIndex < 0. The caller drops INERT DummyAsset placements (those
    // with no entity binding); it KEEPS reachable ones (see entityId/entityGroup).
    int32_t     partType = -1;
    // Entity binding, read from the part's entity sub-struct (pointer @ PART
    // entry +0x60; EntityID@sub+0x00, EntityGroupIDs[8]@sub+0x1c..+0x38; offsets
    // pinned over all ERR maps, see docs/re/windows_msbe_dummyasset_unreachable_re_findings.md).
    // A DummyAsset (type 9) with a non-zero EntityID OR any non-zero
    // EntityGroupID is "reachable" — an EMEVD can activate the placement, so it's
    // a real pickup (the offline pipeline's criterion #3: dummy-only-no-Entity =
    // unreachable). Exactly 3 such lots across ERR; the caller KEEPS them instead
    // of dropping, recovering reachable_dummy loot without the bake. 0/false when
    // partIndex < 0 or the part has no entity sub-struct.
    uint32_t    entityId = 0;
    bool        entityGroup = false;  // any EntityGroupIDs[i] set (!=0, !=-1)
};

// MSBE PartsParam part-type enum values (subset). A Treasure bound to a
// DummyAsset is a structurally-inert / cut placement the player can't reach.
constexpr int32_t PART_ASSET = 13;
constexpr int32_t PART_DUMMY_ASSET = 9;

// A placed Asset part (PartsParam type 13) whose name starts with "AEG" — the
// runtime collectible source. The item it gives is resolved LIVE from
// AssetEnvironmentGeometryParam[aegRow].pickUpItemLotParamId -> ItemLotParam_map
// (no bake, no manual model->item table). aegRow is parsed from the name
// "AEG{A}_{B}_..." as A*1000 + B (e.g. AEG099_821 -> 99821).
struct Asset
{
    std::string name;            // e.g. "AEG099_821_9000"
    uint32_t    aegRow = 0;      // A*1000+B, = AssetEnvironmentGeometryParam row id
    // The part's ACTUAL model name. ERR sometimes substitutes a gather node's model while
    // keeping its vanilla NAME (part "AEG099_753_9000" instantiating DLC model "AEG463_860").
    // The game writes GeomFlagSaveData (GEOF) entries under the ACTUAL model's hash, so
    // collected-graying must bucket by THIS, not the name prefix. Read from the part's
    // modelIndex (u32 @ part+0x14) -> MODEL section name list. Empty if the index is OOB.
    // Offset pinned 11415/11415 on m12_02 + m60_40_52 vs SoulsFormats (probe_modelindex.py).
    std::string modelName;       // e.g. "AEG463_860" (== name prefix for non-substituted)
    float       pos[3] = {0, 0, 0};  // BLOCK-LOCAL position (= bake x/z transform input)
    // EntityID (entity sub-struct @ part+0x60, EntityID@+0x00 — same offset as the
    // treasure/enemy paths). 0 when unset. The World-feature disk pass uses it to split
    // INTERACTIVE asset features (Imp Statue seals, Hero's Tomb statues carry an EntityID)
    // from non-interactive DECORATION (the same model placed without one) — see
    // build_disk_world_feature_markers / world_feature_assets entity_required.
    uint32_t    entityId = 0;
};

// A placed Enemy part (PartsParam type 2) — the no-bake source for enemy-drop loot.
// The drop is resolved LIVE: npcParamId -> NpcParam.itemLotId_map (pref) / itemLotId_enemy
// -> ItemLotParam (no bake). Position = the enemy part's block-local pos (same transform as
// treasures). Chain pinned over all ERR maps, see docs/re/windows_enemy_loot_nobake_analysis.md
// + memory msbe-enemy-loot-offsets.
struct Enemy
{
    std::string name;            // e.g. "c3670_9000" (the ChrIns placement)
    uint32_t    npcParamId = 0;  // = *(u32)( *(u64)(part+0x68) + 0x0c )  (NpcParam row id)
    float       pos[3] = {0, 0, 0};  // BLOCK-LOCAL position (part+0x20)
    // EntityID (entity sub-struct @ part+0x60, EntityID@+0x00 — same offset as the
    // treasure path). 0 when unset. This is the anchor the EMEVD pass joins on: an
    // EMEVD template award co-carries (entity_id, lot_id), and the enemy part with
    // that EntityID gives the world position. See parse_emevd / loot_emevd_drops.
    uint32_t    entityId = 0;
};

// One EMEVD template-award reference: a bank-2000 event init whose eventId is a known
// item-award template carries (entity_id, lot_id) at fixed arg offsets. The lot is an
// ItemLotParam_map row (lotType 1); the entity_id resolves to an MSB Enemy part's
// position (the marker location). The 13 templates + their (entity,lot) arg offsets are
// datamined in tools/extract_all_items.py:646 (validated 500/500 vs SoulsFormats by
// tools/probe_emevd_format.py). Both > 0 (filtered). See docs/re/windows_enemy_loot_nobake_analysis.md §5b.
struct EmevdAward
{
    uint32_t entityId = 0;  // MSB Enemy EntityID this award is positioned by
    uint32_t lotId    = 0;  // ItemLotParam_map row id (lotType 1) awarded
    // Loose-anchor fallback (direct template awards only): the init's other entity-range 4-byte
    // windows, ordered by nearness to the lot arg. Many ERR template inits carry the documented
    // `entityId` as a NON-positionable id (a logic/flag entity), while the real placeable anchor —
    // an MSB Enemy OR Asset — sits at a different arg. When `entityId` doesn't resolve to a position,
    // the caller (build_disk_emevd_markers) walks `anchors` for the nearest one that does. Empty for
    // perTile awards (those stay strictly entity@8→ENEMY to avoid the asset-chest over-match).
    std::vector<uint32_t> anchors;
};

// One EMEVD event that SetEventFlag(flag,1)s — the "event-1200" boss-drop mechanism.
// `common.emevd` ev0 binds a trigger flag to an ItemLotParam_enemy lot via
// RunEvent(1200,[flag,lot]); a per-map event then SetEventFlag(flag,1) on boss death. The
// boss's MSB EntityID is referenced somewhere in that same event, so `candidates` carries
// every entity-range value the event references; the caller intersects it with the known
// MSB enemy entities (boss-preferred) to get the position. See loot_disk::load_emevd_awards
// + docs/re/windows_enemy_loot_nobake_analysis.md §5b (mechanism B).
struct EmevdSetter
{
    std::vector<uint32_t> flags;       // flags this event SetEventFlag(., state=1)
    std::vector<uint32_t> candidates;  // entity-range int32 values referenced in the event
    // The tile (area<<16 | gx<<8 | gz) of the emevd file this setter came from, stamped by
    // load_emevd_awards from the filename (parse_emevd_full has no filename). 0 if the file
    // isn't a single tile (e.g. the overworld-common "m60.emevd.dcx"). The caller resolves the
    // boss entity from `candidates` intersected with THIS tile's MSB enemies only — a global
    // intersection mis-picks a numerically-lower boss-like EntityID from a DIFFERENT dungeon
    // that merely appears as a 4-byte window in this event (the per-dungeon Rune Piece bug).
    uint32_t mapTile = 0;
};

// A boss-reward (90005860/61/80) init: the defeat flag, the awarded base lot, and the EXPLICIT
// boss entity (arg X8_4 @16). entity is 0 when the arg is absent/zero. See EmevdParse::bossFlagLot.
struct BossFlagLot
{
    uint32_t flag   = 0;  // defeatFlag (X0_4 @8) — gray flag comes from the lot's getItemFlagId, not this
    uint32_t lot    = 0;  // baseLot (X16_4 @24) — piece = base+1/+2
    uint32_t entity = 0;  // bossEntity (X8_4 @16) — explicit boss MSB EntityID (position fallback)
};

// Full EMEVD parse: direct template awards (mechanism A, the shipped pass) PLUS the
// event-1200 inputs (mechanism B). RunEvent(2000:00) inits with callee 1200 give (flag,lot);
// the setters give (flag-set, entity candidates). The caller resolves B by joining
// flag→lot (from common.emevd) with the setter's candidate entities.
struct EmevdParse
{
    std::vector<EmevdAward>                       direct;        // (entity, lot) — mechanism A
    std::vector<std::pair<uint32_t, uint32_t>>    runEvent1200;  // (flag, lot) — RunEvent(1200)
    std::vector<EmevdSetter>                      setters;       // SetEventFlag(.,1) events
    // Boss-reward templates 90005860/61/80: init args (defeatFlag@8 = X0_4, bossEntity@16 = X8_4,
    // baseLot@24 = X16_4). We parse all three. The caller resolves the boss position from defeatFlag
    // first (Reforged convention: defeatFlag == the boss's MSB EntityID for ~83/92 binds), then falls
    // back to the EXPLICIT bossEntity@16 (what the offline bake reads — extract_all_items.py:651), then
    // to a map-scoped setter-candidate join. The @16 fallback recovers the 9 overworld field-boss binds
    // where defeatFlag != EntityID (c4980/c3150/c4950 + the cross-tile c4503), which the flag-only path
    // left baked (tools/_probe_boss_piece_entity.py: 9 ENTITY-only). Collected from EVERY map's emevd
    // (the binds are per-map, unlike RunEvent(1200) which is common-only); then walk the baseLot
    // ItemLotParam chain (piece = baseLot+1/+2) for the Rune/Ember Piece.
    std::vector<BossFlagLot>                      bossFlagLot;   // (defeatFlag, baseLot, bossEntity) — 90005860/61/80
    // Per-tile "enemy-death item award" inits: a bank-2000 InitializeEvent whose callee is a per-tile
    // template (eventId >= 1e9, NOT a kEmevdTemplate) that awards an ItemLotParam when an enemy dies.
    // Layout (verified on the 12 uncovered Golden Runes + tools/_probe_emevd_precise.py): entity@X0_4
    // (a+8) = the MSB enemy; lot@idx(n-2) (a+argLen-8) = the awarded lot. The downstream entity→disk-
    // ENEMY join enforces "positionable enemy" (asset-entity chests don't join → excluded, which is
    // what keeps this off the 395-chest over-match), and the lot-coverage dedup drops any lot another
    // pass already placed. Treated as a lotType-1 direct award by the caller.
    std::vector<EmevdAward>                       perTileEnemyAward;
};

// A placed Region (PointParam / POINT section) — the no-bake Spirit Springs source. The POINT
// section is secs[2] (order MODEL,EVENT,POINT,ROUTE,LAYER,PARTS). Entry layout pinned vs
// SoulsFormats over 651 tiles (tools/probe_region_layout.py): name@+0x00 (offset), subtype@+0x08
// (i32), position@+0x14 (Vec3 — NOTE: ≠ Parts' +0x20). Only the spirit-spring-relevant regions
// are emitted: MountJump (subtype 46) + LockedMountJump (54) launch points, and Others (-1) whose
// name contains "FakeSpiritSpringJump" (ERR's manual springs).
struct Region
{
    std::string name;            // region name (FakeSpiritSpring filter / diag)
    int32_t     subtype = 0;     // +0x08: 46=MountJump, 54=LockedMountJump, -1=Others
    float       pos[3] = {0, 0, 0};  // +0x14 BLOCK-LOCAL position
};

// One EMEVD gesture-spawn reference (the no-bake Loot - Gestures source). Common template
// 90005570 registers a gesture pickup: args = [_, tmpl, flag@idx2, gestureParam@idx3,
// entity@idx4, ...]. The flag is the gesture-learned event flag (graying); the gestureParam is
// a GestureParam row id whose itemId field (read LIVE) gives the gesture's goods name; the
// entity resolves to an MSB Asset position. See parse_emevd_gestures + build_disk_gesture_markers.
struct GestureRef
{
    uint32_t entityId     = 0;  // MSB Asset EntityID this gesture is positioned by
    uint32_t flag         = 0;  // gesture-learned event flag (graying / census)
    uint32_t gestureParam = 0;  // GestureParam row id → itemId (live) → goods name
};

struct ParseResult
{
    std::vector<Treasure> treasures;
    std::vector<Asset>    assets;   // AEG Asset placements (collectible candidates)
    std::vector<Enemy>    enemies;  // Enemy placements (enemy-drop candidates)
    std::vector<Region>   regions;  // Spirit-spring POINT regions (MountJump/Locked/FakeSpiring)
    bool ok = false;
};

// Parse a DECOMPRESSED MSB blob.
//   resident=false (disk): entry-internal offsets are entry-relative (added to entry start).
//   resident=true:         entry-internal offsets are absolute VAs; pass blobBase = VA of buf[0].
// wantAssets: also enumerate AEG Asset placements into ParseResult.assets (the
// runtime collectible source). Off by default (skips an extra PARTS pass).
// wantEnemies: also enumerate Enemy placements (type 2) into ParseResult.enemies
// (the runtime enemy-drop source). Off by default.
// wantRegions: also enumerate the spirit-spring POINT regions (MountJump/Locked/FakeSpiring)
// into ParseResult.regions. Off by default (skips an extra POINT-section pass).
// crossTileAssets: accept Asset part names carrying a cross-tile LOD prefix
// ("m{AA}_{BB}_{CC}_{LOD}-AEG…", e.g. the Snow Town statues in a supertile) — off by default so the
// _00 passes keep the strict start-anchored model parse (a handful of _00 assets carry such a prefix
// and must NOT newly emit). Only the non-_00 LOD feature-asset scan opts in. See load_lod_feature_assets.
ParseResult parse_msb(const uint8_t *buf, size_t len, bool resident, uintptr_t blobBase = 0,
                      bool wantAssets = false, bool wantEnemies = false, bool wantRegions = false,
                      bool crossTileAssets = false);

// Parse a DECOMPRESSED EMEVD (.emevd) blob and return every template item-award
// reference (bank-2000 event init whose eventId is one of the 13 award templates),
// already filtered to entity_id > 0 && lot_id > 0. The 64-bit EMEVD layout is pinned in
// tools/probe_emevd_format.py (eventCount@0x10, eventsOffset@0x18, instrTableOffset@0x28,
// argsOffset@0x78; event stride 0x30, instruction stride 0x20). The caller joins each
// EntityID to an MSB Enemy position (Enemy::entityId) to place the marker. Empty on any
// malformed blob.
std::vector<EmevdAward> parse_emevd(const uint8_t *buf, size_t len);

// Richer EMEVD parse for the event-1200 recovery (mechanism B) on top of the direct awards
// (mechanism A). Same pinned layout as parse_emevd. Returns direct awards, RunEvent(1200)
// (flag,lot) pairs, and per-event SetEventFlag(.,1) records with their entity candidates.
EmevdParse parse_emevd_full(const uint8_t *buf, size_t len);

// World-feature graying: a bank-2000 init whose eventId is a FLAG template (Hero's Tomb
// statue 90005683) carries (entity, FLAG) — the activated-state event flag, NOT an item lot.
// Returns (entity -> flag), both filtered > 0. The World-feature disk pass joins an
// interactive asset's EntityID to its flag → marker textDisableFlagId1 (an activated statue
// grays/hides like the bake). Same pinned 64-bit layout as parse_emevd. Empty on malformed.
std::vector<std::pair<uint32_t, uint32_t>> parse_emevd_flag_awards(const uint8_t *buf, size_t len);

// World-feature Paintings: a bank-2000 init that registers a painting collection (flag in
// 580000-580199) carries (entity, FLAG). Detected by the flag RANGE, not a fixed template, since
// the DLC paintings each use a unique map-specific template id (e.g. 2045432550). Two arg shapes
// (matches tools/generate_paintings.py): template 90005632 → flag@idx2/entity@idx3 (Asset
// paintings); else flag@idx3 (if in range)/entity@idx4 (ghost-painter Enemy paintings). Returns
// (entity -> flag), both > 0. The disk pass joins the entity to its MSB Asset or Enemy position.
std::vector<std::pair<uint32_t, uint32_t>> parse_emevd_paintings(const uint8_t *buf, size_t len);

// World-feature Gestures: a bank-2000 init of common template 90005570 (ER's gesture-spawn)
// carries (flag, gestureParam, entity) at idx2/idx3/idx4. Returns one GestureRef per call with
// entity > 0; the disk pass joins the entity to its MSB Asset position and reads the gesture's
// name LIVE from GestureParam[gestureParam].itemId. Same pinned 64-bit layout. Empty on malformed.
std::vector<GestureRef> parse_emevd_gestures(const uint8_t *buf, size_t len);

// oo2core's OodleLZ_Decompress (14-arg; x64 ABI unifies __stdcall/__fastcall). Kept as a
// plain function-pointer typedef so msbe_parser needs no <windows.h>/oo2core link — the DLL
// resolves the real export (GetProcAddress) and passes it in; offline tools can too.
using OodleDecompressFn = long long (*)(const void *src, long long srcLen, void *dst,
                                        long long dstLen, int fuzz, int crc, int verbose,
                                        void *dbase, long long dsize, void *cb, void *cbctx,
                                        void *scratch, long long scratchSize, int threadPhase);

// DCX -> raw MSB bytes. DCX_DFLT (zlib/Deflate) is decompressed in-tree (stb). DCX_KRAK
// (Oodle) needs `oodle`: when given, the KRAK stream is decompressed via it; when null, the
// call sets *isKrak true and returns empty (caller skips / counts it). Empty on any failure.
std::vector<uint8_t> dcx_decompress(const uint8_t *dcx, size_t len, bool *isKrak = nullptr,
                                    OodleDecompressFn oodle = nullptr);

// Find a named texture in a decompressed PC TPF buffer (the menu item-icon atlas
// menu/hi/01_common.tpf — magic "TPF\0", platform PC, encoding 1 = UTF-16 texture names)
// and set ddsOff/ddsLen to that texture's DDS slice within `buf` (a full "DDS " file, the
// SB_Icon_* sheets are BC7 / DX10-header). `name` is ASCII (e.g. "SB_Icon_00"); the TPF's
// UTF-16 name is compared case-sensitively. Returns false on a name miss, a malformed/short
// TPF, a non-PC platform, or a per-entry-DCX texture (flags1 2/3 = DCP_EDGE — the menu icon
// sheets are stored raw, flags1 0, so this isn't supported). The TPF format is from
// SoulsFormats TPF.cs (PC Texture = u32 off, i32 size, byte fmt/type/mips/flags1, u32
// nameOff, i32 hasFloatStruct[+FloatStruct]); validated byte-exact extracting SB_Icon_00.
bool tpf_find_texture(const uint8_t *buf, size_t n, const char *name, size_t &ddsOff,
                      size_t &ddsLen);

#ifdef PARSER_COVERAGE
void start_coverage(const uint8_t *buf, size_t len);
std::vector<uint8_t> get_coverage();
void stop_coverage();
#endif

} // namespace goblin::msbe
