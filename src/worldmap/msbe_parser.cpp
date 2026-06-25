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
            uint64_t tdOff = rd64(buf, pe + 0x68);
            if (tdOff == 0) continue;
            size_t tdp = eio(tdOff, pe);
            if (!inb(tdp, 0x10, len)) continue;
            uint32_t npc = rd32(buf, tdp + 0x0c);
            if (npc == 0 || npc == 0xffffffffu) continue;
            size_t nm = eio(rd64(buf, pe + 0x00), pe);
            if (nm >= len) continue;
            Enemy en;
            en.name = rd_utf16(buf, nm, len);
            en.npcParamId = npc;
            en.pos[0] = rdf(buf, pe + 0x20);
            en.pos[1] = rdf(buf, pe + 0x24);
            en.pos[2] = rdf(buf, pe + 0x28);
            R.enemies.push_back(std::move(en));
        }
    }

    R.ok = true;
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
