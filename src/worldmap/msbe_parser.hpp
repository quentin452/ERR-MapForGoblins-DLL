#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
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

struct ParseResult
{
    std::vector<Treasure> treasures;
    bool ok = false;
};

// Parse a DECOMPRESSED MSB blob.
//   resident=false (disk): entry-internal offsets are entry-relative (added to entry start).
//   resident=true:         entry-internal offsets are absolute VAs; pass blobBase = VA of buf[0].
ParseResult parse_msb(const uint8_t *buf, size_t len, bool resident, uintptr_t blobBase = 0);

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
