// Standalone msgbnd name indexer — uses the SAME BND4/FMG parsing semantics as
// src/worldmap/name_fmg_en.cpp (the mod's own reader): BND4 entries at
// 0x40+i*hdrSize with uncomp@+0x10 / data_off@+0x18 / name_off@+0x20; FMG v2
// groups at 0x28 + g*16 (4×int32), first-group-match wins.
//
// Answers the 2026-08-14 question: does the active install's msg data contain
// map-fragment goods under ANY name (English "Map:" / Japanese "地図"), i.e. is
// the "0 Cartes markers on GA" caused by missing goods or by a renamed/category
// mismatch? Uses the mod's real parsing code path (msbe::dcx_decompress).
//
// The KRAK decompressor needs the game's oo2core DLL — its location comes from
// GAME_DIR (or MFG_OO2CORE) in the environment, else from the repo's .env.local
// (same KEY=VALUE format as tools/load_env.py). Missing → KRAK files are skipped.
//
// Build: clang++ -std=c++17 -I../src -I../third_party tests/test_msg_names.cpp
//        ../src/worldmap/msbe_parser.cpp ../src/stb_image_impl.cpp -o test_msg_names
// Run:   test_msg_names <file.msgbnd.dcx> [more files...]

#include "worldmap/msbe_parser.hpp"  // msbe::dcx_decompress

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

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
// optional 'export ' prefix, surrounding quotes stripped; real shell env wins) ──
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
    // Candidate .env.local files: cwd, then up to 4 ancestor dirs (the repo root),
    // then the executable's own dir.
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
        else if (c < 0x800) { s.push_back(static_cast<char>(0xC0 | (c >> 6))); s.push_back(static_cast<char>(0x80 | (c & 0x3F))); }
        else { s.push_back(static_cast<char>(0xE0 | (c >> 12))); s.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F))); s.push_back(static_cast<char>(0x80 | (c & 0x3F))); }
    }
    return s;
}

// The mod's index_fmg, trimmed to GoodsName only, collecting (id -> name).
static void index_goods_fmg(const uint8_t *d, size_t n, std::map<int32_t, std::string> &out)
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

static int index_msgbnd(const std::vector<uint8_t> &buf, std::map<int32_t, std::string> &out)
{
    const uint8_t *d = buf.data();
    size_t n = buf.size();
    if (n < 0x40 || std::memcmp(d, "BND4", 4) != 0) return 0;
    int32_t file_count = rdi32(d, n, 0x0C);
    int64_t hdr_size = static_cast<int64_t>(rd64(d, n, 0x20));
    if (file_count <= 0 || file_count > 100000 || hdr_size < 0x24) return 0;
    int kept = 0;
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
        if (base != "GoodsName.fmg") continue;
        index_goods_fmg(d + data_off, static_cast<size_t>(uncomp), out);
        ++kept;
    }
    return kept;
}

static bool map_like(const std::string &s)
{
    if (s.size() < 3) return false;
    if (s.rfind("map", 0) == 0 || s.rfind("Map", 0) == 0) return true;  // "Map: Limgrave"
    if (s.find("\xE5\x9C\xB0\xE5\x9B\xB3") != std::string::npos) return true;  // 地図 (JP)
    if (s.find("\xE5\x9C\xB0\xE5\x9B\xBE") != std::string::npos) return true;  // 地图 (CN)
    return false;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    // oo2core location: env var / .env.local (see oo2core_path). Not found → KRAK files skip.
    HMODULE oo = nullptr;
    const std::string ooPath = oo2core_path();
    if (!ooPath.empty())
    {
        oo = LoadLibraryA(ooPath.c_str());
        if (!oo) std::printf("(oo2core not loadable at '%s' — KRAK files will fail)\n", ooPath.c_str());
    }
    else
        std::printf("(no GAME_DIR/MFG_OO2CORE in env or .env.local — KRAK files will fail)\n");
    goblin::msbe::OodleDecompressFn oodle = oo
        ? reinterpret_cast<goblin::msbe::OodleDecompressFn>(GetProcAddress(oo, "OodleLZ_Decompress"))
        : nullptr;
    if (oodle) std::printf("(oo2core loaded from %s)\n", ooPath.c_str());
#else
    goblin::msbe::OodleDecompressFn oodle = nullptr;
#endif
    if (argc < 2) { std::printf("usage: %s <msgbnd.dcx>...\n", argv[0]); return 1; }
    for (int a = 1; a < argc; ++a)
    {
        std::vector<uint8_t> raw = slurp(argv[a]);
        if (raw.size() < 4) { std::printf("== %s: unreadable\n", argv[a]); continue; }
        std::vector<uint8_t> buf;
        if (raw[0] == 'D' && raw[1] == 'C' && raw[2] == 'X' && raw[3] == 0)
        {
            bool krak = false;
            buf = goblin::msbe::dcx_decompress(raw.data(), raw.size(), &krak, oodle);
            std::printf("== %s: dcx (%s) -> %zu bytes\n", argv[a], krak ? "KRAK" : "DFLT", buf.size());
        }
        else { buf = raw; std::printf("== %s: not a DCX, %zu bytes\n", argv[a], buf.size()); }
        if (buf.size() < 4) continue;
        std::map<int32_t, std::string> names;
        int kept = index_msgbnd(buf, names);
        std::printf("   BND4 kept=%d GoodsName entries=%zu\n", kept, names.size());
        int maps = 0;
        for (const auto &[id, name] : names)
        {
            if (!map_like(name)) continue;
            ++maps;
            std::printf("   MAP-LIKE id=%d: %s\n", id, name.c_str());
        }
        std::printf("   -> %d map-like names\n", maps);
    }
    return 0;
}
