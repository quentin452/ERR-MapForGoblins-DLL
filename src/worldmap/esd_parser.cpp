#include "esd_parser.hpp"

#include <cstring>
#include <string>

// See esd_parser.hpp for the format map. Everything here is bounds-checked; a
// malformed blob yields an empty result, never UB — these bytes come straight
// from the active install's files (any mod), so treat them as untrusted input.
namespace goblin::esd
{
namespace
{
inline bool inb(size_t off, size_t need, size_t len) { return need <= len && off <= len - need; }

inline uint32_t rd32(const uint8_t *b, size_t o)
{
    return (uint32_t)b[o] | (uint32_t)b[o + 1] << 8 | (uint32_t)b[o + 2] << 16 |
           (uint32_t)b[o + 3] << 24;
}
inline int32_t rdi32(const uint8_t *b, size_t o) { return (int32_t)rd32(b, o); }
inline uint64_t rd64(const uint8_t *b, size_t o)
{
    return (uint64_t)rd32(b, o) | (uint64_t)rd32(b, o + 4) << 32;
}

// Sequential varint cursor: i64 in the long format (fsSL), i32 in the short (fSSL).
struct VarCur
{
    const uint8_t *b;
    size_t len, pos;
    bool lng, ok = true;
    int64_t next()
    {
        size_t w = lng ? 8u : 4u;
        if (!inb(pos, w, len)) { ok = false; return -1; }
        int64_t v = lng ? (int64_t)rd64(b, pos) : (int64_t)rdi32(b, pos);
        pos += w;
        return v;
    }
};

// Decode one EzState argument expression IF it is the literal push-int form
// `82 <int32 LE> A1`. The 0x82 opcode is push-int32, 0xA1 is end-of-expression
// (docs/re/esd_ezstate_decoder_re_findings.md). Anything else → false.
bool decode_literal_arg(const uint8_t *b, size_t len, size_t off, size_t size, int32_t &out)
{
    if (size != 6 || !inb(off, 6, len)) return false;
    if (b[off] != 0x82 || b[off + 5] != 0xA1) return false;
    out = rdi32(b, off + 1);
    return true;
}
} // namespace

std::vector<std::pair<int32_t, int32_t>> collect_literal_pairs(const uint8_t *buf, size_t len,
                                                               int bank, int id, int *skippedExpr)
{
    std::vector<std::pair<int32_t, int32_t>> out;
    if (!inb(0, 0x6c, len)) return out;
    bool lng;
    if (std::memcmp(buf, "fsSL", 4) == 0) lng = true;        // 64-bit varints (Elden Ring talk)
    else if (std::memcmp(buf, "fSSL", 4) == 0) lng = false;  // 32-bit varints
    else return out;
    // Fixed 26-i32 header after the magic (see hpp). Validate the load-bearing
    // constants loosely (version + the per-record sizes we depend on), read the counts.
    if (rd32(buf, 0x04) != 1) return out;
    const uint32_t stateGroupSize = rd32(buf, 0x24);
    const uint32_t stateGroupCount = rd32(buf, 0x28);
    const uint32_t stateSize = rd32(buf, 0x2c);
    const uint32_t stateCount = rd32(buf, 0x30);
    const uint32_t condSize = rd32(buf, 0x34);
    const uint32_t condCount = rd32(buf, 0x38);
    if (stateGroupSize != (lng ? 0x20u : 0x10u) || stateSize != (lng ? 0x48u : 0x24u) ||
        condSize != (lng ? 0x38u : 0x1cu))
        return out;
    // Cap the counts against the file size so a corrupt header can't spin the walk.
    if ((uint64_t)stateCount * stateSize > len || (uint64_t)condCount * condSize > len ||
        (uint64_t)stateGroupCount * stateGroupSize > len)
        return out;

    const size_t dataStart = 0x6c;
    // Skip the data-section preamble: 5 i32 (+1 pad i32 in long format), 6 varints.
    size_t pos = dataStart + 20 + (lng ? 4 : 0) + 6 * (lng ? 8 : 4);
    // Skip the state-group table (we enumerate ALL states sequentially instead).
    pos += (size_t)stateGroupCount * stateGroupSize;

    // One command list at dataStart+off: count × {bank i32, id i32, argsOff, argsCnt}.
    auto scan_cmd_list = [&](int64_t off, int64_t count)
    {
        if (off < 0 || count <= 0) return;
        size_t c = dataStart + (size_t)off;
        const size_t cmdSize = 8 + 2 * (lng ? 8u : 4u);
        for (int64_t i = 0; i < count; ++i, c += cmdSize)
        {
            if (!inb(c, cmdSize, len)) return;
            int cbank = rdi32(buf, c);
            int cid = rdi32(buf, c + 4);
            VarCur ac{buf, len, c + 8, lng};
            int64_t argsOff = ac.next();
            int64_t argsCnt = ac.next();
            if (!ac.ok || cbank != bank || cid != id) continue;
            if (argsCnt != 2 || argsOff < 0)
            {
                if (skippedExpr) ++*skippedExpr;
                continue;
            }
            VarCur at{buf, len, dataStart + (size_t)argsOff, lng};
            int64_t a0off = at.next(), a0sz = at.next();
            int64_t a1off = at.next(), a1sz = at.next();
            int32_t v0 = 0, v1 = 0;
            if (at.ok && a0off >= 0 && a1off >= 0 &&
                decode_literal_arg(buf, len, dataStart + (size_t)a0off, (size_t)a0sz, v0) &&
                decode_literal_arg(buf, len, dataStart + (size_t)a1off, (size_t)a1sz, v1))
                out.emplace_back(v0, v1);
            else if (skippedExpr)
                ++*skippedExpr;
        }
    };

    // States: 9 varints each; command lists at fields 3/4 (entry), 5/6 (exit), 7/8 (while).
    VarCur st{buf, len, pos, lng};
    for (uint32_t i = 0; i < stateCount && st.ok; ++i)
    {
        st.next();                                    // id
        st.next(); st.next();                         // condition offsets (skip)
        int64_t eo = st.next(), ec = st.next();       // entry commands
        int64_t xo = st.next(), xc = st.next();       // exit commands
        int64_t wo = st.next(), wc = st.next();       // while commands
        if (!st.ok) break;
        scan_cmd_list(eo, ec);
        scan_cmd_list(xo, xc);
        scan_cmd_list(wo, wc);
    }
    // Conditions (incl. every subcondition — they all live in this sequential block):
    // 7 varints each; the pass-command list is fields 1/2.
    VarCur cd{buf, len, st.pos, lng};
    for (uint32_t i = 0; i < condCount && cd.ok; ++i)
    {
        cd.next();                                    // target-state offset
        int64_t po = cd.next(), pc = cd.next();       // pass commands
        cd.next(); cd.next();                         // subcondition offsets (skip)
        cd.next(); cd.next();                         // evaluator (skip)
        if (!cd.ok) break;
        scan_cmd_list(po, pc);
    }
    return out;
}

std::vector<TalkShopRange> parse_talkbnd_shop_ranges(const uint8_t *buf, size_t len,
                                                     int *skippedExpr)
{
    std::vector<TalkShopRange> out;
    // BND4 walk — same entry layout as name_fmg_en.cpp's msgbnd reader (ER format
    // 0x74 entries: uncompressed size @+0x10, data offset @+0x18, name offset @+0x20).
    if (!inb(0, 0x40, len) || std::memcmp(buf, "BND4", 4) != 0) return out;
    int32_t fileCount = rdi32(buf, 0x0c);
    int64_t hdrSize = (int64_t)rd64(buf, 0x20);
    bool unicode = buf[0x30] != 0;  // BND4 name encoding flag (ER talk bnds = UTF-16)
    if (fileCount <= 0 || fileCount > 100000 || hdrSize < 0x24) return out;
    for (int32_t i = 0; i < fileCount; ++i)
    {
        size_t e = 0x40 + (size_t)i * (size_t)hdrSize;
        if (!inb(e, 0x24, len)) break;
        int64_t uncomp = (int64_t)rd64(buf, e + 0x10);
        int32_t dataOff = rdi32(buf, e + 0x18);
        int32_t nameOff = rdi32(buf, e + 0x20);
        if (dataOff < 0 || uncomp <= 0) continue;
        if ((size_t)dataOff + (size_t)uncomp > len) continue;

        // Entry basename after the last path separator, e.g. "t316006000.esd".
        std::string base;
        if (nameOff > 0 && (size_t)nameOff < len)
        {
            if (unicode)
                for (size_t j = (size_t)nameOff; j + 1 < len; j += 2)
                {
                    uint16_t c = (uint16_t)(buf[j] | (uint16_t)buf[j + 1] << 8);
                    if (c == 0) break;
                    if (c == '\\' || c == '/') base.clear();
                    else base.push_back((char)(c & 0xFF));
                }
            else
                for (size_t j = (size_t)nameOff; j < len && buf[j]; ++j)
                {
                    if (buf[j] == '\\' || buf[j] == '/') base.clear();
                    else base.push_back((char)buf[j]);
                }
        }
        if (base.size() < 6 || base[0] != 't') continue;
        if (base.compare(base.size() - 4, 4, ".esd") != 0) continue;
        uint32_t talkId = 0;
        bool digits = true;
        for (size_t j = 1; j + 4 < base.size(); ++j)
        {
            if (base[j] < '0' || base[j] > '9') { digits = false; break; }
            talkId = talkId * 10u + (uint32_t)(base[j] - '0');
        }
        if (!digits || talkId == 0) continue;

        for (auto [a, b] : collect_literal_pairs(buf + dataOff, (size_t)uncomp,
                                                 /*bank=*/1, /*id=*/22, skippedExpr))
            out.push_back(TalkShopRange{talkId, a, b});
    }
    return out;
}
} // namespace goblin::esd
