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
};

struct ParseResult
{
    std::vector<Treasure> treasures;
    bool ok = false;
};

// Parse a DECOMPRESSED MSB blob.
//   resident=false (disk): entry-internal offsets are entry-relative (added to entry start).
//   resident=true:         entry-internal offsets are absolute VAs; pass blobBase = VA of buf[0].
ParseResult parse_msb(const uint8_t *buf, size_t len, bool resident, uintptr_t blobBase = 0);

// DCX_DFLT (zlib/Deflate) -> raw MSB bytes. Empty on failure or unsupported. Sets *isKrak
// true when the DCX is Oodle/KRAK (decompress via the game's Oodle elsewhere).
std::vector<uint8_t> dcx_decompress(const uint8_t *dcx, size_t len, bool *isKrak = nullptr);

} // namespace goblin::msbe
