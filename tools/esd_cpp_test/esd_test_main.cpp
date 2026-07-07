// Offline oracle test for the C++ ESD parser (src/worldmap/esd_parser.cpp) — verifies the
// runtime merchant-pin chain WITHOUT booting the game, against two oracles:
//
//   esd_test <talkesdbnd.dcx> [...]      one "t<talkId>  1:22  args=[a, b]" line per hit,
//                                        the same shape as `dotnet tools/esd_shop dump 1:22`
//                                        → diff against the SoulsFormats reader.
//   esd_test join <talkDir> <mapDir>     the FULL merchant join (talk ESD ranges × MSB Enemy
//                                        TalkID), one line per deduped placement → compare
//                                        against tools/esd_shop/merchants.json.
//
// KRAK (Oodle) bnds need the game's oo2core: set ESD_TEST_OODLE=<path to oo2core_6_win64.dll>.
// Build (standalone, no CMake):
//   clang++ -std=c++20 -O2 -D_CRT_SECURE_NO_WARNINGS -I src -I third_party \
//     tools/esd_cpp_test/esd_test_main.cpp src/worldmap/esd_parser.cpp \
//     src/worldmap/msbe_parser.cpp src/stb_image_impl.cpp -o esd_test.exe
//
// Validated 2026-07-07 (Windows box, ERRv2.2.9.6): 161/161 literal 1:22 ranges byte-identical
// to esd_shop over all 17 ERR talk bnds; join = 38/39 merchants.json rows matched (the 39th is
// Kalé, whom ERR itself replaces with talk 437006001 at the same spot — the runtime join
// correctly reports ERR's version, which the vanilla-MSB oracle can't see).
#include "worldmap/esd_parser.hpp"
#include "worldmap/msbe_parser.hpp"

#include <cstdio>
#include <fstream>
#include <vector>

#include <windows.h>

#include <cmath>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

static std::vector<uint8_t> slurp(const fs::path &p)
{
    std::ifstream f(p, std::ios::binary);
    return f ? std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>())
             : std::vector<uint8_t>{};
}

// join <talkDir> <mapDir>: the full merchant join, mirroring build_disk_merchant_markers —
// prints one line per deduped placement for the merchants.json oracle compare.
static int run_join(const char *talkDir, const char *mapDir,
                    goblin::msbe::OodleDecompressFn oodle)
{
    std::unordered_map<uint32_t, std::vector<std::pair<int32_t, int32_t>>> by_talk;
    for (auto &de : fs::directory_iterator(talkDir))
    {
        std::string n = de.path().filename().string();
        if (n.size() < 16 || n.compare(n.size() - 15, 15, ".talkesdbnd.dcx") != 0) continue;
        std::vector<uint8_t> dcx = slurp(de.path());
        bool krak = false;
        std::vector<uint8_t> bnd = goblin::msbe::dcx_decompress(dcx.data(), dcx.size(), &krak, oodle);
        if (bnd.empty()) { std::fprintf(stderr, "decompress failed: %s\n", n.c_str()); continue; }
        for (const auto &r : goblin::esd::parse_talkbnd_shop_ranges(bnd.data(), bnd.size()))
            if (r.talkId != 1000)
                by_talk[r.talkId].emplace_back(r.begin, r.end);
    }
    std::fprintf(stderr, "[join] %zu shop TalkIDs\n", by_talk.size());

    int placements = 0;
    std::unordered_set<std::string> seen;
    for (auto &de : fs::directory_iterator(mapDir))
    {
        std::string n = de.path().filename().string();
        if (n.size() < 8 || n.compare(n.size() - 8, 8, ".msb.dcx") != 0) continue;
        int a = 0, x = 0, z = 0, lod = -1;
        if (std::sscanf(n.c_str(), "m%d_%d_%d_%d", &a, &x, &z, &lod) != 4 || lod != 0) continue;
        std::vector<uint8_t> dcx = slurp(de.path());
        bool krak = false;
        std::vector<uint8_t> msb = goblin::msbe::dcx_decompress(dcx.data(), dcx.size(), &krak, oodle);
        if (msb.empty()) continue;
        auto r = goblin::msbe::parse_msb(msb.data(), msb.size(), false, 0,
                                         /*wantAssets=*/false, /*wantEnemies=*/true);
        if (!r.ok) continue;
        for (const auto &en : r.enemies)
        {
            if (en.talkId == 0 || !by_talk.count(en.talkId)) continue;
            char pk[96];
            std::snprintf(pk, sizeof(pk), "%u_%d_%d_%d_%ld_%ld", en.talkId, a, x, z,
                          std::lround(en.pos[0] * 10.0f), std::lround(en.pos[2] * 10.0f));
            if (!seen.insert(pk).second) continue;
            std::printf("talk=%u tile=m%02d_%02d_%02d pos=%.1f,%.1f,%.1f shops=", en.talkId,
                        a, x, z, en.pos[0], en.pos[1], en.pos[2]);
            for (auto [b, e2] : by_talk[en.talkId]) std::printf("%d-%d ", b, e2);
            std::printf("\n");
            ++placements;
        }
    }
    std::fprintf(stderr, "[join] %d deduped placements\n", placements);
    return 0;
}

int main(int argc, char **argv)
{
    // KRAK (Oodle) bnds: borrow the game's own oo2core via ESD_TEST_OODLE.
    goblin::msbe::OodleDecompressFn oodle = nullptr;
    if (const char *op = std::getenv("ESD_TEST_OODLE"))
        if (HMODULE h = LoadLibraryA(op))
            oodle = reinterpret_cast<goblin::msbe::OodleDecompressFn>(
                GetProcAddress(h, "OodleLZ_Decompress"));
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: esd_test <talkesdbnd.dcx> [...] | esd_test join <talkDir> <mapDir>\n");
        return 2;
    }
    if (argc == 4 && std::string(argv[1]) == "join")
        return run_join(argv[2], argv[3], oodle);
    for (int a = 1; a < argc; ++a)
    {
        std::ifstream f(argv[a], std::ios::binary);
        if (!f) { std::fprintf(stderr, "read failed: %s\n", argv[a]); return 1; }
        std::vector<uint8_t> dcx((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
        bool krak = false;
        std::vector<uint8_t> bnd = goblin::msbe::dcx_decompress(dcx.data(), dcx.size(), &krak, oodle);
        if (bnd.empty()) { std::fprintf(stderr, "dcx decompress failed (krak=%d): %s\n", krak, argv[a]); return 1; }
        int skipped = 0;
        auto ranges = goblin::esd::parse_talkbnd_shop_ranges(bnd.data(), bnd.size(), &skipped);
        for (const auto &r : ranges)
            std::printf("t%u  1:22  args=[%d, %d]\n", r.talkId, r.begin, r.end);
        if (skipped) std::fprintf(stderr, "%s: %d non-literal/odd-arity 1:22 args skipped\n", argv[a], skipped);
    }
    return 0;
}
