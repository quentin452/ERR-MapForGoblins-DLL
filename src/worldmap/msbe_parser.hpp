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
    float       pos[3] = {0, 0, 0};  // BLOCK-LOCAL position (= bake x/z transform input)
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
};

struct ParseResult
{
    std::vector<Treasure> treasures;
    std::vector<Asset>    assets;   // AEG Asset placements (collectible candidates)
    std::vector<Enemy>    enemies;  // Enemy placements (enemy-drop candidates)
    bool ok = false;
};

// Parse a DECOMPRESSED MSB blob.
//   resident=false (disk): entry-internal offsets are entry-relative (added to entry start).
//   resident=true:         entry-internal offsets are absolute VAs; pass blobBase = VA of buf[0].
// wantAssets: also enumerate AEG Asset placements into ParseResult.assets (the
// runtime collectible source). Off by default (skips an extra PARTS pass).
// wantEnemies: also enumerate Enemy placements (type 2) into ParseResult.enemies
// (the runtime enemy-drop source). Off by default.
ParseResult parse_msb(const uint8_t *buf, size_t len, bool resident, uintptr_t blobBase = 0,
                      bool wantAssets = false, bool wantEnemies = false);

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

} // namespace goblin::msbe
