#include "msbe_parser.hpp"

#include <cstring>

#include "stb_image.h" // stbi_zlib_decode_buffer (raw zlib inflate, already in tree)

namespace goblin::msbe {
namespace {

// --- little-endian, bounds-checked readers over the decompressed blob -------------------
inline bool inb(size_t off, size_t need, size_t len) { return need <= len && off <= len - need; }

inline uint32_t rd32(const uint8_t *b, size_t o)
{
    return (uint32_t)b[o] | (uint32_t)b[o + 1] << 8 | (uint32_t)b[o + 2] << 16 |
           (uint32_t)b[o + 3] << 24;
}
inline uint64_t rd64(const uint8_t *b, size_t o)
{
    return (uint64_t)rd32(b, o) | ((uint64_t)rd32(b, o + 4) << 32);
}
inline float rdf(const uint8_t *b, size_t o)
{
    uint32_t v = rd32(b, o);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
// UTF-16LE -> ASCII (MSB part/name strings are ASCII-range "AEG099_..."); non-ASCII -> '?'.
std::string rd_utf16(const uint8_t *b, size_t o, size_t len)
{
    std::string s;
    while (o + 1 < len)
    {
        uint16_t c = (uint16_t)b[o] | (uint16_t)b[o + 1] << 8;
        if (!c) break;
        s.push_back(c < 0x80 ? (char)c : '?');
        o += 2;
        if (s.size() > 256) break;
    }
    return s;
}

constexpr uint32_t EVENT_TYPE_TREASURE = 4;
constexpr int SEC_EVENT = 1; // PARAM section order: MODEL,EVENT,POINT,ROUTE,LAYER,PARTS
constexpr int SEC_PARTS = 5;
// MSBE PartsParam part-type (@ PART entry +0x0c). 2 = Enemy (the enemy-drop source).
constexpr int32_t PART_ENEMY = 2;

// Parse "AEG{A}_{B}..." -> A*1000+B (= AssetEnvironmentGeometryParam row id).
// 0 if the name isn't an AEG asset. e.g. "AEG099_821_9000" -> 99821.
inline uint32_t aeg_row_from_name(const std::string &n)
{
    if (n.size() < 8 || n.compare(0, 3, "AEG") != 0) return 0;
    size_t i = 3;
    uint32_t a = 0;
    while (i < n.size() && n[i] >= '0' && n[i] <= '9') { a = a * 10 + (n[i] - '0'); ++i; }
    if (i >= n.size() || n[i] != '_') return 0;
    ++i;
    uint32_t b = 0;
    bool any = false;
    while (i < n.size() && n[i] >= '0' && n[i] <= '9') { b = b * 10 + (n[i] - '0'); ++i; any = true; }
    if (!any) return 0;
    return a * 1000 + b;
}

} // namespace

ParseResult parse_msb(const uint8_t *buf, size_t len, bool resident, uintptr_t blobBase,
                      bool wantAssets, bool wantEnemies)
{
    ParseResult R;
    if (len < 0x10 || std::memcmp(buf, "MSB ", 4) != 0) return R;

    // Resolve an ENTRY-INTERNAL offset (entry name / typeData): entry-relative on disk,
    // absolute VA in the resident copy. PARAM-LEVEL offsets (entryOffset[], next) are plain
    // file offsets in both -> index buf directly.
    auto eio = [&](uint64_t val, size_t entryStart) -> size_t {
        return resident ? (size_t)(val - blobBase) : entryStart + (size_t)val;
    };

    struct Sec { uint32_t entries; size_t entryArr; };
    Sec secs[6] = {};
    size_t po = rd32(buf, 0x08); // header +0x08 = first PARAM offset (= 0x10)
    for (int s = 0; s < 6; s++)
    {
        if (!inb(po, 0x10, len)) return R;
        uint32_t offsetCount = rd32(buf, po + 4); // = entries + 1
        if (offsetCount == 0 || offsetCount > 1000000u) return R;
        uint32_t entries = offsetCount - 1;
        size_t entryArr = po + 0x10; // long entryOffset[entries], then long nextParamOffset
        if (!inb(entryArr, (size_t)entries * 8 + 8, len)) return R;
        secs[s] = {entries, entryArr};
        po = (size_t)rd64(buf, entryArr + (size_t)entries * 8); // nextParamOffset (file-abs)
    }

    const Sec &EV = secs[SEC_EVENT];
    const Sec &PT = secs[SEC_PARTS];

    for (uint32_t i = 0; i < EV.entries; i++)
    {
        size_t e = (size_t)rd64(buf, EV.entryArr + (size_t)i * 8);
        if (!inb(e, 0x28, len)) continue;
        if (rd32(buf, e + 0x0c) != EVENT_TYPE_TREASURE) continue;

        uint64_t tdOff = rd64(buf, e + 0x20);
        if (tdOff == 0) continue;
        size_t td = eio(tdOff, e);
        if (!inb(td, 0x14, len)) continue;

        Treasure t;
        t.itemLotId = rd32(buf, td + 0x10);
        if (t.itemLotId == 0 || t.itemLotId == 0xffffffffu) continue;

        uint32_t pidx = rd32(buf, td + 0x08);
        if (pidx != 0xffffffffu && pidx < PT.entries)
        {
            t.partIndex = (int32_t)pidx;
            size_t pe = (size_t)rd64(buf, PT.entryArr + (size_t)pidx * 8);
            if (inb(pe, 0x2c, len))
            {
                size_t nm = eio(rd64(buf, pe + 0x00), pe);
                if (nm < len) t.partName = rd_utf16(buf, nm, len);
                t.partType = (int32_t)rd32(buf, pe + 0x0c); // 13=Asset, 9=DummyAsset
                t.pos[0] = rdf(buf, pe + 0x20);
                t.pos[1] = rdf(buf, pe + 0x24);
                t.pos[2] = rdf(buf, pe + 0x28);
                // Entity sub-struct (ptr @ part+0x60, entry-relative on disk /
                // absolute VA resident): EntityID@+0x00, EntityGroupIDs[8]@+0x1c.
                // A type-9 DummyAsset with either set is reachable (kept by the
                // caller). Needs the +0x60..+0x68 u64 in bounds.
                if (inb(pe, 0x68, len))
                {
                    uint64_t entOff = rd64(buf, pe + 0x60);
                    if (entOff)
                    {
                        size_t ent = eio(entOff, pe);
                        if (inb(ent, 0x3c, len))
                        {
                            t.entityId = rd32(buf, ent + 0x00);
                            for (size_t k = 0x1c; k < 0x3c; k += 4)
                            {
                                uint32_t g = rd32(buf, ent + k);
                                if (g != 0 && g != 0xffffffffu) { t.entityGroup = true; break; }
                            }
                        }
                    }
                }
            }
        }
        R.treasures.push_back(std::move(t));
    }

    // Collectibles: enumerate every Asset part (type 13) named "AEG..." with its
    // block-local position. The caller resolves each aegRow ->
    // AssetEnvironmentGeometryParam.pickUpItemLotParamId -> ItemLotParam_map LIVE.
    if (wantAssets)
    {
        for (uint32_t i = 0; i < PT.entries; i++)
        {
            size_t pe = (size_t)rd64(buf, PT.entryArr + (size_t)i * 8);
            if (!inb(pe, 0x2c, len)) continue;
            if ((int32_t)rd32(buf, pe + 0x0c) != PART_ASSET) continue;
            size_t nm = eio(rd64(buf, pe + 0x00), pe);
            if (nm >= len) continue;
            std::string name = rd_utf16(buf, nm, len);
            uint32_t row = aeg_row_from_name(name);
            if (row == 0) continue;
            Asset a;
            a.name = std::move(name);
            a.aegRow = row;
            a.pos[0] = rdf(buf, pe + 0x20);
            a.pos[1] = rdf(buf, pe + 0x24);
            a.pos[2] = rdf(buf, pe + 0x28);
            R.assets.push_back(std::move(a));
        }
    }

    // Enemy drops: enumerate every Enemy part (type 2) with its NPCParamID + block-local
    // position. The caller resolves each npcParamId -> NpcParam.itemLotId_map/_enemy ->
    // ItemLotParam LIVE. NPCParamID lives in the Enemy typeData sub-struct: the u64 pointer
    // @ part+0x68 (entry-relative on disk / absolute VA resident, same eio() rule), then the
    // u32 @ +0x0c inside it. Pinned 26277/26277 vs SoulsFormats (probe_enemy_npc_offset.py).
    if (wantEnemies)
    {
        for (uint32_t i = 0; i < PT.entries; i++)
        {
            size_t pe = (size_t)rd64(buf, PT.entryArr + (size_t)i * 8);
            if (!inb(pe, 0x70, len)) continue;
            if ((int32_t)rd32(buf, pe + 0x0c) != PART_ENEMY) continue;
            uint32_t npc = 0;
            uint64_t tdOff = rd64(buf, pe + 0x68);
            if (tdOff)
            {
                size_t tdp = eio(tdOff, pe);
                if (inb(tdp, 0x10, len))
                {
                    uint32_t v = rd32(buf, tdp + 0x0c);
                    if (v != 0xffffffffu) npc = v;
                }
            }
            // EntityID: entity sub-struct ptr @ part+0x60, EntityID@+0x00 (same as the
            // treasure path). The EMEVD pass joins on this; an enemy with no npc lot but
            // a non-zero EntityID is still a valid EMEVD position anchor (e.g. a scripted
            // boss reward), so we keep the part when EITHER is set.
            uint32_t entityId = 0;
            uint64_t entOff = rd64(buf, pe + 0x60);
            if (entOff)
            {
                size_t ent = eio(entOff, pe);
                if (inb(ent, 0x04, len)) entityId = rd32(buf, ent + 0x00);
            }
            if (entityId == 0xffffffffu) entityId = 0;
            if (npc == 0 && entityId == 0) continue;
            size_t nm = eio(rd64(buf, pe + 0x00), pe);
            if (nm >= len) continue;
            Enemy en;
            en.name = rd_utf16(buf, nm, len);
            en.npcParamId = npc;
            en.entityId = entityId;
            en.pos[0] = rdf(buf, pe + 0x20);
            en.pos[1] = rdf(buf, pe + 0x24);
            en.pos[2] = rdf(buf, pe + 0x28);
            R.enemies.push_back(std::move(en));
        }
    }

    R.ok = true;
    return R;
}

// ── EMEVD parse (no-bake EMEVD loot pass, loot_emevd_drops) ───────────────────────────
// 13 template events that award an item lot, each with the (entity, lot) byte offsets into
// the event-init ArgData + the minimum ArgData length. Datamined + DarkScript3-verified in
// tools/extract_all_items.py:646 (TEMPLATE_EVENTS); the pure-bytes parse here matches
// SoulsFormats 500/500 over all ERR event files (tools/probe_emevd_format.py).
namespace {
struct EmevdTemplate { uint32_t eventId; uint16_t entityOff, lotOff, minLen; };
constexpr EmevdTemplate kEmevdTemplates[] = {
    {90005300, 8, 16, 20}, {90005301, 8, 16, 20},                   // scarab / enemy drops
    {90005860, 16, 24, 28}, {90005861, 16, 24, 28}, {90005880, 16, 24, 28}, // boss rewards
    {90005750, 8, 16, 20}, {90005753, 8, 16, 20},                   // NPC quest/dialog rewards
    {90005774, 8, 12, 16},                                          // NPC invasion (pseudo multi)
    {90005792, 20, 24, 28},                                         // hostile NPC defeat
    {90005632, 8, 16, 20},                                          // painting pickups
    {90005110, 8, 20, 24},                                          // great runes
    {90005390, 8, 28, 32},                                          // larval tears (boss morph)
    {90005555, 8, 12, 16},                                          // DLC forging / special
};
const EmevdTemplate *find_emevd_template(uint32_t eventId)
{
    for (const auto &t : kEmevdTemplates)
        if (t.eventId == eventId) return &t;
    return nullptr;
}
constexpr uint32_t EMEVD_INIT_BANK = 2000; // RunEvent / RunCommonEvent instruction bank
} // namespace

std::vector<EmevdAward> parse_emevd(const uint8_t *buf, size_t len)
{
    std::vector<EmevdAward> out;
    if (len < 0x80 || std::memcmp(buf, "EVD\0", 4) != 0) return out;

    // Header (64-bit EMEVD; all offsets are plain file offsets). Pinned in probe_emevd_format.py.
    uint64_t eventCount   = rd64(buf, 0x10);
    uint64_t eventsOff    = rd64(buf, 0x18);
    uint64_t instrTblOff  = rd64(buf, 0x28);
    uint64_t argsOff      = rd64(buf, 0x78);
    if (eventCount > 1000000u) return out;

    constexpr size_t EVENT_SZ = 0x30;  // {id@0, instrCount@8, instrOffset@10, ...}
    constexpr size_t INSTR_SZ = 0x20;  // {bank@0, id@4, argLen@8, argOffset@10(int32), ...}

    for (uint64_t i = 0; i < eventCount; ++i)
    {
        size_t e = (size_t)eventsOff + (size_t)i * EVENT_SZ;
        if (!inb(e, EVENT_SZ, len)) break;
        uint64_t instrCount  = rd64(buf, e + 0x08);
        uint64_t instrOffset = rd64(buf, e + 0x10);  // byte offset into the instruction table
        size_t base = (size_t)instrTblOff + (size_t)instrOffset;
        if (instrCount > 1000000u) continue;
        for (uint64_t j = 0; j < instrCount; ++j)
        {
            size_t ins = base + (size_t)j * INSTR_SZ;
            if (!inb(ins, INSTR_SZ, len)) break;
            if (rd32(buf, ins + 0x00) != EMEVD_INIT_BANK) continue;
            uint64_t argLen = rd64(buf, ins + 0x08);
            int32_t  argOff = (int32_t)rd32(buf, ins + 0x10);  // -1 when no args
            if (argOff < 0 || argLen < 8) continue;
            size_t a = (size_t)argsOff + (size_t)argOff;
            if (!inb(a, (size_t)argLen, len)) continue;
            uint32_t eventId = rd32(buf, a + 4);  // arg[4..8] = the invoked event id
            const EmevdTemplate *t = find_emevd_template(eventId);
            if (!t) continue;
            if (argLen < t->minLen) continue;
            uint32_t entity = rd32(buf, a + t->entityOff);
            uint32_t lot    = rd32(buf, a + t->lotOff);
            if ((int32_t)lot > 0 && (int32_t)entity > 0)
                out.push_back({entity, lot});
        }
    }
    return out;
}

EmevdParse parse_emevd_full(const uint8_t *buf, size_t len)
{
    EmevdParse R;
    if (len < 0x80 || std::memcmp(buf, "EVD\0", 4) != 0) return R;

    uint64_t eventCount  = rd64(buf, 0x10);
    uint64_t eventsOff   = rd64(buf, 0x18);
    uint64_t instrTblOff = rd64(buf, 0x28);
    uint64_t argsOff     = rd64(buf, 0x78);
    if (eventCount > 1000000u) return R;
    constexpr size_t EVENT_SZ = 0x30, INSTR_SZ = 0x20;
    // entity-id range filter for setter-event candidates (the caller intersects with the
    // real MSB enemy entities, so this only needs to drop obvious non-entities: flags,
    // small enums, item ids stay possible but get filtered out by the intersection).
    constexpr uint32_t kEntMin = 1000u, kEntMax = 0x7fffffffu;

    for (uint64_t i = 0; i < eventCount; ++i)
    {
        size_t e = (size_t)eventsOff + (size_t)i * EVENT_SZ;
        if (!inb(e, EVENT_SZ, len)) break;
        uint64_t instrCount  = rd64(buf, e + 0x08);
        uint64_t instrOffset = rd64(buf, e + 0x10);
        size_t base = (size_t)instrTblOff + (size_t)instrOffset;
        if (instrCount > 1000000u) continue;

        std::vector<uint32_t> setFlags;     // SetEventFlag(.,1) in this event
        std::vector<uint32_t> candidates;   // entity-range values referenced in this event
        for (uint64_t j = 0; j < instrCount; ++j)
        {
            size_t ins = base + (size_t)j * INSTR_SZ;
            if (!inb(ins, INSTR_SZ, len)) break;
            uint32_t bank = rd32(buf, ins + 0x00);
            uint32_t iid  = rd32(buf, ins + 0x04);
            uint64_t argLen = rd64(buf, ins + 0x08);
            int32_t  argOff = (int32_t)rd32(buf, ins + 0x10);
            if (argOff < 0 || argLen < 4) continue;
            size_t a = (size_t)argsOff + (size_t)argOff;
            if (!inb(a, (size_t)argLen, len)) continue;

            // candidate entity values: every 4-byte window in this instruction's args
            for (size_t k = 0; k + 4 <= (size_t)argLen; k += 4)
            {
                uint32_t v = rd32(buf, a + k);
                if (v >= kEntMin && v <= kEntMax) candidates.push_back(v);
            }
            if (argLen < 8) continue;
            uint32_t eventId = rd32(buf, a + 4);
            if (bank == EMEVD_INIT_BANK)
            {
                // mechanism A: direct template award
                if (const EmevdTemplate *t = find_emevd_template(eventId))
                {
                    if (argLen >= t->minLen)
                    {
                        uint32_t entity = rd32(buf, a + t->entityOff);
                        uint32_t lot    = rd32(buf, a + t->lotOff);
                        if ((int32_t)lot > 0 && (int32_t)entity > 0)
                            R.direct.push_back({entity, lot});
                    }
                }
                // mechanism B input: RunEvent(2000:00) binding flag→lot via callee 1200
                if (iid == 0 && eventId == 1200 && argLen >= 16)
                {
                    uint32_t flag = rd32(buf, a + 8);
                    uint32_t lot  = rd32(buf, a + 12);
                    if ((int32_t)flag > 0 && (int32_t)lot > 0)
                        R.runEvent1200.push_back({flag, lot});
                }
            }
            // mechanism B input: SetEventFlag(2003:66) / SetNetworkEventFlag(2003:69), state==1
            else if (bank == 2003 && (iid == 66 || iid == 69) && argLen >= 12)
            {
                uint32_t flag  = rd32(buf, a + 4);
                int32_t  state = (int32_t)rd32(buf, a + 8);
                if (state == 1 && (int32_t)flag > 0) setFlags.push_back(flag);
            }
        }
        if (!setFlags.empty())
            R.setters.push_back({std::move(setFlags), std::move(candidates)});
    }
    return R;
}

std::vector<uint8_t> dcx_decompress(const uint8_t *d, size_t len, bool *isKrak,
                                    OodleDecompressFn oodle)
{
    if (isKrak) *isKrak = false;
    std::vector<uint8_t> out;
    if (len < 0x50 || std::memcmp(d, "DCX\0", 4) != 0) return out;
    if (std::memcmp(d + 0x18, "DCS\0", 4) != 0) return out;

    auto be32 = [&](size_t o) {
        return (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 |
               (uint32_t)d[o + 3];
    };
    uint32_t uncomp = be32(0x1c); // DCS +4 = uncompressed size (big-endian)

    // Format block: "DCP\0" tag @0x24, 4-char format @0x28 ("DFLT" zlib | "KRAK" Oodle).
    bool krak = false;
    if (std::memcmp(d + 0x24, "DCP\0", 4) == 0)
    {
        if (std::memcmp(d + 0x28, "KRAK", 4) == 0) krak = true;
        else if (std::memcmp(d + 0x28, "DFLT", 4) != 0) return out;
    }
    if (krak && isKrak) *isKrak = true;

    // Compressed stream starts at find("DCA\0") + 8 (DFLT zlib or KRAK Oodle alike).
    size_t dca = SIZE_MAX;
    for (size_t i = 0x28; i + 4 <= len; i++)
        if (d[i] == 'D' && d[i + 1] == 'C' && d[i + 2] == 'A' && d[i + 3] == 0) { dca = i; break; }
    if (dca == SIZE_MAX) return out;
    size_t zoff = dca + 8;
    if (zoff >= len || uncomp == 0 || uncomp > 256u * 1024 * 1024) return out;

    out.resize(uncomp);
    if (krak)
    {
        // KRAK needs the game's Oodle; without it the caller skips this map (isKrak set).
        if (!oodle) { out.clear(); return out; }
        long long got = oodle(d + zoff, (long long)(len - zoff), out.data(), (long long)uncomp,
                              /*fuzz=*/1, /*crc=*/0, /*verbose=*/0, nullptr, 0, nullptr, nullptr,
                              nullptr, 0, /*threadPhase=*/3 /* OodleLZ_Decode_Unthreaded */);
        if (got != (long long)uncomp) { out.clear(); return out; }
        return out;
    }

    int got = stbi_zlib_decode_buffer((char *)out.data(), (int)uncomp,
                                      (const char *)(d + zoff), (int)(len - zoff));
    if (got <= 0) { out.clear(); return out; }
    out.resize((size_t)got);
    return out;
}

} // namespace goblin::msbe
