#include "loot_disk.hpp"

#include "msbe_parser.hpp"
#include "goblin_config.hpp"

#include <spdlog/spdlog.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <system_error>

namespace fs = std::filesystem;

namespace goblin::worldmap
{
namespace
{
fs::path g_mod_folder;

// ── Map-dir discovery state (see loot_disk.hpp) ───────────────────────────────
std::atomic<int>                      g_state{static_cast<int>(DiskLootState::Disabled)};
std::mutex                            g_dir_mtx;       // guards g_resolved_dir
fs::path                              g_resolved_dir;  // the MapStudio dir (Found)
std::atomic<bool>                     g_dir_attempted{false};  // ancestor-walk ran once
std::chrono::steady_clock::time_point g_search_t0;     // when Searching began
constexpr int kSearchTimeoutSec = 120;  // covers menu+save-load; Failed is recoverable
void (*g_build_trigger)() = nullptr;    // set at init; kicks the build on discovery

// Resolve the game's loaded oo2core OodleLZ_Decompress (for DCX_KRAK maps — vanilla
// + the mod's unmodified maps). eldenring.exe imports oo2core_6_win64.dll, so it's
// already in-process; GetModuleHandle finds it (LoadLibrary as a fallback). Cached.
msbe::OodleDecompressFn resolve_oodle()
{
    static msbe::OodleDecompressFn fn = nullptr;
    static bool tried = false;
    if (tried) return fn;
    tried = true;
    HMODULE h = GetModuleHandleW(L"oo2core_6_win64.dll");
    if (!h) h = LoadLibraryW(L"oo2core_6_win64.dll");
    if (h) fn = (msbe::OodleDecompressFn)GetProcAddress(h, "OodleLZ_Decompress");
    spdlog::info("[LOOTDISK] Oodle (KRAK maps) {}", fn ? "available" : "NOT found (KRAK skipped)");
    return fn;
}

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
// to its MapStudio, or used as-is if it's a loose dir of .msb.dcx); then a DLL-
// relative search; then the game install dir.
fs::path resolve_map_dir()
{
    // Test affordance: __test_fallback__ forces the ancestor-walk to find nothing →
    // Searching → the CreateFileW discovery (Tier 2) takes over (ERR opens maps within
    // seconds, so it recovers to Found fast). To see the Tier-3 Failed red-error on a
    // normal install, use __test_error__ (handled in ensure_map_dir_resolved — it can't
    // be reached by timeout here because the game always opens a map first).
    if (config::lootMsbDir == "__test_fallback__") return {};

    if (!config::lootMsbDir.empty())
    {
        fs::path cfg(config::lootMsbDir);
        if (fs::path r = map_studio_in(cfg); !r.empty()) return r;
        std::error_code ec;
        if (fs::exists(cfg, ec)) return cfg;  // a custom dir of loose .msb.dcx
    }
    // DLL-relative search. g_mod_folder is the DLL's OWN folder (where the INI lives),
    // which on the ERR/ModEngine layout is <root>/dll/offline while the mod's loose maps
    // are in <root>/mod/map/MapStudio — so map_studio_in(g_mod_folder) alone misses them.
    // Walk up a few levels, probing each ancestor AND its "mod" subfolder.
    fs::path p = g_mod_folder;
    for (int up = 0; up < 5 && !p.empty() && p != p.root_path(); ++up, p = p.parent_path())
    {
        if (fs::path r = map_studio_in(p); !r.empty()) return r;
        if (fs::path r = map_studio_in(p / "mod"); !r.empty()) return r;
    }
    // Game install dir, and its "mod" sibling (ModEngine2 next to eldenring.exe).
    fs::path g = game_dir();
    if (fs::path r = map_studio_in(g); !r.empty()) return r;
    if (fs::path r = map_studio_in(g / "mod"); !r.empty()) return r;
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

bool disk_source_enabled()
{
    return config::lootFromDiskMsb || config::lootCollectibles || config::lootEnemyDrops ||
           config::lootEmevdDrops;
}

void ensure_map_dir_resolved()
{
    if (!disk_source_enabled())
    {
        g_state.store(static_cast<int>(DiskLootState::Disabled));
        return;
    }
    if (config::lootMsbDir == "__test_error__")  // test: force the F1 red-error directly
    {
        g_state.store(static_cast<int>(DiskLootState::Failed));
        return;
    }
    if (g_dir_attempted.exchange(true)) return;  // ancestor-walk runs exactly once
    if (static_cast<DiskLootState>(g_state.load()) == DiskLootState::Found) return;  // discovery beat us
    fs::path d = resolve_map_dir();
    if (!d.empty())
    {
        {
            std::lock_guard<std::mutex> lk(g_dir_mtx);
            g_resolved_dir = d;
        }
        g_state.store(static_cast<int>(DiskLootState::Found));
    }
    else
    {
        g_search_t0 = std::chrono::steady_clock::now();
        g_state.store(static_cast<int>(DiskLootState::Searching));
        spdlog::warn("[LOOTDISK] map dir not found by ancestor-walk — falling back to the "
                     "CreateFileW observer (waiting for the game to open a map; {}s timeout).",
                     kSearchTimeoutSec);
    }
}

DiskLootState disk_loot_state()
{
    if (!disk_source_enabled()) return DiskLootState::Disabled;
    DiskLootState s = static_cast<DiskLootState>(g_state.load());
    if (s == DiskLootState::Searching)
    {
        auto el = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::steady_clock::now() - g_search_t0)
                      .count();
        if (el > kSearchTimeoutSec)
        {
            g_state.store(static_cast<int>(DiskLootState::Failed));
            return DiskLootState::Failed;
        }
    }
    return s;
}

fs::path disk_loot_dir()
{
    std::lock_guard<std::mutex> lk(g_dir_mtx);
    return g_resolved_dir;
}

void on_map_opened_path(const wchar_t *full_path)
{
    if (!full_path || !disk_source_enabled()) return;
    if (config::lootMsbDir == "__test_error__") return;  // test: keep Failed, suppress discovery
    if (static_cast<DiskLootState>(g_state.load()) == DiskLootState::Found) return;  // already have it
    std::error_code ec;
    fs::path dir = fs::path(full_path).lexically_normal().parent_path();  // ...\map\MapStudio
    if (dir.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_dir_mtx);
        g_resolved_dir = dir;
    }
    g_state.store(static_cast<int>(DiskLootState::Found));
    spdlog::info("[LOOTDISK] map dir discovered via CreateFileW fallback: {}", dir.string());
    if (g_build_trigger) g_build_trigger();  // kick the worker now, not at the next overlay tick
}

void set_build_trigger(void (*fn)()) { g_build_trigger = fn; }

std::vector<DiskTreasure> load_disk_treasures(std::vector<uint32_t> *droppedDummyLots,
                                              std::vector<DiskCollectible> *collectibles,
                                              std::vector<DiskEnemy> *enemies)
{
    std::vector<DiskTreasure> out;
    const bool wantAssets = collectibles != nullptr;
    const bool wantEnemies = enemies != nullptr;
    ensure_map_dir_resolved();      // ancestor-walk → Found/Searching (CreateFileW completes it)
    fs::path dir = disk_loot_dir(); // empty until Found (ancestor-walk or the observer)
    if (dir.empty())
    {
        spdlog::warn("[LOOTDISK] no map\\MapStudio dir yet (ancestor-walk empty; awaiting "
                     "CreateFileW discovery or set loot_msb_dir) — disk loot deferred");
        return out;
    }
    spdlog::info("[LOOTDISK] reading MSBs from {}", dir.string());

    msbe::OodleDecompressFn oodle = resolve_oodle(); // KRAK support (vanilla/unmodified maps)
    int parsed = 0, kraks = 0, withPart = 0, dummies = 0;
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
        std::vector<uint8_t> msb = msbe::dcx_decompress(dcx.data(), dcx.size(), &krak, oodle);
        if (msb.empty())
        {
            if (krak) ++kraks;  // KRAK map but Oodle unavailable / decompress failed
            else spdlog::warn("[LOOTDISK] decompress failed: {}", name);
            continue;
        }
        msbe::ParseResult r = msbe::parse_msb(msb.data(), msb.size(), /*resident=*/false,
                                              /*blobBase=*/0, wantAssets, wantEnemies);
        if (!r.ok)
        {
            spdlog::warn("[LOOTDISK] parse failed: {}", name);
            continue;
        }
        ++parsed;

        if (wantAssets)
        {
            for (const auto &a : r.assets)
            {
                DiskCollectible c;
                c.aegRow = a.aegRow;
                c.area = (uint8_t)area;
                c.gx = (uint8_t)gx;
                c.gz = (uint8_t)gz;
                c.posX = a.pos[0];
                c.posZ = a.pos[2];
                collectibles->push_back(c);
            }
        }

        if (wantEnemies)
        {
            for (const auto &en : r.enemies)
            {
                DiskEnemy e;
                e.npcParamId = en.npcParamId;
                e.entityId = en.entityId;
                e.area = (uint8_t)area;
                e.gx = (uint8_t)gx;
                e.gz = (uint8_t)gz;
                e.posX = en.pos[0];
                e.posZ = en.pos[2];
                enemies->push_back(e);
            }
        }

        int tilePos = 0;
        for (const auto &t : r.treasures)
        {
            if (t.partIndex < 0) continue;  // item-glow / EMEVD-region → no MSB pos
            // DummyAsset placements: drop INERT ones (no entity binding) — these
            // are disabled/cut placeholders the player can't reach (305/315 of the
            // pipeline's unreachable lots, validated offline). KEEP reachable ones
            // (EntityID or an EntityGroupID set → an EMEVD can activate the pickup):
            // exactly the 3 reachable_dummy lots, recovered here without the bake
            // (4910/15000990 carry an EntityID; the rule re-introduces 0 false
            // positives — see windows_msbe_dummyasset_unreachable_re_findings.md).
            // A dropped inert dummy just stays on its baked marker (coverage-replace
            // keeps uncovered baked rows), so no loot is lost.
            if (t.partType == msbe::PART_DUMMY_ASSET && t.entityId == 0 && !t.entityGroup)
            {
                ++dummies;
                if (droppedDummyLots && t.itemLotId) droppedDummyLots->push_back(t.itemLotId);
                continue;
            }
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
    spdlog::info("[LOOTDISK] {} _00 MSBs parsed ({} positioned treasures, {} inert DummyAsset "
                 "dropped; reachable dummies kept); {} KRAK skipped", parsed, withPart, dummies, kraks);
    if (wantEnemies)
        spdlog::info("[LOOTDISK] {} enemy placements parsed (with NPCParamID)", enemies->size());
    return out;
}

namespace
{
// The mod's event\ dir (sibling of map\MapStudio), holding *.emevd.dcx. Derived from the
// resolved MapStudio dir: <root>\map\MapStudio → <root>\event. Falls back across a couple
// of layouts (a map\ root, or the dir itself) so a custom loot_msb_dir still works when it
// sits beside an event\ folder. Empty if none found.
fs::path resolve_event_dir(const fs::path &mapStudio)
{
    if (mapStudio.empty()) return {};
    std::error_code ec;
    // <root>\map\MapStudio → <root>\event  (the normal ERR/ModEngine layout)
    fs::path root = mapStudio.parent_path().parent_path();
    if (fs::path e = root / "event"; fs::exists(e, ec)) return e;
    // <root>\map → <root>\event
    if (fs::path e = mapStudio.parent_path() / "event"; fs::exists(e, ec)) return e;
    // a flat custom dir that itself contains an event\ subfolder
    if (fs::path e = mapStudio / "event"; fs::exists(e, ec)) return e;
    return {};
}
} // namespace

std::vector<DiskEmevd> load_emevd_awards()
{
    std::vector<DiskEmevd> out;
    ensure_map_dir_resolved();
    fs::path mapStudio = disk_loot_dir();
    fs::path evdir = resolve_event_dir(mapStudio);
    if (evdir.empty())
    {
        spdlog::warn("[LOOTDISK] no event\\ dir found beside {} — EMEVD loot deferred",
                     mapStudio.empty() ? "(no map dir yet)" : mapStudio.string());
        return out;
    }
    spdlog::info("[LOOTDISK] reading EMEVD from {}", evdir.string());

    msbe::OodleDecompressFn oodle = resolve_oodle();  // KRAK events (unmodified vanilla)
    int parsed = 0, kraks = 0;
    std::error_code ec;
    for (auto &de : fs::directory_iterator(evdir, ec))
    {
        if (!de.is_regular_file(ec)) continue;
        std::string name = de.path().filename().string();  // e.g. "m60.emevd.dcx"
        std::string lower = name;
        for (char &c : lower) c = (char)std::tolower((unsigned char)c);
        if (lower.size() < 10 || lower.compare(lower.size() - 10, 10, ".emevd.dcx") != 0)
            continue;
        std::vector<uint8_t> dcx = slurp(de.path());
        if (dcx.empty()) { spdlog::warn("[LOOTDISK] read failed: {}", name); continue; }
        bool krak = false;
        std::vector<uint8_t> evd = msbe::dcx_decompress(dcx.data(), dcx.size(), &krak, oodle);
        if (evd.empty())
        {
            if (krak) ++kraks;
            else spdlog::warn("[LOOTDISK] EMEVD decompress failed: {}", name);
            continue;
        }
        std::vector<msbe::EmevdAward> aw = msbe::parse_emevd(evd.data(), evd.size());
        for (const auto &a : aw) out.push_back({a.entityId, a.lotId});
        ++parsed;
        spdlog::debug("[LOOTDISK] {} -> {} EMEVD awards", name, aw.size());
    }
    spdlog::info("[LOOTDISK] {} EMEVD files parsed; {} template awards; {} KRAK skipped",
                 parsed, (int)out.size(), kraks);
    return out;
}
} // namespace goblin::worldmap
