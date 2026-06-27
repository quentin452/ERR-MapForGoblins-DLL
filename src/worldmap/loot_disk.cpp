#include "loot_disk.hpp"

#include "dvdbnd_reader.hpp"
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
#include <map>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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

// First existing <root>/rel over the candidate roots (mod overlay first so a mod's own
// file wins, then the UXM-unpacked game install). Mirrors resolve_map_dir's ancestor-walk
// but for an arbitrary file path — independent of the loot map-dir state. Empty if none.
fs::path resolve_root_file(const fs::path &rel)
{
    std::error_code ec;
    fs::path p = g_mod_folder;
    for (int up = 0; up < 5 && !p.empty() && p != p.root_path(); ++up, p = p.parent_path())
    {
        if (fs::exists(p / "mod" / rel, ec)) return p / "mod" / rel;  // ModEngine overlay
        if (fs::exists(p / rel, ec)) return p / rel;
    }
    fs::path g = game_dir();
    if (!g.empty())
    {
        if (fs::exists(g / "mod" / rel, ec)) return g / "mod" / rel;  // ME2 next to the exe
        if (fs::exists(g / rel, ec)) return g / rel;                  // UXM-unpacked base
    }
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
           config::lootEmevdDrops || config::worldFeaturesFromDisk;
}

std::vector<uint8_t> read_game_file_decompressed(const std::string &rel_path)
{
    // Test affordance: set env MFG_TEST_NO_GAMEFILE=1 to force "file unavailable" WITHOUT touching
    // any game file — disables BOTH the loose reader AND the packed dvdbnd fallback so the graceful
    // no-data path can be verified on a dev box. The game still loads its UI normally; only OUR
    // reader pretends to miss.
    bool test_no_file = false;
    if (char e[8]{}; GetEnvironmentVariableA("MFG_TEST_NO_GAMEFILE", e, sizeof(e)) && e[0] == '1')
        test_no_file = true;
    // Test affordance: MFG_TEST_FORCE_PACKED=1 SKIPS the loose reader so the packed dvdbnd path is
    // exercised even on an install that HAS the loose file (e.g. ERR via ME3). Proves the packed
    // chain in-game — expect the VANILLA sblytbnd from Data0, not the mod's loose override.
    bool force_packed = false;
    if (char e[8]{}; GetEnvironmentVariableA("MFG_TEST_FORCE_PACKED", e, sizeof(e)) && e[0] == '1')
        force_packed = true;

    // 1) Loose file first — a mod overlay's own .dcx (ME3/ModEngine/native mod/) or a UXM-unpacked
    //    install. This is the mod-AWARE source, so it wins when present.
    std::vector<uint8_t> raw;
    fs::path             full;
    if (!test_no_file && !force_packed)
    {
        full = resolve_root_file(fs::path(rel_path));
        if (!full.empty())
            raw = slurp(full);
    }
    if (force_packed)
        spdlog::warn("[GAMEFILE] MFG_TEST_FORCE_PACKED=1 → skipping loose, forcing dvdbnd: {}",
                     rel_path);
    // 2) Packed fallback — read straight out of the encrypted dvdbnd (Data*.bhd/.bdt) next to
    //    eldenring.exe. Works on a genuinely packed install where there's no loose .dcx for us to
    //    slurp; pure data + crypto, no game call. See [[dvdbnd-packed-reader]].
    std::string src = full.empty() ? std::string("dvdbnd") : full.string();
    if (raw.empty() && !test_no_file)
    {
        raw = dvdbnd::read_packed_file(game_dir(), rel_path);
        src = "dvdbnd:" + rel_path;
    }
    if (raw.size() < 4)
    {
        spdlog::warn("[GAMEFILE] '{}' not found loose or in the packed dvdbnd", rel_path);
        return {};
    }
    // Loose-uncompressed (not wrapped in DCX) → return as-is.
    if (!(raw[0] == 'D' && raw[1] == 'C' && raw[2] == 'X' && raw[3] == 0))
        return raw;
    bool krak = false;
    std::vector<uint8_t> out =
        msbe::dcx_decompress(raw.data(), raw.size(), &krak, resolve_oodle());
    if (out.empty())
        spdlog::warn("[GAMEFILE] DCX decompress failed ({}): {}", krak ? "KRAK" : "DFLT", src);
    else
        spdlog::info("[GAMEFILE] {} -> {} bytes ({})", src, out.size(), krak ? "KRAK" : "DFLT");
    return out;
}

std::map<std::string, std::vector<uint8_t>>
read_item_icon_sheets(const std::vector<std::string> &names)
{
    std::map<std::string, std::vector<uint8_t>> out;
    if (names.empty())
        return out;
    // Read + decompress the menu texture pack ONCE (loose mod overlay first, then packed dvdbnd).
    std::vector<uint8_t> tpf = read_game_file_decompressed("menu/hi/01_common.tpf.dcx");
    if (tpf.size() < 16)
    {
        spdlog::warn("[ITEMSHEET] 01_common.tpf unavailable ({} bytes)", tpf.size());
        return out;
    }
    int found = 0;
    for (const std::string &name : names)
    {
        size_t off = 0, len = 0;
        if (msbe::tpf_find_texture(tpf.data(), tpf.size(), name.c_str(), off, len))
        {
            out.emplace(name, std::vector<uint8_t>(tpf.begin() + off, tpf.begin() + off + len));
            ++found;
        }
        else
            spdlog::warn("[ITEMSHEET] '{}' not in 01_common.tpf", name);
    }
    spdlog::info("[ITEMSHEET] extracted {}/{} sheet DDS from 01_common.tpf ({} MB)", found,
                 names.size(), tpf.size() / (1024 * 1024));
    return out;  // the big TPF buffer frees here; only the small per-sheet DDS copies survive
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
                                              std::vector<DiskEnemy> *enemies,
                                              std::vector<DiskRegion> *regions)
{
    std::vector<DiskTreasure> out;
    const bool wantAssets = collectibles != nullptr;
    const bool wantEnemies = enemies != nullptr;
    const bool wantRegions = regions != nullptr;
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
    std::map<int, int> tier_files;  // LOD suffix → file count (tile coverage diag)
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
        // Tile coverage: tally the LOD suffix of EVERY tile (parsed or not) so the scoreboard
        // can show what the _00-only rule covers vs skips. _01/_02 are LOD connect-proxies and
        // _10/_11/_12 hold mostly GED-tier DUPLICATES of _00 (validated tools/tier_coverage.py),
        // so skipping them loses ~no unique markers — but the count makes the scope explicit.
        {
            int a = 0, x = 0, z = 0, lod = -1;
            if (std::sscanf(stem.c_str(), "m%d_%d_%d_%d", &a, &x, &z, &lod) == 4)
                ++tier_files[lod];
        }
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
                                              /*blobBase=*/0, wantAssets, wantEnemies, wantRegions);
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
                c.entityId = a.entityId;
                c.area = (uint8_t)area;
                c.gx = (uint8_t)gx;
                c.gz = (uint8_t)gz;
                c.posX = a.pos[0];
                c.posY = a.pos[1];
                c.posZ = a.pos[2];
                c.name = a.name;
                c.modelName = a.modelName;
                collectibles->push_back(std::move(c));
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
                e.name = en.name;
                enemies->push_back(std::move(e));
            }
        }

        if (wantRegions)
        {
            for (const auto &rg : r.regions)
            {
                DiskRegion d;
                d.subtype = rg.subtype;
                d.area = (uint8_t)area;
                d.gx = (uint8_t)gx;
                d.gz = (uint8_t)gz;
                d.posX = rg.pos[0];
                d.posY = rg.pos[1];
                d.posZ = rg.pos[2];
                d.name = rg.name;
                regions->push_back(std::move(d));
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
    if (wantRegions)
        spdlog::info("[LOOTDISK] {} spirit-spring regions parsed (MountJump/Locked/FakeSpiring)",
                     regions->size());
    // Tile coverage: parsed _00 vs skipped LOD/duplicate tiers, for the scoreboard.
    {
        int total = 0, skipped = 0;
        std::string by_tier;
        for (auto &[lod, n] : tier_files)
        {
            total += n;
            if (lod != 0) skipped += n;
            if (!by_tier.empty()) by_tier += " ";
            by_tier += "_" + std::string(lod < 10 ? "0" : "") + std::to_string(lod) + "=" +
                       std::to_string(n);
        }
        spdlog::info("[COVERAGE-TILES] parsed {}/{} tiles (_00 only); skipped {} non-_00 [{}]",
                     tier_files.count(0) ? tier_files[0] : 0, total, skipped, by_tier);
    }
    return out;
}

std::vector<DiskEnemy> load_lod_award_entities(const std::unordered_set<uint32_t> &wanted)
{
    std::vector<DiskEnemy> out;
    if (wanted.empty()) return out;
    fs::path dir = disk_loot_dir();
    if (dir.empty()) return out;
    msbe::OodleDecompressFn oodle = resolve_oodle();
    std::unordered_set<uint32_t> remaining = wanted;  // first-occurrence wins; stop once all found
    int scanned = 0;
    std::error_code ec;
    for (auto &de : fs::directory_iterator(dir, ec))
    {
        if (remaining.empty()) break;  // all resolved → skip the rest of the non-_00 tiles
        if (!de.is_regular_file(ec)) continue;
        const fs::path &p = de.path();
        std::string name = p.filename().string();
        std::string lower = name;
        for (char &c : lower) c = (char)std::tolower((unsigned char)c);
        if (lower.size() < 8 || lower.compare(lower.size() - 8, 8, ".msb.dcx") != 0) continue;
        std::string stem = name.substr(0, name.size() - 8);
        // Non-_00 LOD tiers ONLY: the _00 enemies are already in disk_enemies; this fills the
        // gap for award entities that exist solely as overworld LOD proxies (_01/_02/_10/_11/_12).
        // Skip _99 lighting tiles (no parts). The marker tile = the LOD tile (= what the bake used).
        int a = 0, x = 0, z = 0, lod = -1;
        if (std::sscanf(stem.c_str(), "m%d_%d_%d_%d", &a, &x, &z, &lod) != 4) continue;
        if (lod == 0 || lod == 99) continue;
        if (a < 0 || a > 255 || x < 0 || x > 255 || z < 0 || z > 255) continue;
        std::vector<uint8_t> dcx = slurp(p);
        if (dcx.empty()) continue;
        bool krak = false;
        std::vector<uint8_t> msb = msbe::dcx_decompress(dcx.data(), dcx.size(), &krak, oodle);
        if (msb.empty()) continue;
        // Enemy section only (no assets/regions) — minimal parse for the EntityID → pos join.
        msbe::ParseResult r = msbe::parse_msb(msb.data(), msb.size(), /*resident=*/false,
                                              /*blobBase=*/0, /*wantAssets=*/false,
                                              /*wantEnemies=*/true, /*wantRegions=*/false);
        if (!r.ok) continue;
        ++scanned;
        for (const auto &en : r.enemies)
        {
            if (en.entityId == 0 || !remaining.count(en.entityId)) continue;
            DiskEnemy e;
            e.npcParamId = en.npcParamId;
            e.entityId = en.entityId;
            e.area = (uint8_t)a;
            e.gx = (uint8_t)x;
            e.gz = (uint8_t)z;
            e.posX = en.pos[0];
            e.posZ = en.pos[2];
            e.name = en.name;
            out.push_back(std::move(e));
            remaining.erase(en.entityId);  // first MSB occurrence wins (mirrors the bake's join)
        }
    }
    spdlog::info("[LOOTDISK] LOD award-entity scan: {}/{} resolved in non-_00 tiles ({} tiles parsed, "
                 "{} unresolved)", (int)(wanted.size() - remaining.size()), (int)wanted.size(),
                 scanned, (int)remaining.size());
    return out;
}

std::vector<DiskCollectible> load_lod_feature_assets(const std::unordered_set<uint32_t> &wanted)
{
    std::vector<DiskCollectible> out;
    if (wanted.empty()) return out;
    fs::path dir = disk_loot_dir();
    if (dir.empty()) return out;
    msbe::OodleDecompressFn oodle = resolve_oodle();
    int scanned = 0;
    std::error_code ec;
    for (auto &de : fs::directory_iterator(dir, ec))
    {
        if (!de.is_regular_file(ec)) continue;
        const fs::path &p = de.path();
        std::string name = p.filename().string();
        std::string lower = name;
        for (char &c : lower) c = (char)std::tolower((unsigned char)c);
        if (lower.size() < 8 || lower.compare(lower.size() - 8, 8, ".msb.dcx") != 0) continue;
        std::string stem = name.substr(0, name.size() - 8);
        // Non-_00 LOD tiers ONLY: _00 assets are already in disk_collectibles; this fills the gap for
        // World-feature assets that exist solely as cross-tile LOD proxies (Snow Town statues live only
        // in the supertile m60_24_28_01). Skip _99 lighting tiles. Cannot stop early (a model has many
        // placements, no "all found"), so every non-_00 tile is parsed — same ~25ms budget as the LOD
        // award-entity scan, and only when worldFeaturesFromDisk is on.
        int a = 0, x = 0, z = 0, lod = -1;
        if (std::sscanf(stem.c_str(), "m%d_%d_%d_%d", &a, &x, &z, &lod) != 4) continue;
        if (lod == 0 || lod == 99) continue;
        std::vector<uint8_t> dcx = slurp(p);
        if (dcx.empty()) continue;
        bool krak = false;
        std::vector<uint8_t> msb = msbe::dcx_decompress(dcx.data(), dcx.size(), &krak, oodle);
        if (msb.empty()) continue;
        // Asset section only — the World-feature pass keys on aegRow + EntityID + position.
        // crossTileAssets=true: these LOD proxies carry a "m{tile}-AEG…" cross-tile part name, which
        // the start-anchored model parse would skip (the very reason the _00 enumeration misses them).
        msbe::ParseResult r = msbe::parse_msb(msb.data(), msb.size(), /*resident=*/false,
                                              /*blobBase=*/0, /*wantAssets=*/true,
                                              /*wantEnemies=*/false, /*wantRegions=*/false,
                                              /*crossTileAssets=*/true);
        if (!r.ok) continue;
        ++scanned;
        for (const auto &as : r.assets)
        {
            if (!wanted.count(as.aegRow) || as.entityId == 0) continue;
            DiskCollectible c;
            c.aegRow = as.aegRow;
            c.entityId = as.entityId;
            // Marker tile = the asset's TRUE fine tile, from the cross-tile part-name prefix
            // "m{AA}_{BB}_{CC}_00-…" — the supertile stores the part RELATIVE to that fine tile's
            // origin, so grid(fine)+pos lands correctly (the bake parses the same prefix,
            // generate_seal_puzzles.py). Fall back to the LOD file's own tile if there's no prefix.
            int fa = a, fx = x, fz = z, flod = 0;
            if (std::sscanf(as.name.c_str(), "m%d_%d_%d_%d", &fa, &fx, &fz, &flod) != 4)
            { fa = a; fx = x; fz = z; }
            c.area = (uint8_t)fa;
            c.gx = (uint8_t)fx;
            c.gz = (uint8_t)fz;
            c.posX = as.pos[0];
            c.posY = as.pos[1];
            c.posZ = as.pos[2];
            c.name = as.name;
            c.modelName = as.modelName;
            out.push_back(std::move(c));
        }
    }
    spdlog::info("[LOOTDISK] LOD feature-asset scan: {} placements in non-_00 tiles ({} tiles parsed)",
                 (int)out.size(), scanned);
    return out;
}

std::vector<DiskTreasure> load_lod_treasures()
{
    std::vector<DiskTreasure> out;
    fs::path dir = disk_loot_dir();
    if (dir.empty()) return out;
    msbe::OodleDecompressFn oodle = resolve_oodle();
    int scanned = 0;
    std::error_code ec;
    for (auto &de : fs::directory_iterator(dir, ec))
    {
        if (!de.is_regular_file(ec)) continue;
        const fs::path &p = de.path();
        std::string name = p.filename().string();
        std::string lower = name;
        for (char &c : lower) c = (char)std::tolower((unsigned char)c);
        if (lower.size() < 8 || lower.compare(lower.size() - 8, 8, ".msb.dcx") != 0) continue;
        std::string stem = name.substr(0, name.size() - 8);
        int a = 0, x = 0, z = 0, lod = -1;
        if (std::sscanf(stem.c_str(), "m%d_%d_%d_%d", &a, &x, &z, &lod) != 4) continue;
        if (lod == 0 || lod == 99) continue;  // non-_00 LOD tiers only (_00 done by load_disk_treasures)
        std::vector<uint8_t> dcx = slurp(p);
        if (dcx.empty()) continue;
        bool krak = false;
        std::vector<uint8_t> msb = msbe::dcx_decompress(dcx.data(), dcx.size(), &krak, oodle);
        if (msb.empty()) continue;
        // Treasures are parsed unconditionally; no asset/enemy/region sections needed.
        msbe::ParseResult r = msbe::parse_msb(msb.data(), msb.size(), /*resident=*/false,
                                              /*blobBase=*/0, /*wantAssets=*/false,
                                              /*wantEnemies=*/false, /*wantRegions=*/false);
        if (!r.ok) continue;
        ++scanned;
        for (const auto &t : r.treasures)
        {
            if (t.partIndex < 0 || t.itemLotId == 0) continue;  // item-glow / no part
            // Drop inert DummyAsset placements (same rule as the _00 scan).
            if (t.partType == msbe::PART_DUMMY_ASSET && t.entityId == 0 && !t.entityGroup) continue;
            // CROSS-TILE only: the part name must carry an "m{AA}_{BB}_{CC}_00-…" prefix pointing to a
            // DIFFERENT fine tile than this LOD file. A same-tile-named part in a LOD tier is just a
            // lower-detail copy of a _00 placement → skip it (the _00 scan owns it). The prefix tile is
            // where the bake places the marker (the supertile stores the part relative to that origin).
            int fa = 0, fx = 0, fz = 0, flod = 0;
            if (std::sscanf(t.partName.c_str(), "m%d_%d_%d_%d", &fa, &fx, &fz, &flod) != 4) continue;
            if (fa == a && fx == x && fz == z) continue;  // same-tile LOD copy, not cross-tile
            DiskTreasure d;
            d.lotId = t.itemLotId;
            d.area = (uint8_t)fa;
            d.gx = (uint8_t)fx;
            d.gz = (uint8_t)fz;
            d.posX = t.pos[0];
            d.posZ = t.pos[2];
            out.push_back(d);
        }
    }
    spdlog::info("[LOOTDISK] LOD treasure scan: {} cross-tile LOD treasures ({} non-_00 tiles parsed)",
                 (int)out.size(), scanned);
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

std::vector<DiskEmevd> load_emevd_awards(
    const std::unordered_set<uint32_t> &knownEntities,
    const std::unordered_map<uint32_t, std::unordered_set<uint32_t>> &entitiesByTile)
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
    int parsed = 0, kraks = 0, direct = 0, per_tile_award = 0;
    // Mechanism B inputs, accumulated across all files: flag→lot (from common.emevd's
    // RunEvent(1200)) and every SetEventFlag(.,1) event's flags+entity candidates.
    std::unordered_map<uint32_t, uint32_t> flag_to_lot;
    // Boss-reward (90005860/61/80) defeatFlag→(baseLot, bossEntity@16) binds, accumulated from EVERY
    // map (the binds are per-map, unlike the common-only RunEvent(1200) ones). Joined to a boss entity
    // below — defeatFlag first, then the explicit bossEntity@16, then a setter-candidate fallback.
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> boss_flag_to_lot;  // flag → (lot, entity)
    std::vector<msbe::EmevdSetter> setters;
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
        msbe::EmevdParse p = msbe::parse_emevd_full(evd.data(), evd.size());
        // Direct template awards: allowAsset=true (some are asset-anchored — Blaidd's spirit ash,
        // the scarab-cluster runes) + carry the loose-anchor fallback windows.
        for (const auto &a : p.direct)
            out.push_back({a.entityId, a.lotId, /*lotType=*/1, /*bossReward=*/false,
                           /*allowAsset=*/true, a.anchors});
        direct += (int)p.direct.size();
        // Per-tile enemy-death awards (callee >= 1e9, NOT a kEmevdTemplate): join entity→disk-ENEMY
        // pos like a direct award (lotType 1, fallback to _enemy downstream). The enemy join filters
        // asset-entity chests; the lot-coverage dedup in build_disk_emevd_markers drops covered lots.
        // perTile enemy-death awards: allowAsset=FALSE — strictly entity/anchors→ENEMY (an asset-join
        // here re-introduces the ~395 asset-entity-chest over-match, see [[nobake-coverage-scoreboard]]).
        // The lot@idx(n-1) "kill-the-group" variant carries loose anchors (entity 0) resolved
        // enemy-only by the join.
        for (const auto &a : p.perTileEnemyAward)
            out.push_back({a.entityId, a.lotId, /*lotType=*/1, /*bossReward=*/false,
                           /*allowAsset=*/false, a.anchors});
        per_tile_award += (int)p.perTileEnemyAward.size();
        // flag→lot only from common.emevd (the engine binding lives there; restricting
        // matches the bake and avoids a stray per-map RunEvent(1200) re-binding a flag).
        if (lower == "common.emevd.dcx")
            for (const auto &fl : p.runEvent1200) flag_to_lot[fl.first] = fl.second;
        // Boss-reward binds come from EVERY map's emevd (each dungeon inits 90005860 with its own
        // boss flag + lot), so collect them unconditionally.
        for (const auto &fl : p.bossFlagLot) boss_flag_to_lot[fl.flag] = {fl.lot, fl.entity};
        // Stamp each setter with its emevd file's tile (area<<16|gx<<8|gz) so the boss-candidate
        // resolution below stays map-scoped. "m30_12_00_00.emevd.dcx" → (30,12,0); files that
        // aren't a single tile (e.g. "common.emevd.dcx", "m60.emevd.dcx") leave mapTile=0 and fall
        // back to the global set (their setters are overworld-common, handled by other mechanisms).
        uint32_t setterTile = 0;
        {
            int a = 0, x = 0, z = 0, lod = 0;
            if (std::sscanf(name.c_str(), "m%d_%d_%d_%d", &a, &x, &z, &lod) == 4 &&
                a >= 0 && a < 256 && x >= 0 && x < 256 && z >= 0 && z < 256)
                setterTile = ((uint32_t)a << 16) | ((uint32_t)x << 8) | (uint32_t)z;
        }
        for (auto &s : p.setters) { s.mapTile = setterTile; setters.push_back(std::move(s)); }
        ++parsed;
        spdlog::debug("[LOOTDISK] {} -> {} direct awards", name, p.direct.size());
    }

    // Resolve a setter's referenced boss entity, intersecting its candidates with the entities of
    // its OWN tile (entitiesByTile[s.mapTile]) when that tile is known, else the global set. Scoping
    // to the tile is what stops a numerically-lower boss-like EntityID from a neighbouring dungeon
    // (present in the global set, coincidentally a 4-byte window here) from being picked — the bug
    // that mislocated ~23 per-dungeon Rune Pieces. Boss-preferred (eid%1000 ∈ 800..899), else lowest.
    auto resolve_boss = [&](const msbe::EmevdSetter &s) -> uint32_t {
        const std::unordered_set<uint32_t> *scope = &knownEntities;
        if (s.mapTile)
        {
            auto mit = entitiesByTile.find(s.mapTile);
            if (mit != entitiesByTile.end()) scope = &mit->second;
        }
        uint32_t boss = 0, any = 0;
        for (uint32_t c : s.candidates)
        {
            if (!scope->count(c)) continue;
            uint32_t last3 = c % 1000;
            if (last3 >= 800 && last3 <= 899) { if (!boss || c < boss) boss = c; }
            else if (!any || c < any) any = c;
        }
        return boss ? boss : any;
    };

    // Mechanism B: resolve each setter event whose flag is bound to a lot → pick the boss
    // entity it references (boss-preferred eid%1000 ∈ 800..899, else lowest), dedup by
    // (entity,lot). Lot resolves via ItemLotParam_enemy (lotType 2).
    int ev1200 = 0, ev1200_no_entity = 0;
    std::unordered_set<uint64_t> seen;  // (entity<<32 | lot)
    for (const msbe::EmevdSetter &s : setters)
    {
        for (uint32_t flag : s.flags)
        {
            auto it = flag_to_lot.find(flag);
            if (it == flag_to_lot.end()) continue;
            uint32_t lot = it->second;
            uint32_t chosen = resolve_boss(s);  // candidate ∩ this tile's MSB entities, boss-preferred
            if (!chosen) { ++ev1200_no_entity; continue; }
            uint64_t key = ((uint64_t)chosen << 32) | lot;
            if (!seen.insert(key).second) continue;
            out.push_back({chosen, lot, /*lotType=*/2, /*bossReward=*/false,
                           /*allowAsset=*/false, {}});
            ++ev1200;
        }
    }
    // Boss-reward join (90005860/61/80): SAME setter→entity machinery, but keyed on the per-map
    // boss defeatFlag→baseLot binds. The award's lot is a BASE (_map, lotType 1); bossReward=true
    // tells the marker pass to walk the chain (base+1/+2) for the Rune/Ember Piece and emit it
    // under the Reforged category (the rune/ember sibling suppression is lifted for these).
    // Reforged convention (~83/92 binds): the boss defeatFlag IS the boss's MSB EntityID, so use it
    // directly. The minority without flag==entity fall back to (1) the EXPLICIT bossEntity@16 the init
    // carries — what the offline bake reads (extract_all_items.py:651) — which recovers the 9 overworld
    // field-boss binds where defeatFlag != EntityID (c4980/c3150/c4950 + cross-tile c4503; flag==entity
    // for all 83 binds where both resolve, so flag-first never shifts a validated marker — verified by
    // tools/_probe_boss_piece_entity.py), then (2) a map-scoped setter-event candidate join. Loop the
    // BINDS (not setters) so flag-as-entity is tried first.
    int boss_ev = 0, boss_no_entity = 0, boss_via_entity16 = 0;
    for (const auto &fl : boss_flag_to_lot)
    {
        const uint32_t flag = fl.first, lot = fl.second.first, entity16 = fl.second.second;
        uint32_t chosen = knownEntities.count(flag) ? flag : 0;  // defeatFlag == boss EntityID
        if (!chosen && entity16)  // explicit bossEntity@16 (bake path) — authoritative over the setter
        {                         // heuristic, even when it resolves only in a LOD tile (8 of 9 are _00
            chosen = entity16;    // enemies; the 1 cross-tile c4503 is recovered by the LOD-award scan
            ++boss_via_entity16;  // in map_entry_layer.cpp, which now includes bossReward entities).
        }
        if (!chosen)
            for (const msbe::EmevdSetter &s : setters)  // fallback: setter that sets this flag
            {
                bool sets = false;
                for (uint32_t f : s.flags) if (f == flag) { sets = true; break; }
                if (!sets) continue;
                chosen = resolve_boss(s);  // map-scoped (same as the ev1200 join)
                if (chosen) break;
            }
        if (!chosen) { ++boss_no_entity; continue; }
        uint64_t key = ((uint64_t)chosen << 32) | lot;
        if (!seen.insert(key).second) continue;
        out.push_back({chosen, lot, /*lotType=*/1, /*bossReward=*/true,
                       /*allowAsset=*/true, {}});
        ++boss_ev;
    }
    spdlog::info("[LOOTDISK] {} EMEVD files parsed; {} direct + {} per-tile-enemy + {} event-1200 + {} "
                 "boss-reward awards ({} flag→lot binds, {} boss-flag binds, {} setters, {} "
                 "ev1200-no-entity, {} boss-no-entity, {} boss-via-entity@16); {} KRAK skipped",
                 parsed, direct, per_tile_award, ev1200, boss_ev, (int)flag_to_lot.size(),
                 (int)boss_flag_to_lot.size(), (int)setters.size(), ev1200_no_entity, boss_no_entity,
                 boss_via_entity16, kraks);
    return out;
}

std::unordered_map<uint32_t, uint32_t> load_emevd_world_feature_flags(
    std::unordered_map<uint32_t, uint32_t> *paintings_out,
    std::vector<msbe::GestureRef> *gestures_out)
{
    std::unordered_map<uint32_t, uint32_t> out;
    ensure_map_dir_resolved();
    fs::path mapStudio = disk_loot_dir();
    fs::path evdir = resolve_event_dir(mapStudio);
    if (evdir.empty())
    {
        spdlog::warn("[LOOTDISK] no event\\ dir for World-feature flags — graying deferred");
        return out;
    }
    spdlog::info("[LOOTDISK] reading World-feature graying flags from EMEVD {}", evdir.string());

    msbe::OodleDecompressFn oodle = resolve_oodle();  // KRAK events (unmodified vanilla)
    int parsed = 0, kraks = 0;
    std::error_code ec;
    for (auto &de : fs::directory_iterator(evdir, ec))
    {
        if (!de.is_regular_file(ec)) continue;
        std::string name = de.path().filename().string();
        std::string lower = name;
        for (char &c : lower) c = (char)std::tolower((unsigned char)c);
        if (lower.size() < 10 || lower.compare(lower.size() - 10, 10, ".emevd.dcx") != 0)
            continue;
        std::vector<uint8_t> dcx = slurp(de.path());
        if (dcx.empty()) { spdlog::warn("[LOOTDISK] read failed: {}", name); continue; }
        bool krak = false;
        std::vector<uint8_t> evd = msbe::dcx_decompress(dcx.data(), dcx.size(), &krak, oodle);
        if (evd.empty()) { if (krak) ++kraks; continue; }
        // entity→flag; first writer wins (phase-variant events _00/_10 share the same entity).
        for (const auto &ef : msbe::parse_emevd_flag_awards(evd.data(), evd.size()))
            out.emplace(ef.first, ef.second);
        // Painting collection events (entity→flag 580000-580199) from the SAME decompressed blob.
        if (paintings_out)
            for (const auto &pf : msbe::parse_emevd_paintings(evd.data(), evd.size()))
                paintings_out->emplace(pf.first, pf.second);
        // Gesture-spawn refs (template 90005570) from the SAME blob (one per call; dedup at the pass).
        if (gestures_out)
            for (const auto &gr : msbe::parse_emevd_gestures(evd.data(), evd.size()))
                gestures_out->push_back(gr);
        ++parsed;
    }
    spdlog::info("[LOOTDISK] World-feature flags: {} entity→flag from {} EMEVD files ({} KRAK skipped)",
                 (int)out.size(), parsed, kraks);
    if (paintings_out)
        spdlog::info("[LOOTDISK] World-feature flags: {} painting events (entity→flag 580000-580199)",
                     (int)paintings_out->size());
    if (gestures_out)
        spdlog::info("[LOOTDISK] World-feature flags: {} gesture-spawn refs (template 90005570)",
                     (int)gestures_out->size());
    return out;
}
} // namespace goblin::worldmap
