// Offline MSBE parser verifier (Linux host tool). Reads a .msb.dcx, decompresses (DCX_DFLT
// zlib via stb), parses Treasure events, prints lot/part/pos. Validates the parser against
// the known result BEFORE any DLL build (m10_00_00_00: 113 treasures, lot=10000850
// part='AEG099_990_9002' pos=(-298.4,64.3,426.3) — exact-match to items_database.json).
//   build: see tools/msbe_test/build.sh
//   run:   ./msbe_test <path-to.msb.dcx>
#include "worldmap/msbe_parser.hpp"

#include <cstdio>
#include <cstdint>
#include <vector>

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

int main(int argc, char **argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: %s <file.msb.dcx>\n", argv[0]); return 2; }

    std::vector<uint8_t> dcx = slurp(argv[1]);
    if (dcx.empty()) { std::fprintf(stderr, "read failed: %s\n", argv[1]); return 1; }

    bool krak = false;
    std::vector<uint8_t> msb = goblin::msbe::dcx_decompress(dcx.data(), dcx.size(), &krak);
    if (msb.empty())
    {
        std::fprintf(stderr, "decompress failed (krak=%d, %zu bytes in)\n", krak, dcx.size());
        return 1;
    }
    std::printf("decompressed %zu -> %zu bytes\n", dcx.size(), msb.size());

    goblin::msbe::ParseResult r = goblin::msbe::parse_msb(msb.data(), msb.size(), false);
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
