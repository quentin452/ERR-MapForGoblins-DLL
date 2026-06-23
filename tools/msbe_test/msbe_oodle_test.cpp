// Offline KRAK (Oodle) MSBE verifier — Windows exe run under Wine. Loads the REAL
// oo2core_6_win64.dll, OodleLZ_Decompress's a DCX_KRAK .msb.dcx, parses Treasure events.
// Validates the full no-bake coverage path (vanilla-untouched maps stay KRAK) offline,
// using the exact Oodle entry the DLL hooks at runtime.
//   build/run: see tools/msbe_test/build_oodle.sh
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <vector>

#include "worldmap/msbe_parser.hpp"

// The 14-arg OodleLZ_Decompress (same shape as the DLL's g_oodle_orig typedef).
typedef intptr_t(__stdcall *OodleLZ_Decompress_t)(const void *src, intptr_t srcLen, void *dst,
                                                  intptr_t dstLen, int fuzz, int crc, int verbose,
                                                  void *dbase, intptr_t dsize, void *cb,
                                                  void *cbctx, void *scratch, intptr_t scratchSize,
                                                  int threadPhase);

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
    if (argc < 3) { std::fprintf(stderr, "usage: %s <oo2core.dll> <file.msb.dcx>\n", argv[0]); return 2; }

    HMODULE oo = LoadLibraryA(argv[1]);
    if (!oo) { std::fprintf(stderr, "LoadLibrary failed: %s (err %lu)\n", argv[1], GetLastError()); return 1; }
    auto OodleLZ_Decompress = (OodleLZ_Decompress_t)GetProcAddress(oo, "OodleLZ_Decompress");
    if (!OodleLZ_Decompress) { std::fprintf(stderr, "no OodleLZ_Decompress export\n"); return 1; }

    std::vector<uint8_t> d = slurp(argv[2]);
    if (d.size() < 0x50 || memcmp(d.data(), "DCX\0", 4) != 0) { std::fprintf(stderr, "not DCX\n"); return 1; }

    auto be32 = [&](size_t o) {
        return (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 | (uint32_t)d[o + 3];
    };
    uint32_t uncomp = be32(0x1c), comp = be32(0x20);
    bool isKrak = memcmp(d.data() + 0x24, "DCP\0", 4) == 0 && memcmp(d.data() + 0x28, "KRAK", 4) == 0;
    std::printf("uncomp=%u comp=%u krak=%d\n", uncomp, comp, isKrak);
    if (!isKrak) { std::fprintf(stderr, "not KRAK (use msbe_test for DFLT)\n"); return 1; }

    // zlib/oodle stream begins at find("DCA\0")+8
    size_t dca = SIZE_MAX;
    for (size_t i = 0x28; i + 4 <= d.size(); i++)
        if (d[i] == 'D' && d[i + 1] == 'C' && d[i + 2] == 'A' && d[i + 3] == 0) { dca = i; break; }
    if (dca == SIZE_MAX) { std::fprintf(stderr, "no DCA\n"); return 1; }
    size_t zoff = dca + 8;

    std::vector<uint8_t> raw(uncomp);
    intptr_t got = OodleLZ_Decompress(d.data() + zoff, (intptr_t)(d.size() - zoff), raw.data(),
                                      (intptr_t)uncomp, 1, 0, 0, nullptr, 0, nullptr, nullptr,
                                      nullptr, 0, 3 /* OodleLZ_Decode_Unthreaded */);
    if (got != (intptr_t)uncomp) { std::fprintf(stderr, "Oodle decompress got=%lld want=%u\n", (long long)got, uncomp); return 1; }
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
