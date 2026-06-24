// Offline KRAK (Oodle) MSBE verifier — NATIVE Linux, no Wine. dlopen's the native
// liboo2corelinux64.so.9 that ERR ships, OodleLZ_Decompress's a DCX_KRAK .msb.dcx, parses
// Treasure events. Same job as msbe_oodle_test.cpp (the Windows/Wine variant) but System-V
// ABI: plain extern-C OodleLZ_Decompress (no __stdcall), dlsym instead of GetProcAddress.
//   build/run: see tools/msbe_test/build_oodle.sh
#include <cstdint>
#include <cstdio>
#include <vector>

#include <dlfcn.h>

#include "worldmap/msbe_parser.hpp"

// System-V ABI OodleLZ_Decompress (same arg shape as the Windows __stdcall typedef).
typedef intptr_t (*OodleLZ_Decompress_t)(const void *src, intptr_t srcLen, void *dst,
                                         intptr_t dstLen, int fuzz, int crc, int verbose,
                                         void *dbase, intptr_t dsize, void *cb, void *cbctx,
                                         void *scratch, intptr_t scratchSize, int threadPhase);

static std::vector<uint8_t> slurp(const char *p)
{
    std::vector<uint8_t> v;
    FILE *f = std::fopen(p, "rb");
    if (!f) return v;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) { v.resize((size_t)n); if (std::fread(v.data(), 1, (size_t)n, f) != (size_t)n) v.clear(); }
    std::fclose(f);
    return v;
}

int main(int argc, char **argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: %s <liboo2corelinux64.so> <file.msb.dcx>\n", argv[0]); return 2; }

    void *oo = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!oo) { std::fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 1; }
    auto OodleLZ_Decompress = (OodleLZ_Decompress_t)dlsym(oo, "OodleLZ_Decompress");
    if (!OodleLZ_Decompress) { std::fprintf(stderr, "no OodleLZ_Decompress: %s\n", dlerror()); return 1; }

    std::vector<uint8_t> d = slurp(argv[2]);
    if (d.size() < 0x50 || d[0] != 'D' || d[1] != 'C' || d[2] != 'X' || d[3] != 0) { std::fprintf(stderr, "not DCX\n"); return 1; }

    auto be32 = [&](size_t o) {
        return (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 | (uint32_t)d[o + 3];
    };
    uint32_t uncomp = be32(0x1c);
    bool isKrak = d[0x24] == 'D' && d[0x25] == 'C' && d[0x26] == 'P' && d[0x27] == 0 &&
                  d[0x28] == 'K' && d[0x29] == 'R' && d[0x2a] == 'A' && d[0x2b] == 'K';
    std::printf("uncomp=%u krak=%d\n", uncomp, isKrak);
    if (!isKrak) { std::fprintf(stderr, "not KRAK (use msbe_test for DFLT)\n"); return 1; }

    size_t dca = SIZE_MAX;
    for (size_t i = 0x28; i + 4 <= d.size(); i++)
        if (d[i] == 'D' && d[i + 1] == 'C' && d[i + 2] == 'A' && d[i + 3] == 0) { dca = i; break; }
    if (dca == SIZE_MAX) { std::fprintf(stderr, "no DCA\n"); return 1; }
    size_t zoff = dca + 8;

    std::vector<uint8_t> raw(uncomp);
    intptr_t got = OodleLZ_Decompress(d.data() + zoff, (intptr_t)(d.size() - zoff), raw.data(),
                                      (intptr_t)uncomp, 1, 0, 0, nullptr, 0, nullptr, nullptr,
                                      nullptr, 0, 3 /* OodleLZ_Decode_Unthreaded */);
    if (got != (intptr_t)uncomp) { std::fprintf(stderr, "Oodle got=%lld want=%u\n", (long long)got, uncomp); return 1; }
    std::printf("Oodle OK %zu -> %u bytes\n", d.size() - zoff, uncomp);

    goblin::msbe::ParseResult r = goblin::msbe::parse_msb(raw.data(), raw.size(), false);
    if (!r.ok) { std::fprintf(stderr, "parse failed\n"); return 1; }
    int withPart = 0;
    for (const auto &t : r.treasures)
    {
        if (t.partIndex >= 0) ++withPart;
        std::printf("lot=%-10u part[%5d]='%s' pos=(%.1f,%.1f,%.1f)\n", t.itemLotId, t.partIndex,
                    t.partName.c_str(), t.pos[0], t.pos[1], t.pos[2]);
    }
    std::printf("== %zu treasures (%d with part) ==\n", r.treasures.size(), withPart);
    return 0;
}
