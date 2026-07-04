#include "maptile.hpp"

#include "loot_disk.hpp"    // read_game_file_decompressed
#include "msbe_parser.hpp"  // dcx_decompress, tpf_find_texture, OodleDecompressFn

#include <spdlog/spdlog.h>

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <map>

namespace goblin::worldmap::maptile
{
namespace
{
uint32_t rd_u32(const uint8_t *p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint64_t rd_u64(const uint8_t *p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

// Resolve the game's loaded Oodle for inner DCX_KRAK tiles (same approach as loot_disk::resolve_oodle,
// kept local so this module doesn't depend on loot_disk's file-private helper). Cached.
msbe::OodleDecompressFn oodle()
{
    static msbe::OodleDecompressFn fn = nullptr;
    static bool tried = false;
    if (tried) return fn;
    tried = true;
    HMODULE h = GetModuleHandleW(L"oo2core_6_win64.dll");
    if (!h) h = LoadLibraryW(L"oo2core_6_win64.dll");
    if (h) fn = (msbe::OodleDecompressFn)GetProcAddress(h, "OodleLZ_Decompress");
    return fn;
}

// UTF-16LE null-terminated (ASCII subset) name at byte offset `off` in the header blob.
std::string utf16_name(const std::vector<uint8_t> &bhd, uint32_t off)
{
    std::string s;
    for (size_t i = off; i + 1 < bhd.size(); i += 2)
    {
        uint8_t lo = bhd[i], hi = bhd[i + 1];
        if (lo == 0 && hi == 0) break;
        s.push_back(hi == 0 && lo >= 0x20 && lo < 0x7f ? (char)lo : '?');
    }
    return s;
}

// First UTF-16LE printable name (>=3 chars) in a TPF blob — the tile's texture name.
std::string first_tpf_name(const uint8_t *b, size_t n)
{
    std::string cur;
    for (size_t i = 0; i + 1 < n; i += 2)
    {
        uint8_t lo = b[i], hi = b[i + 1];
        if (hi == 0 && lo >= 0x20 && lo < 0x7f) { cur.push_back((char)lo); }
        else { if (cur.size() >= 3) return cur; cur.clear(); }
    }
    return cur.size() >= 3 ? cur : std::string();
}
} // namespace

bool parse_bhf4(const std::vector<uint8_t> &bhd, std::vector<Entry> &out)
{
    out.clear();
    if (bhd.size() < 0x40 || std::memcmp(bhd.data(), "BHF4", 4) != 0)
        return false;
    uint32_t count = rd_u32(&bhd[0x0C]);
    uint64_t entriesStart = rd_u64(&bhd[0x10]);
    uint64_t stride = rd_u64(&bhd[0x20]);
    if (stride < 0x24 || entriesStart + (uint64_t)count * stride > bhd.size())
    {
        spdlog::warn("[MAPTILE] BHF4 entry table out of range (count={} start=0x{:x} stride=0x{:x} size={})",
                     count, entriesStart, stride, bhd.size());
        return false;
    }
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint8_t *e = &bhd[entriesStart + (uint64_t)i * stride];
        Entry en;
        en.compressedSize = rd_u64(e + 0x08);
        en.dataOffset = rd_u32(e + 0x18);
        en.fileId = rd_u32(e + 0x1c);
        uint32_t noff = rd_u32(e + 0x20);
        en.name = noff < bhd.size() ? utf16_name(bhd, noff) : std::string();
        out.push_back(std::move(en));
    }
    return true;
}

bool grid_range(const std::vector<Entry> &entries, const std::string &prefix, GridRange &out)
{
    out = GridRange{};
    bool any = false;
    for (const Entry &e : entries)
    {
        size_t p = e.name.find(prefix);  // e.g. "M00_L0"
        if (p == std::string::npos) continue;
        size_t c0 = p + prefix.size();
        if (c0 >= e.name.size() || e.name[c0] != '_') continue;
        size_t c1 = e.name.find('_', c0 + 1);          // after col
        size_t r1 = (c1 == std::string::npos) ? std::string::npos : e.name.find('_', c1 + 1);  // after row
        if (c1 == std::string::npos || r1 == std::string::npos) continue;
        long col = std::strtol(e.name.substr(c0 + 1, c1 - c0 - 1).c_str(), nullptr, 16);
        long row = std::strtol(e.name.substr(c1 + 1, r1 - c1 - 1).c_str(), nullptr, 16);
        if (!any) { out.minCol = out.maxCol = (int)col; out.minRow = out.maxRow = (int)row; any = true; }
        else
        {
            if (col < out.minCol) out.minCol = (int)col;
            if (col > out.maxCol) out.maxCol = (int)col;
            if (row < out.minRow) out.minRow = (int)row;
            if (row > out.maxRow) out.maxRow = (int)row;
        }
        out.count++;
    }
    return any;
}

bool load_archive(const std::string &rel_base, std::vector<Entry> &entries, std::vector<uint8_t> &bdt)
{
    std::vector<uint8_t> bhd = read_game_file_decompressed(rel_base + ".tpfbhd");
    if (bhd.size() < 0x40)
    {
        spdlog::warn("[MAPTILE] header unavailable: {}.tpfbhd ({} bytes)", rel_base, bhd.size());
        return false;
    }
    if (!parse_bhf4(bhd, entries))
        return false;
    bdt = read_game_file_decompressed(rel_base + ".tpfbdt");
    if (bdt.size() < 4)
    {
        spdlog::warn("[MAPTILE] data unavailable: {}.tpfbdt ({} bytes)", rel_base, bdt.size());
        return false;
    }
    spdlog::info("[MAPTILE] loaded {} ({} entries, .tpfbdt {} bytes)", rel_base, entries.size(), bdt.size());
    return true;
}

std::vector<uint8_t> extract_dds(const std::vector<uint8_t> &bdt, const Entry &e, std::string &texName,
                                 uint32_t *w, uint32_t *h)
{
    texName.clear();
    if ((uint64_t)e.dataOffset + e.compressedSize > bdt.size() || e.compressedSize < 4)
        return {};
    const uint8_t *blob = bdt.data() + e.dataOffset;
    bool isDcx = std::memcmp(blob, "DCX\0", 4) == 0;
    std::vector<uint8_t> tpf;
    if (isDcx)
    {
        bool krak = false;
        tpf = msbe::dcx_decompress(blob, (size_t)e.compressedSize, &krak, oodle());
    }
    else
        tpf.assign(blob, blob + e.compressedSize);
    if (tpf.size() < 4 || std::memcmp(tpf.data(), "TPF\0", 4) != 0)
        return {};
    std::string tn = first_tpf_name(tpf.data(), tpf.size());
    if (tn.empty())
        return {};
    size_t off = 0, len = 0;
    if (!msbe::tpf_find_texture(tpf.data(), tpf.size(), tn.c_str(), off, len))
        return {};
    if (len < 0x14 || std::memcmp(tpf.data() + off, "DDS ", 4) != 0)
        return {};
    texName = tn;
    if (h) *h = rd_u32(tpf.data() + off + 0x0C);
    if (w) *w = rd_u32(tpf.data() + off + 0x10);
    return std::vector<uint8_t>(tpf.begin() + off, tpf.begin() + off + len);
}

std::vector<uint8_t> extract_named(const std::string &rel_base, const std::string &needle,
                                   std::string &texName, uint32_t *w, uint32_t *h)
{
    texName.clear();
    std::vector<Entry> entries;
    std::vector<uint8_t> bdt;
    if (!load_archive(rel_base, entries, bdt))
        return {};
    for (const Entry &e : entries)
        if (e.name.find(needle) != std::string::npos)
        {
            std::vector<uint8_t> dds = extract_dds(bdt, e, texName, w, h);
            spdlog::info("[MAPTILE] extract_named '{}' -> {} ({} DDS bytes)", needle,
                         texName.empty() ? "(fail)" : texName, dds.size());
            return dds;  // bdt frees here
        }
    spdlog::warn("[MAPTILE] extract_named: no entry matches '{}'", needle);
    return {};
}

std::string probe(const std::string &rel_base, int max_probe, const char *name_filter)
{
    std::vector<Entry> entries;
    std::vector<uint8_t> bdt;
    if (!load_archive(rel_base, entries, bdt))
        return "err maptile: archive unavailable (" + rel_base + ")";

    // Histogram of the M{MM}_L{L} dimension/LOD prefix over ALL entries (name-only, no decode) — tells
    // slice 3 how many dimensions (M) + LOD levels (L) exist and how many tiles per level (SRV budgeting).
    std::map<std::string, int> hist;
    for (const Entry &e : entries)
    {
        size_t p = e.name.find("Tile_M");  // the SECOND MapTile ("MENU_MapTile_M00_L0_…")
        if (p == std::string::npos) continue;
        p += 5;                            // -> at 'M' of M00
        size_t u1 = e.name.find('_', p);   // after M00
        size_t u2 = (u1 == std::string::npos) ? std::string::npos : e.name.find('_', u1 + 1);  // after L0
        if (u2 != std::string::npos)
            hist[e.name.substr(p, u2 - p)]++;  // e.g. "M00_L0"
    }
    std::string histstr;
    for (auto &kv : hist)
    {
        spdlog::info("[MAPTILE] prefix {} : {} tiles", kv.first, kv.second);
        histstr += " " + kv.first + "=" + std::to_string(kv.second);
    }

    int probed = 0, dds = 0;
    uint32_t w0 = 0, h0 = 0;
    std::string first_tex;
    for (const Entry &e : entries)
    {
        if (probed >= max_probe) break;
        if (name_filter && e.name.find(name_filter) == std::string::npos) continue;
        ++probed;
        std::string tex;
        uint32_t w = 0, hh = 0;
        std::vector<uint8_t> d = extract_dds(bdt, e, tex, &w, &hh);
        if (!d.empty())
        {
            ++dds;
            if (first_tex.empty()) { first_tex = tex; w0 = w; h0 = hh; }
        }
        spdlog::info("[MAPTILE] [{}] name={} csize={} doff=0x{:x} -> {} {}x{} ({} DDS bytes)", e.fileId,
                     e.name, e.compressedSize, e.dataOffset, tex.empty() ? "(fail)" : tex, w, hh, d.size());
    }
    return "ok maptile " + rel_base + " entries=" + std::to_string(entries.size()) + " probed=" +
           std::to_string(probed) + " dds_ok=" + std::to_string(dds) +
           (first_tex.empty() ? "" : " tex0=" + first_tex + " " + std::to_string(w0) + "x" + std::to_string(h0)) +
           " |" + histstr;
}
} // namespace goblin::worldmap::maptile
