// Standalone data-parsing tester — runs the MOD'S OWN C++ parsers against real install files
// (the active mod's or vanilla's), so a parsing suspicion can be proven/refuted offline without
// booting the game. See docs/memory/tooling/data-parse-tester.md.
//
// The parsers are the same code the DLL ships (msbe_parser.cpp + the BND4/FMG semantics of
// name_fmg_en.cpp), compiled here as a plain binary. The KRAK decompressor needs the game's
// oo2core DLL — its location comes from MFG_OO2CORE or GAME_DIR in the environment, else from
// the repo's .env.local (same KEY=VALUE format as tools/load_env.py). Missing → KRAK files are
// skipped with a note.
//
// usage: test_parse <mode> [options] <files...>
//   modes:
//     msgbnd  — index every Name FMG in each msgbnd (BND4 → FMG v2) and print per-category
//               entry counts; --cat <basename> limits to one FMG; --filter <substr> prints the
//               matching (id, name) rows. (The 2026-08-14 GA Cartes hunt: "Map:" names under a
//               re-sorted sortGroup.)
//     msb     — msbe::parse_msb each MSB and print section counts (treasures/assets/enemies/
//               regions/objacts) + a sample of the treasure lot ids.
//     emevd   — msbe::parse_emevd + emevd_inits each EMEVD and print the award/init counts.
//     dcx     — just DCX-decompress each file and report sizes (sanity-checks the oo2core path).
//   options:
//     --cat <basename>   msgbnd only: index just this FMG basename (e.g. GoodsName.fmg)
//     --filter <substr>  msgbnd only: print id=name rows containing <substr> (case-insensitive)
//     -h | --help        this text
//
// Build (clang-cl/xwin, same toolchain as the DLL):
//   clang++ -std=c++17 -Isrc -Ithird_party tests/test_parse.cpp src/worldmap/msbe_parser.cpp \
//           src/stb_image_impl.cpp -o test_parse
// Run examples:
//   test_parse msgbnd --cat GoodsName.fmg --filter "Map:" GA/msg/engus/item_dlc02.msgbnd.dcx
//   test_parse msb     GA/map/MapStudio/m60_46_38_00.msb.dcx
//   test_parse emevd   GA/event/common.emevd.dcx
//   test_parse dcx     <any .dcx>            # verifies the oo2core/.env.local plumbing

#include "worldmap/msbe_parser.hpp"  // msbe::dcx_decompress / parse_msb / parse_emevd / emevd_inits

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// ── file I/O ────────────────────────────────────────────────────────────────────────────────

static std::vector<uint8_t> slurp(const std::string &p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> v(static_cast<size_t>(n));
    f.read(reinterpret_cast<char *>(v.data()), n);
    return v;
}

// ── .env.local resolution (mirrors tools/load_env.py: KEY=VALUE lines, # comments,
//    optional 'export ' prefix, surrounding quotes stripped; real shell env wins) ──

static std::string env_value(const char *key)
{
#ifdef _WIN32
    char buf[1024] = {0};
    DWORD n = GetEnvironmentVariableA(key, buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) return buf;
#else
    if (const char *v = std::getenv(key)) return v;
#endif
    return {};
}

static std::string env_local_value(const std::string &key)
{
    std::vector<std::string> cands = {".env.local"};
    for (int up = 1; up <= 4; ++up)
    {
        std::string p;
        for (int i = 0; i < up; ++i) p += "..\\";
        p += ".env.local";
        cands.push_back(p);
    }
#ifdef _WIN32
    {
        char exe[MAX_PATH] = {0};
        if (GetModuleFileNameA(nullptr, exe, MAX_PATH))
        {
            std::string d = exe;
            size_t sep = d.find_last_of("\\/");
            if (sep != std::string::npos) cands.push_back(d.substr(0, sep + 1) + ".env.local");
        }
    }
#endif
    for (const auto &cand : cands)
    {
        std::ifstream f(cand);
        if (!f) continue;
        std::string line;
        while (std::getline(f, line))
        {
            if (line.empty() || line[0] == '#') continue;
            if (line.rfind("export ", 0) == 0) line = line.substr(7);
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq);
            while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
            if (k != key) continue;
            std::string v = line.substr(eq + 1);
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
            while (!v.empty() && (v.back() == ' ' || v.back() == '\r' || v.back() == '\n')) v.pop_back();
            if (v.size() >= 2 && v.front() == v.back() && (v.front() == '\'' || v.front() == '"'))
                v = v.substr(1, v.size() - 2);
            return v;
        }
    }
    return {};
}

static std::string oo2core_path()
{
    std::string direct = env_value("MFG_OO2CORE");
    if (direct.empty()) direct = env_local_value("MFG_OO2CORE");
    if (!direct.empty()) return direct;
    std::string game = env_value("GAME_DIR");
    if (game.empty()) game = env_local_value("GAME_DIR");
    if (game.empty()) return {};
    if (game.back() != '\\' && game.back() != '/') game += '\\';
    return game + "oo2core_6_win64.dll";
}

static goblin::msbe::OodleDecompressFn load_oodle()
{
#ifdef _WIN32
    const std::string ooPath = oo2core_path();
    if (ooPath.empty())
    {
        std::printf("(no GAME_DIR/MFG_OO2CORE in env or .env.local — KRAK files will fail)\n");
        return nullptr;
    }
    HMODULE oo = LoadLibraryA(ooPath.c_str());
    if (!oo)
    {
        std::printf("(oo2core not loadable at '%s' — KRAK files will fail)\n", ooPath.c_str());
        return nullptr;
    }
    auto fn = reinterpret_cast<goblin::msbe::OodleDecompressFn>(GetProcAddress(oo, "OodleLZ_Decompress"));
    if (fn) std::printf("(oo2core loaded from %s)\n", ooPath.c_str());
    return fn;
#else
    return nullptr;
#endif
}

// Decompress a file (DCX or raw) with the shared path — the load every mode starts with.
static std::vector<uint8_t> load_decompressed(const std::string &path,
                                              goblin::msbe::OodleDecompressFn oodle,
                                              bool *was_dcx, bool *was_krak)
{
    std::vector<uint8_t> raw = slurp(path);
    if (raw.size() < 4) return {};
    if (!(raw[0] == 'D' && raw[1] == 'C' && raw[2] == 'X' && raw[3] == 0))
    {
        *was_dcx = false;
        *was_krak = false;
        return raw;
    }
    *was_dcx = true;
    bool krak = false;
    std::vector<uint8_t> out = goblin::msbe::dcx_decompress(raw.data(), raw.size(), &krak, oodle);
    *was_krak = krak;
    return out;
}

// ── BND4/FMG readers (the mod's name_fmg_en.cpp semantics: BND4 entries at 0x40+i*hdrSize
//    with uncomp@+0x10 / data_off@+0x18 / name_off@+0x20; FMG v2 groups at 0x28 + g*16) ──

static uint32_t rd32(const uint8_t *d, size_t n, size_t o)
{
    return (o + 4 <= n) ? *(reinterpret_cast<const uint32_t *>(d + o)) : 0;
}
static uint64_t rd64(const uint8_t *d, size_t n, size_t o)
{
    return (o + 8 <= n) ? *(reinterpret_cast<const uint64_t *>(d + o)) : 0;
}
static int32_t rdi32(const uint8_t *d, size_t n, size_t o)
{
    return static_cast<int32_t>(rd32(d, n, o));
}

static std::string utf16le_to_utf8(const uint8_t *d, size_t n, size_t off)
{
    std::string s;
    for (size_t j = off; j + 1 < n; j += 2)
    {
        uint16_t c = static_cast<uint16_t>(d[j] | (static_cast<uint16_t>(d[j + 1]) << 8));
        if (c == 0) break;
        if (c < 0x80) s.push_back(static_cast<char>(c));
        else if (c < 0x800)
        {
            s.push_back(static_cast<char>(0xC0 | (c >> 6)));
            s.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
        else
        {
            s.push_back(static_cast<char>(0xE0 | (c >> 12)));
            s.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return s;
}

// One FMG → every id→string (first-group-match wins, like the mod).
static void index_fmg(const uint8_t *d, size_t n, std::map<int32_t, std::string> &out)
{
    if (n < 0x28) return;
    uint32_t entry_count = rd32(d, n, 0x10);
    uint64_t str_off_tbl = rd64(d, n, 0x18);
    uint32_t group_count = rd32(d, n, 0x0C);
    for (uint32_t g = 0; g < group_count; ++g)
    {
        size_t off = 0x28 + static_cast<size_t>(g) * 0x10;  // FMG v2: 16-byte groups
        if (off + 12 > n) break;
        int32_t idx_start = rdi32(d, n, off);
        int32_t id_start = rdi32(d, n, off + 4);
        int32_t id_end = rdi32(d, n, off + 8);
        if (id_end < id_start) continue;
        for (int64_t id = id_start; id <= id_end; ++id)
        {
            if (out.count(static_cast<int32_t>(id))) continue;  // first group wins
            int64_t entry_idx = static_cast<int64_t>(idx_start) + (id - id_start);
            if (entry_idx < 0 || static_cast<uint64_t>(entry_idx) >= entry_count) continue;
            size_t soff = static_cast<size_t>(str_off_tbl) + static_cast<size_t>(entry_idx) * 8;
            if (soff + 8 > n) continue;
            uint64_t str_off = rd64(d, n, soff);
            if (str_off == 0 || str_off + 2 > n) continue;
            std::string s = utf16le_to_utf8(d, n, static_cast<size_t>(str_off));
            if (s.empty()) continue;
            out.emplace(static_cast<int32_t>(id), std::move(s));
        }
    }
}

// A decompressed msgbnd → per-FMG-basename (id → name) maps.
static std::map<std::string, std::map<int32_t, std::string>>
index_msgbnd_fmgs(const std::vector<uint8_t> &buf)
{
    std::map<std::string, std::map<int32_t, std::string>> out;
    const uint8_t *d = buf.data();
    size_t n = buf.size();
    if (n < 0x40 || std::memcmp(d, "BND4", 4) != 0) return out;
    int32_t file_count = rdi32(d, n, 0x0C);
    int64_t hdr_size = static_cast<int64_t>(rd64(d, n, 0x20));
    if (file_count <= 0 || file_count > 100000 || hdr_size < 0x24) return out;
    for (int i = 0; i < file_count; ++i)
    {
        size_t eoff = 0x40 + static_cast<size_t>(i) * static_cast<size_t>(hdr_size);
        if (eoff + 0x24 > n) break;
        int64_t uncomp = static_cast<int64_t>(rd64(d, n, eoff + 0x10));
        int32_t data_off = rdi32(d, n, eoff + 0x18);
        int32_t name_off = rdi32(d, n, eoff + 0x20);
        if (data_off < 0 || uncomp <= 0) continue;
        if (static_cast<size_t>(data_off) + static_cast<size_t>(uncomp) > n) continue;
        std::string base;
        if (name_off > 0 && static_cast<size_t>(name_off) + 1 < n)
        {
            for (size_t j = static_cast<size_t>(name_off); j + 1 < n; j += 2)
            {
                uint16_t c = static_cast<uint16_t>(d[j] | (static_cast<uint16_t>(d[j + 1]) << 8));
                if (c == 0) break;
                if (c == L'\\' || c == L'/') base.clear();
                else base.push_back(static_cast<char>(c & 0xFF));
            }
        }
        if (base.empty() || base.find(".fmg") == std::string::npos) continue;
        std::vector<uint8_t> blob(d + data_off, d + data_off + uncomp);
        if (blob.size() >= 4 && blob[0] == 'D' && blob[1] == 'C' && blob[2] == 'X' && blob[3] == 0)
        {
            bool krak = false;
            blob = goblin::msbe::dcx_decompress(blob.data(), blob.size(), &krak, nullptr);
        }
        index_fmg(blob.data(), blob.size(), out[base]);
    }
    return out;
}

// ── modes ───────────────────────────────────────────────────────────────────────────────────

static int mode_msgbnd(const std::vector<std::string> &files, const std::string &cat,
                       const std::string &filter)
{
    goblin::msbe::OodleDecompressFn oodle = load_oodle();
    int total_cats = 0;
    for (const auto &path : files)
    {
        bool dcx = false, krak = false;
        std::vector<uint8_t> buf = load_decompressed(path, oodle, &dcx, &krak);
        std::printf("== %s: %s (%s) -> %zu bytes\n", path.c_str(),
                    buf.size() < 4 ? "LOAD FAIL" : (dcx ? "dcx" : "raw"),
                    krak ? "KRAK" : "DFLT", buf.size());
        if (buf.size() < 4) continue;
        auto fmgs = index_msgbnd_fmgs(buf);
        for (const auto &[base, names] : fmgs)
        {
            if (!cat.empty() && base != cat) continue;
            ++total_cats;
            std::printf("   %s: %zu entries\n", base.c_str(), names.size());
            if (!filter.empty())
            {
                int hits = 0;
                for (const auto &[id, name] : names)
                {
                    std::string low = name;
                    for (char &c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (low.find(filter) == std::string::npos) continue;
                    ++hits;
                    std::printf("      id=%d: %s\n", id, name.c_str());
                }
                std::printf("   -> %d matches for '%s'\n", hits, filter.c_str());
            }
        }
        if (fmgs.empty()) std::printf("   (no Name FMGs indexed — not a msgbnd?)\n");
    }
    return 0;
}

static int mode_msb(const std::vector<std::string> &files)
{
    goblin::msbe::OodleDecompressFn oodle = load_oodle();
    for (const auto &path : files)
    {
        bool dcx = false, krak = false;
        std::vector<uint8_t> buf = load_decompressed(path, oodle, &dcx, &krak);
        std::printf("== %s: %s (%s) -> %zu bytes\n", path.c_str(),
                    buf.size() < 4 ? "LOAD FAIL" : (dcx ? "dcx" : "raw"),
                    krak ? "KRAK" : "DFLT", buf.size());
        if (buf.size() < 0x10 || std::memcmp(buf.data(), "MSB ", 4) != 0) continue;
        goblin::msbe::ParseResult r = goblin::msbe::parse_msb(
            buf.data(), buf.size(), /*resident=*/false, /*blobBase=*/0,
            /*wantAssets=*/true, /*wantEnemies=*/true, /*wantRegions=*/true,
            /*crossTileAssets=*/false, /*wantObjActs=*/true);
        std::printf("   ok=%d  treasures=%zu assets=%zu enemies=%zu regions=%zu objacts=%zu\n",
                    (int)r.ok, r.treasures.size(), r.assets.size(), r.enemies.size(),
                    r.regions.size(), r.objacts.size());
        for (size_t i = 0; i < r.treasures.size() && i < 5; ++i)
            std::printf("      treasure lot=%u entity=%u pos=(%.0f,%.0f,%.0f) part=%s\n",
                        r.treasures[i].itemLotId, r.treasures[i].entityId, r.treasures[i].pos[0],
                        r.treasures[i].pos[1], r.treasures[i].pos[2],
                        r.treasures[i].partName.empty() ? "?" : r.treasures[i].partName.c_str());
    }
    return 0;
}

static int mode_emevd(const std::vector<std::string> &files)
{
    goblin::msbe::OodleDecompressFn oodle = load_oodle();
    for (const auto &path : files)
    {
        bool dcx = false, krak = false;
        std::vector<uint8_t> buf = load_decompressed(path, oodle, &dcx, &krak);
        std::printf("== %s: %s (%s) -> %zu bytes\n", path.c_str(),
                    buf.size() < 4 ? "LOAD FAIL" : (dcx ? "dcx" : "raw"),
                    krak ? "KRAK" : "DFLT", buf.size());
        if (buf.size() < 0x80 || std::memcmp(buf.data(), "EVD\x00", 4) != 0) continue;
        std::vector<goblin::msbe::EmevdAward> awards = goblin::msbe::parse_emevd(buf.data(), buf.size());
        std::vector<goblin::msbe::EmevdInit> inits = goblin::msbe::emevd_inits(buf.data(), buf.size());
        std::printf("   awards=%zu  bank-2000 inits=%zu\n", awards.size(), inits.size());
        for (size_t i = 0; i < awards.size() && i < 5; ++i)
            std::printf("      award entity=%u lot=%u\n", awards[i].entityId, awards[i].lotId);
    }
    return 0;
}

static int mode_dcx(const std::vector<std::string> &files)
{
    goblin::msbe::OodleDecompressFn oodle = load_oodle();
    for (const auto &path : files)
    {
        bool dcx = false, krak = false;
        std::vector<uint8_t> buf = load_decompressed(path, oodle, &dcx, &krak);
        std::printf("== %s: %s (%s) -> %zu bytes, first 4: %c%c%c%c\n", path.c_str(),
                    buf.size() < 4 ? "LOAD FAIL" : (dcx ? "dcx" : "raw"),
                    krak ? "KRAK" : "DFLT", buf.size(),
                    buf.size() >= 4 ? (char)buf[0] : ' ', buf.size() >= 4 ? (char)buf[1] : ' ',
                    buf.size() >= 4 ? (char)buf[2] : ' ', buf.size() >= 4 ? (char)buf[3] : ' ');
    }
    return 0;
}

static void usage(const char *argv0)
{
    std::printf(
        "usage: %s <mode> [options] <files...>\n"
        "  modes:\n"
        "    msgbnd  index every Name FMG in each msgbnd (BND4 -> FMG v2)\n"
        "    msb     msbe::parse_msb each MSB, print section counts + sample treasures\n"
        "    emevd   msbe::parse_emevd + emevd_inits, print award/init counts\n"
        "    dcx     decompress each file and report sizes (sanity-checks oo2core)\n"
        "  options:\n"
        "    --cat <basename>   msgbnd only: index just this FMG (e.g. GoodsName.fmg)\n"
        "    --filter <substr>  msgbnd only: print id=name rows containing <substr>\n"
        "    -h | --help        this text\n"
        "  oo2core comes from MFG_OO2CORE/GAME_DIR in the env or the repo .env.local.\n",
        argv0);
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        usage(argv[0]);
        return 1;
    }
    const std::string mode = argv[1];
    if (mode == "-h" || mode == "--help")
    {
        usage(argv[0]);
        return 0;
    }
    std::string cat, filter;
    std::vector<std::string> files;
    for (int a = 2; a < argc; ++a)
    {
        std::string arg = argv[a];
        if (arg == "--cat" && a + 1 < argc) cat = argv[++a];
        else if (arg == "--filter" && a + 1 < argc) filter = argv[++a];
        else files.push_back(arg);
    }
    if (files.empty())
    {
        std::printf("no files given\n");
        usage(argv[0]);
        return 1;
    }
    if (mode == "msgbnd") return mode_msgbnd(files, cat, filter);
    if (mode == "msb") return mode_msb(files);
    if (mode == "emevd") return mode_emevd(files);
    if (mode == "dcx") return mode_dcx(files);
    std::printf("unknown mode '%s'\n", mode.c_str());
    usage(argv[0]);
    return 1;
}
