#include "loot_disk.hpp"

#include "msbe_parser.hpp"
#include "goblin_config.hpp"

#include <spdlog/spdlog.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cctype>
#include <cstdio>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace goblin::worldmap
{
namespace
{
fs::path g_mod_folder;

// Parent dir of eldenring.exe (the game install root), or empty.
fs::path game_dir()
{
    HMODULE h = GetModuleHandleW(L"eldenring.exe");
    if (!h) return {};
    wchar_t buf[MAX_PATH] = {0};
    if (!GetModuleFileNameW(h, buf, MAX_PATH)) return {};
    return fs::path(buf).parent_path();
}

// Given a candidate root, return its MapStudio dir if present. Accepts a map\
// root, a mod root (containing map\MapStudio), or the MapStudio dir itself.
fs::path map_studio_in(const fs::path &root)
{
    if (root.empty()) return {};
    std::error_code ec;
    if (fs::exists(root / "map" / "MapStudio", ec)) return root / "map" / "MapStudio";
    if (fs::exists(root / "MapStudio", ec)) return root / "MapStudio";
    if (fs::exists(root, ec) && root.filename() == "MapStudio") return root;
    return {};
}

// First existing of the candidate map dirs. config loot_msb_dir wins (resolved
// to its MapStudio, or used as-is if it's a loose dir of .msb.dcx); then the
// DLL's mod folder, then the game install dir.
fs::path resolve_map_dir()
{
    if (!config::lootMsbDir.empty())
    {
        fs::path cfg(config::lootMsbDir);
        if (fs::path r = map_studio_in(cfg); !r.empty()) return r;
        std::error_code ec;
        if (fs::exists(cfg, ec)) return cfg;  // a custom dir of loose .msb.dcx
    }
    if (fs::path r = map_studio_in(g_mod_folder); !r.empty()) return r;
    if (fs::path r = map_studio_in(game_dir()); !r.empty()) return r;
    return {};
}

std::vector<uint8_t> slurp(const fs::path &p)
{
    std::vector<uint8_t> v;
    std::ifstream f(p, std::ios::binary);
    if (!f) return v;
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    f.seekg(0, std::ios::beg);
    if (n > 0)
    {
        v.resize((size_t)n);
        f.read((char *)v.data(), n);
        if (!f) v.clear();
    }
    return v;
}

// Parse "m{AA}_{BB}_{CC}_00" → area/gx/gz. Only LOD0 (_00). False otherwise so
// _01/_02 connect proxies and _99 lighting tiles are skipped (rule: _00 only).
bool parse_tile(const std::string &stem, int &area, int &gx, int &gz)
{
    int a = 0, x = 0, z = 0, lod = -1;
    if (std::sscanf(stem.c_str(), "m%d_%d_%d_%d", &a, &x, &z, &lod) != 4) return false;
    if (lod != 0) return false;
    if (a < 0 || a > 255 || x < 0 || x > 255 || z < 0 || z > 255) return false;
    area = a;
    gx = x;
    gz = z;
    return true;
}
} // namespace

void set_mod_folder(const fs::path &p) { g_mod_folder = p; }

std::vector<DiskTreasure> load_disk_treasures()
{
    std::vector<DiskTreasure> out;
    fs::path dir = resolve_map_dir();
    if (dir.empty())
    {
        spdlog::warn("[LOOTDISK] no map\\MapStudio dir found (set loot_msb_dir) — disk loot off");
        return out;
    }
    spdlog::info("[LOOTDISK] reading MSBs from {}", dir.string());

    int parsed = 0, kraks = 0, withPart = 0;
    std::error_code ec;
    for (auto &de : fs::directory_iterator(dir, ec))
    {
        if (!de.is_regular_file(ec)) continue;
        const fs::path &p = de.path();
        std::string name = p.filename().string();  // e.g. "m60_42_36_00.msb.dcx"
        std::string lower = name;
        for (char &c : lower) c = (char)std::tolower((unsigned char)c);
        if (lower.size() < 8 || lower.compare(lower.size() - 8, 8, ".msb.dcx") != 0) continue;
        std::string stem = name.substr(0, name.size() - 8);  // strip ".msb.dcx"
        int area = 0, gx = 0, gz = 0;
        if (!parse_tile(stem, area, gx, gz)) continue;

        std::vector<uint8_t> dcx = slurp(p);
        if (dcx.empty())
        {
            spdlog::warn("[LOOTDISK] read failed: {}", name);
            continue;
        }
        bool krak = false;
        std::vector<uint8_t> msb = msbe::dcx_decompress(dcx.data(), dcx.size(), &krak);
        if (msb.empty())
        {
            if (krak) ++kraks;  // vanilla/other KRAK map — needs Oodle (not wired yet)
            else spdlog::warn("[LOOTDISK] decompress failed: {}", name);
            continue;
        }
        msbe::ParseResult r = msbe::parse_msb(msb.data(), msb.size(), /*resident=*/false);
        if (!r.ok)
        {
            spdlog::warn("[LOOTDISK] parse failed: {}", name);
            continue;
        }
        ++parsed;

        int tilePos = 0;
        for (const auto &t : r.treasures)
        {
            if (t.partIndex < 0) continue;  // item-glow / EMEVD-region → no MSB pos
            DiskTreasure d;
            d.lotId = t.itemLotId;
            d.area = (uint8_t)area;
            d.gx = (uint8_t)gx;
            d.gz = (uint8_t)gz;
            d.posX = t.pos[0];
            d.posZ = t.pos[2];  // Part+0x20 X/Z (Y unused for markers)
            out.push_back(d);
            ++tilePos;
        }
        withPart += tilePos;
        spdlog::debug("[LOOTDISK] {} -> {} treasures ({} positioned)", name,
                      r.treasures.size(), tilePos);
    }
    spdlog::info("[LOOTDISK] {} _00 MSBs parsed ({} positioned treasures); {} KRAK skipped",
                 parsed, withPart, kraks);
    return out;
}
} // namespace goblin::worldmap
