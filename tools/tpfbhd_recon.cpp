// Offline BHF4 (.tpfbhd/.tpfbdt) split-archive recon (Linux).
//
// Validates the map-tile-loading chain (endgame phase-1a) end-to-end WITHOUT the game:
//   BHF4 entry table -> seek .tpfbdt -> DCX blob -> dcx_decompress -> TPF -> named DDS -> dims.
// The .tpfbhd is the same format for 00_Solo (loose in ERR, the free de-risk sample) and 71_MapTile
// (packed in the base dvdbnd — read in-game). This proves the BHF4 parser + the reuse of the in-game
// msbe::dcx_decompress + tpf_find_texture, so the DLL reader only has to re-read the dvdbnd blob.
//
// BHF4 header (little-endian):
//   0x00 "BHF4" | 0x08 u32 0x10000 | 0x0C u32 fileCount | 0x10 u64 entriesStart(=0x40)
//   0x18 char[8] version | 0x20 u64 entryStride(=0x24)
// Entry (entryStride bytes, table at entriesStart):
//   +0x00 u32 rawFlags | +0x04 u32 -1 | +0x08 u64 compressedSize | +0x10 u64 uncompressedSize
//   +0x18 u32 dataOffset(into .tpfbdt) | +0x1c u32 fileId | +0x20 u32 nameOffset(.tpfbhd UTF-16LE null-term)
//
// Build: tools/build_tpfbhd_recon.sh
// Run:   ./tools/tpfbhd_recon <path.tpfbhd> [maxProbe=8] [nameFilter]
//   e.g. ./tools/tpfbhd_recon "$ERR_ROOT/mod/menu/hi/00_Solo.tpfbhd" 8
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <dlfcn.h>

#include "worldmap/msbe_parser.hpp"

using goblin::msbe::dcx_decompress;
using goblin::msbe::tpf_find_texture;
using goblin::msbe::OodleDecompressFn;

static const char *OODLE_LIB =
    "/home/iamacat/Games/ERRv2.2.9.6/internals/launcher/liboo2corelinux64.so.9";

static std::vector<uint8_t> slurp(const char *path)
{
    std::vector<uint8_t> v;
    FILE *f = std::fopen(path, "rb");
    if (!f) return v;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) { v.resize((size_t)n); if (std::fread(v.data(), 1, (size_t)n, f) != (size_t)n) v.clear(); }
    std::fclose(f);
    return v;
}

static uint32_t rd_u32(const uint8_t *p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
static uint64_t rd_u64(const uint8_t *p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

// UTF-16LE null-terminated ASCII-ish name at byte offset `off` in the header blob.
static std::string utf16_name(const std::vector<uint8_t> &bhd, uint32_t off)
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

// First UTF-16LE texture name in a TPF blob (>=3 printable chars) — a quick "what's inside" readout.
static std::string first_tpf_name(const uint8_t *b, size_t n)
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

int main(int argc, char **argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: %s <path.tpfbhd> [maxProbe=8] [nameFilter]\n", argv[0]); return 2; }
    std::string bhdPath = argv[1];
    int maxProbe = argc > 2 ? std::atoi(argv[2]) : 8;
    const char *filter = argc > 3 ? argv[3] : nullptr;
    std::string bdtPath = bhdPath.substr(0, bhdPath.size() - 6) + "tpfbdt";  // strip "tpfbhd" -> +"tpfbdt"

    void *h = dlopen(OODLE_LIB, RTLD_NOW | RTLD_GLOBAL);
    OodleDecompressFn oodle = nullptr;
    if (h) oodle = (OodleDecompressFn)dlsym(h, "OodleLZ_Decompress");
    std::printf("[%s] Oodle %s\n", oodle ? "ok" : "warn", oodle ? OODLE_LIB : "(unbound — KRAK entries will fail)");

    std::vector<uint8_t> bhd = slurp(bhdPath.c_str());
    if (bhd.size() < 0x40 || std::memcmp(bhd.data(), "BHF4", 4) != 0)
    { std::fprintf(stderr, "[ERR] not a BHF4 header: %s\n", bhdPath.c_str()); return 1; }

    uint32_t count = rd_u32(&bhd[0x0C]);
    uint64_t entriesStart = rd_u64(&bhd[0x10]);
    char ver[9] = {0}; std::memcpy(ver, &bhd[0x18], 8);
    uint64_t stride = rd_u64(&bhd[0x20]);
    std::printf("[hdr] count=%u entriesStart=0x%llx stride=0x%llx version=%s\n",
                count, (unsigned long long)entriesStart, (unsigned long long)stride, ver);
    if (stride < 0x24 || entriesStart + (uint64_t)count * stride > bhd.size())
    { std::fprintf(stderr, "[ERR] entry table out of range\n"); return 1; }

    FILE *bdt = std::fopen(bdtPath.c_str(), "rb");
    if (!bdt) { std::fprintf(stderr, "[ERR] open .tpfbdt: %s\n", bdtPath.c_str()); return 1; }

    std::printf("\n=== first %d matching entries (of %u) ===\n", maxProbe, count);
    int probed = 0, dcxOk = 0, tpfOk = 0;
    for (uint32_t i = 0; i < count && probed < maxProbe; ++i)
    {
        const uint8_t *e = &bhd[entriesStart + (uint64_t)i * stride];
        uint64_t csize = rd_u64(e + 0x08);
        uint32_t doff  = rd_u32(e + 0x18);
        uint32_t noff  = rd_u32(e + 0x20);
        std::string name = noff < bhd.size() ? utf16_name(bhd, noff) : "(bad nameOff)";
        if (filter && name.find(filter) == std::string::npos) continue;
        ++probed;

        std::printf("  [%u] name=%-46s csize=%llu doff=0x%llx", i, name.c_str(),
                    (unsigned long long)csize, (unsigned long long)doff);

        std::vector<uint8_t> blob(csize);
        std::fseek(bdt, (long)doff, SEEK_SET);
        if (std::fread(blob.data(), 1, csize, bdt) != csize) { std::printf("  [ERR read bdt]\n"); continue; }
        bool isKrak = false;
        bool isDcx = csize >= 4 && std::memcmp(blob.data(), "DCX\0", 4) == 0;
        std::vector<uint8_t> tpf = isDcx ? dcx_decompress(blob.data(), blob.size(), &isKrak, oodle) : blob;
        std::printf("  magic=%.3s%s", (const char *)blob.data(), isDcx ? "" : "(raw)");
        if (tpf.size() >= 4 && std::memcmp(tpf.data(), "TPF\0", 4) == 0)
        {
            ++dcxOk;
            std::string tn = first_tpf_name(tpf.data(), tpf.size());
            size_t off = 0, len = 0;
            bool hasDds = !tn.empty() && tpf_find_texture(tpf.data(), tpf.size(), tn.c_str(), off, len);
            uint32_t w = 0, ht = 0;
            if (hasDds && len >= 0x14 && std::memcmp(tpf.data() + off, "DDS ", 4) == 0)
            { ht = rd_u32(tpf.data() + off + 0x0C); w = rd_u32(tpf.data() + off + 0x10); ++tpfOk; }
            std::printf("  tpf=%zuB tex0=%s%s\n", tpf.size(), tn.c_str(),
                        (w ? (" " + std::to_string(w) + "x" + std::to_string(ht)).c_str() : ""));
        }
        else std::printf("  krak=%d out=%zuB (not TPF)\n", (int)isKrak, tpf.size());
    }
    std::fclose(bdt);
    std::printf("\n[done] probed=%d dcx->tpf=%d tpf->dds-dims=%d\n", probed, dcxOk, tpfOk);
    return 0;
}
