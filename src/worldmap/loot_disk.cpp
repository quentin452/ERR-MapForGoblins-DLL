#include "loot_disk.hpp"

#include "dvdbnd_reader.hpp"
#include "goblin_logic.hpp"     // note/publish_active_event_flags — the gate-hardening flag set
#include "loot_open_probe.hpp"  // captured_path_for — read the game's own file before resolving
#include "msbe_parser.hpp"
#include "goblin_config.hpp"
#include "from/params.hpp"   // GameAreaParam — the game's own boss defeat-flag table

#include <spdlog/spdlog.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cctype>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <tuple>
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
fs::path                              g_walk_dir;      // the ancestor-walk's answer = the BASE install's MapStudio (2026-08-15)
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
} // namespace

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

    // 0) CAPTURED PATH FIRST (2026-08-14, mod-agnostic ground truth): if the game has opened
    //    this virtual file, read THAT exact file (the loader redirected below CreateFileW, so
    //    the captured path IS the active mod's real file). This fixes exotic mounts (GA's
    //    <root>/GA/) that the ancestor-walk below misses — it would silently read the vanilla
    //    install's copy instead. Falls through to the normal resolution if not captured yet.
    std::string captured = captured_path_for(rel_path);
    if (!captured.empty())
    {
        std::vector<uint8_t> cap = read_exact_file_decompressed(captured);
        if (!cap.empty())
        {
            spdlog::debug("[GAMEFILE] '{}' read from the game's own captured path: {}", rel_path,
                          captured);
            return cap;
        }
        spdlog::warn("[GAMEFILE] captured path read failed ({}), falling back to resolution",
                     captured);
    }
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

std::vector<uint8_t> read_loose_file_decompressed(const std::string &rel_path)
{
    // Captured path first (same mod-agnostic ground truth as read_game_file_decompressed).
    std::string captured = captured_path_for(rel_path);
    if (!captured.empty())
    {
        std::vector<uint8_t> cap = read_exact_file_decompressed(captured);
        if (!cap.empty())
            return cap;
    }
    // Loose (mod overlay / UXM base) ONLY — no packed dvdbnd fallback. Lets a
    // caller distinguish "the mod ships this file" from "only the base game has
    // it packed", so a mod's own data can be preferred over vanilla (used by the
    // English name index: prefer the mod's msgbnd, fill gaps from packed vanilla).
    fs::path full = resolve_root_file(fs::path(rel_path));
    if (full.empty()) return {};
    std::vector<uint8_t> raw = slurp(full);
    if (raw.size() < 4) return {};
    if (!(raw[0] == 'D' && raw[1] == 'C' && raw[2] == 'X' && raw[3] == 0))
        return raw;  // loose-uncompressed
    bool krak = false;
    return msbe::dcx_decompress(raw.data(), raw.size(), &krak, resolve_oodle());
}

std::vector<uint8_t> read_exact_file_decompressed(const std::string &exact_path)
{
    // Read + decompress a file at an EXACT resolved path — NO directory-resolution walk and NO
    // packed fallback. For the CreateFileW-captured map paths (resident_msb path source): the
    // game already resolved the path (ME3/UXM redirect BELOW CreateFileW), so it IS the active
    // mod's real file — re-resolving it through the ancestor-walk could silently re-read the
    // wrong install (the very bug this path source fixes).
    std::vector<uint8_t> raw = slurp(exact_path);
    if (raw.size() < 4)
    {
        spdlog::warn("[GAMEFILE] exact-path read failed ({} bytes): {}", raw.size(), exact_path);
        return {};
    }
    if (!(raw[0] == 'D' && raw[1] == 'C' && raw[2] == 'X' && raw[3] == 0))
        return raw;  // loose-uncompressed (plain .msb / .mapbnd)
    bool krak = false;
    std::vector<uint8_t> out = msbe::dcx_decompress(raw.data(), raw.size(), &krak, resolve_oodle());
    if (out.empty())
        spdlog::warn("[GAMEFILE] DCX decompress failed ({}): {}", krak ? "KRAK" : "DFLT", exact_path);
    return out;
}

std::vector<uint8_t> dcx_decompress_bytes(const uint8_t *data, size_t len, bool *isKrak)
{
    // Decompress a raw DCX blob in memory with the game's loaded Oodle (KRAK support).
    // For inner-BXF hkx.dcx that have no vpath (collision meshes live inside hkxbhd/hkxbdt),
    // so read_game_file_decompressed can't reach them — the offline C# reader slices the raw
    // KRAK blob out and hands it here (see docs/re/far_water_surface_disk_re_findings.md §8).
    if (!data || len < 4) return {};
    if (!(data[0] == 'D' && data[1] == 'C' && data[2] == 'X' && data[3] == 0))
    {
        if (isKrak) *isKrak = false;
        return std::vector<uint8_t>(data, data + len);  // not a DCX — pass through
    }
    bool krak = false;
    auto out = msbe::dcx_decompress(data, len, &krak, resolve_oodle());
    if (isKrak) *isKrak = krak;
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
            g_walk_dir = d;  // the ancestor-walk's answer = the BASE install's MapStudio (2026-08-15)
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
    std::error_code ec;
    fs::path dir = fs::path(full_path).lexically_normal().parent_path();  // ...\map\MapStudio
    if (dir.empty()) return;
    // Base-dir filter (2026-08-15): the WALK-FOUND dir is the BASE install's MapStudio (GA's
    // walk lands on E:\SteamLibrary). Opens from it are base tiles — the loader redirects only
    // the mod's files, so the base dir is NOT the mod's data. Ignoring them stops the dir
    // toggling on every zone crossing (the game streams base tiles AND mod tiles interleaved —
    // measured rebuild storm on GA: 183 bucket builds in ~90 s, avg 432 ms). Any OTHER parent
    // = the active mod's real MapStudio → redirect once and stick.
    {
        std::lock_guard<std::mutex> lk(g_dir_mtx);
        if (!g_walk_dir.empty() && dir == g_walk_dir) return;
    }
    // The game's own open is GROUND TRUTH and OVERRIDES a walk-found dir (2026-08-14): once the
    // game opens a real mod .msb.dcx we know the ACTUAL dir. Only kick the worker when the dir
    // CHANGED (the walk already kicked on the initial Found).
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(g_dir_mtx);
        if (g_resolved_dir != dir)
        {
            g_resolved_dir = dir;
            changed = true;
        }
    }
    const bool was_found = static_cast<DiskLootState>(g_state.load()) == DiskLootState::Found;
    g_state.store(static_cast<int>(DiskLootState::Found));
    if (changed)
    {
        spdlog::info("[LOOTDISK] map dir {} via the game's own open: {}", was_found ? "REDIRECTED" : "discovered",
                     dir.string());
        if (g_build_trigger) g_build_trigger();  // kick the worker now, not at the next overlay tick
    }
}

void set_build_trigger(void (*fn)()) { g_build_trigger = fn; }

std::vector<DiskTreasure> load_disk_treasures(std::vector<uint32_t> *droppedDummyLots,
                                              std::vector<DiskCollectible> *collectibles,
                                              std::vector<DiskEnemy> *enemies,
                                              std::vector<DiskRegion> *regions,
                                              std::vector<DiskObjAct> *objacts)
{
    std::vector<DiskTreasure> out;
    const bool wantAssets = collectibles != nullptr;
    const bool wantEnemies = enemies != nullptr;
    const bool wantRegions = regions != nullptr;
    const bool wantObjActs = objacts != nullptr;
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
                                              /*blobBase=*/0, wantAssets, wantEnemies, wantRegions,
                                              /*crossTileAssets=*/false, wantObjActs);
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
                e.talkId = en.talkId;
                e.entityId = en.entityId;
                e.area = (uint8_t)area;
                e.gx = (uint8_t)gx;
                e.gz = (uint8_t)gz;
                e.posX = en.pos[0];
                e.posY = en.pos[1];  // altitude for the above/below-player badge
                e.posZ = en.pos[2];
                e.name = en.name;
                e.modelName = en.modelName;
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

        if (wantObjActs)
        {
            for (const auto &o : r.objacts)
            {
                // Position comes ONLY from the resolved part — an event without one has no
                // place-able anchor (keeping it would drop a marker at tile-local (0,0)).
                if (o.partIndex < 0) continue;
                DiskObjAct d;
                d.objActParamId = o.objActParamId;
                d.entityId = o.objActEntityId ? o.objActEntityId : o.partEntityId;
                d.area = (uint8_t)area;
                d.gx = (uint8_t)gx;
                d.gz = (uint8_t)gz;
                d.posX = o.pos[0];
                d.posY = o.pos[1];  // altitude for the above/below-player badge
                d.posZ = o.pos[2];
                d.partName = o.partName;
                objacts->push_back(std::move(d));
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
            d.posY = t.pos[1];  // altitude for the above/below-player badge
            d.posZ = t.pos[2];  // Part+0x20 X/Z
            d.entityId = t.entityId;
            d.partName = t.partName;
            out.push_back(std::move(d));
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
    if (wantObjActs)
        spdlog::info("[LOOTDISK] {} ObjAct events parsed (Elevator / lever-lift candidates)",
                     objacts->size());
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
            // Marker tile = the enemy's TRUE fine tile, from the cross-tile part-name prefix
            // "m{AA}_{BB}_{CC}_00-…" — a cross-tile supertile (e.g. the "Snow Town" proxy
            // m60_24_28_01) stores the part RELATIVE to that fine tile's origin, so the file's
            // OWN tile (24,28) is off the drawn map. Parse the prefix like the sibling LOD passes
            // (load_lod_treasures / load_lod_feature_assets); fall back to the file tile only when
            // the part carries no prefix (a genuine own-tile LOD proxy). Without this, a scarab-
            // dropped award (e.g. White Shadow's Lure, lot 40524) landed at the supertile's own
            // off-map tile instead of its Mountaintops home.
            int fa = a, fx = x, fz = z, flod = 0;
            if (std::sscanf(en.name.c_str(), "m%d_%d_%d_%d", &fa, &fx, &fz, &flod) != 4)
            { fa = a; fx = x; fz = z; }
            e.area = (uint8_t)fa;
            e.gx = (uint8_t)fx;
            e.gz = (uint8_t)fz;
            e.posX = en.pos[0];
            e.posZ = en.pos[2];
            e.name = en.name;
            e.modelName = en.modelName;
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
        // Feed the ACTIVE event-flag set (fragment/story gate hardening, goblin_logic):
        // every SetEventFlag(., state=1) id the mod's own EMEVDs set. Published once at the
        // end of this pass — the gate tables honour a flag only when it's in this set.
        {
            std::vector<uint32_t> all_flags;
            for (auto &s : p.setters) all_flags.insert(all_flags.end(), s.flags.begin(), s.flags.end());
            goblin::note_active_event_flags(all_flags);
        }
        ++parsed;
        spdlog::debug("[LOOTDISK] {} -> {} direct awards", name, p.direct.size());
    }
    goblin::publish_active_event_flags();

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
    std::vector<msbe::GestureRef> *gestures_out,
    std::unordered_set<uint32_t> *portals_out)
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
        // Portal / sending-gate entities (warp template 90005605, entity@arg[2]) from the SAME blob.
        // Set-insert dedups the LOD _00/_10 duplicate inits automatically.
        if (portals_out)
            for (uint32_t ent : msbe::parse_emevd_portal_gates(evd.data(), evd.size()))
                portals_out->insert(ent);
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
    if (portals_out)
        spdlog::info("[LOOTDISK] World-feature flags: {} portal gates (warp template 90005605)",
                     (int)portals_out->size());
    return out;
}

std::vector<QuestNpcRuntime> load_quest_npcs()
{
    std::vector<QuestNpcRuntime> out;
    ensure_map_dir_resolved();
    fs::path evdir = resolve_event_dir(disk_loot_dir());
    if (evdir.empty())
    {
        spdlog::warn("[LOOTDISK] no event\\ dir for quest NPCs — deferred");
        return out;
    }
    spdlog::info("[LOOTDISK] reading quest NPCs (90005702) from EMEVD {}", evdir.string());

    msbe::OodleDecompressFn oodle = resolve_oodle();
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, size_t> byKey;  // (concluded,lo,hi) -> out idx
    int parsed = 0, kraks = 0, calls = 0;
    std::error_code ec;
    for (auto &de : fs::directory_iterator(evdir, ec))
    {
        if (!de.is_regular_file(ec)) continue;
        std::string lower = de.path().filename().string();
        for (char &c : lower) c = (char)std::tolower((unsigned char)c);
        if (lower.size() < 10 || lower.compare(lower.size() - 10, 10, ".emevd.dcx") != 0)
            continue;
        std::vector<uint8_t> dcx = slurp(de.path());
        if (dcx.empty()) continue;
        bool krak = false;
        std::vector<uint8_t> evd = msbe::dcx_decompress(dcx.data(), dcx.size(), &krak, oodle);
        if (evd.empty()) { if (krak) ++kraks; continue; }
        for (const auto &q : msbe::parse_emevd_quest_npcs(evd.data(), evd.size()))
        {
            ++calls;
            auto key = std::make_tuple(q.concluded, q.regLo, q.regHi);
            auto it = byKey.find(key);
            if (it == byKey.end())
            {
                byKey.emplace(key, out.size());
                out.push_back({q.concluded, q.regLo, q.regHi, {q.entity}});
            }
            else
            {
                auto &ents = out[it->second].entities;
                if (std::find(ents.begin(), ents.end(), q.entity) == ents.end())
                    ents.push_back(q.entity);
            }
        }
        ++parsed;
    }
    spdlog::info("[LOOTDISK] quest NPCs: {} grouped (from {} 90005702 calls over {} EMEVD files, {} KRAK skipped)",
                 (int)out.size(), calls, parsed, kraks);
    return out;
}

// Boss entity -> its DEFEAT event flag. Filled by the same EMEVD walk as the health-bar names
// below, from two sources that must be merged in this order of authority:
//   1. literal 2003[12] HandleBossDefeatAndDisplayBanner(entity, ...) -> flag = entity id
//   2. the common-func CALL SITES (parse_emevd_boss_defeat_calls) -> flag = X0_4, entity = X8_4
// (2) WINS: it is the only source for fights registered through a shared handler (the entity is a
// parameter there, invisible to the literal scan), and its X0_4 is the PERSISTENT flag where the
// entity id can be a transient one that common.emevd resets — see parse_emevd_boss_defeat_calls.
static std::unordered_map<uint32_t, uint32_t> &defeats()
{
    static std::unordered_map<uint32_t, uint32_t> m;
    return m;
}

// Where GameAreaParam says each boss stands. Same one-time pass as the defeat flags. Lets a fight
// that produced NO map marker still be placed in a region: the marker layer is the only other
// source of a region label, and 16 of 216 fights have no marker (deduped per tile, LOD-tile-only,
// or not an MSB Enemy part), which is exactly how 17 rows ended up under "(no region)".
static std::unordered_map<uint32_t, goblin::worldmap::BossArea> &boss_areas()
{
    static std::unordered_map<uint32_t, goblin::worldmap::BossArea> m;
    return m;
}

const std::unordered_map<uint32_t, uint32_t> &emevd_boss_bars()
{
    // Built by the marker-build worker; the returned map is read-only + stable afterwards, so only
    // the one-time fill needs guarding (the enemy-tag path on the present thread deliberately never
    // calls this — it goes through register_boss_bar_name instead).
    static std::mutex mtx;
    static std::unordered_map<uint32_t, uint32_t> cache;
    static bool loaded = false;
    std::lock_guard<std::mutex> lk(mtx);
    if (loaded) return cache;

    ensure_map_dir_resolved();
    fs::path evdir = resolve_event_dir(disk_loot_dir());
    if (evdir.empty())
    {
        // No map dir yet (disk source off, or discovery still Searching). Do NOT latch `loaded` —
        // a later call, once the CreateFileW fallback has revealed the dir, must retry.
        spdlog::warn("[LOOTDISK] no event\\ dir for boss bars — boss names fall back to the model band");
        return cache;
    }
    spdlog::info("[LOOTDISK] reading boss health bars (2003[11]) from EMEVD {}", evdir.string());

    msbe::OodleDecompressFn oodle = resolve_oodle();
    int parsed = 0, kraks = 0, calls = 0, defeat_calls = 0, defeat_sites = 0;
    std::vector<msbe::BossDefeatSite> literal_sites;  // resolved after the walk (needs `resettable`)
    std::unordered_set<uint32_t> resettable;          // flags common.emevd turns OFF
    std::error_code ec;
    for (auto &de : fs::directory_iterator(evdir, ec))
    {
        if (!de.is_regular_file(ec)) continue;
        std::string lower = de.path().filename().string();
        for (char &c : lower) c = (char)std::tolower((unsigned char)c);
        if (lower.size() < 10 || lower.compare(lower.size() - 10, 10, ".emevd.dcx") != 0)
            continue;
        std::vector<uint8_t> dcx = slurp(de.path());
        if (dcx.empty()) continue;
        bool krak = false;
        std::vector<uint8_t> evd = msbe::dcx_decompress(dcx.data(), dcx.size(), &krak, oodle);
        if (evd.empty()) { if (krak) ++kraks; continue; }
        for (const auto &b : msbe::parse_emevd_boss_bars(evd.data(), evd.size()))
        {
            ++calls;
            // A boss has several calls (show/hide, phase re-arms); they agree on the name, so
            // first-wins keeps one entry per ENCOUNTER without a second pass.
            cache.emplace(b.entityId, b.nameId);
        }
        // Defeat registrations ride on the SAME decompressed buffer — a separate pass would mean a
        // second walk of every EMEVD in the install (Oodle decompress included) for one extra field.
        // Literal registrations are BUFFERED, not resolved here: choosing the flag needs the reset
        // group, which lives in common.emevd and may be read after this file. Resolved below.
        for (auto &s : msbe::parse_emevd_boss_defeats(evd.data(), evd.size()))
        {
            ++defeat_calls;
            literal_sites.push_back(std::move(s));
        }
        if (lower.rfind("common.emevd", 0) == 0)
            for (uint32_t f : msbe::parse_emevd_flags_cleared(evd.data(), evd.size()))
                resettable.insert(f);
        for (const auto &c : msbe::parse_emevd_boss_defeat_calls(evd.data(), evd.size()))
        {
            ++defeat_sites;
            defeats()[c.entity] = c.flag;  // call site WINS (persistent flag; sees param'd handlers)
        }
        // Handler discovery: over common_func.emevd this names THIS install's shared defeat
        // handlers. Hardcoding them is what failed — the ids taken from an ERR decompile matched a
        // single call site on vanilla. Logged so the real ids can drive the call-site pass.
        if (lower.rfind("common_func", 0) == 0)
        {
            std::string ids;
            for (const auto &h : msbe::parse_emevd_defeat_handlers(evd.data(), evd.size()))
                ids += (ids.empty() ? "" : ", ") + std::to_string(h.eventId) + "(entity=" +
                       std::to_string(h.rawEntity) + ")";
            spdlog::info("[LOOTDISK] defeat handlers in {}: {}", lower,
                         ids.empty() ? "none" : ids);
        }
        ++parsed;
    }
    // ── Resolve the literal registrations now that the reset group is known ──────────────────────
    // Normal case: the entity id IS the persistent defeat flag. But a night/roaming boss's entity
    // sits in common.emevd's reset group (event 6901 turns 94 flags OFF), so trusting it would let
    // a beaten boss un-tick itself. In that case the persistent flag is one of the flags the SAME
    // event turns ON — and it is only accepted when exactly ONE candidate is itself non-resettable,
    // i.e. when the data leaves no choice to make. Anything ambiguous keeps the entity id and is
    // logged, so the next pass reads measurements rather than inheriting a guess.
    int reset_entities = 0, rescued = 0, ambiguous = 0;
    std::string rescue_log, ambiguous_log;
    for (const auto &s : literal_sites)
    {
        uint32_t flag = s.entity;
        if (resettable.count(s.entity))
        {
            ++reset_entities;
            std::vector<uint32_t> keep;
            for (uint32_t f : s.flagsOn)
                if (f != s.entity && !resettable.count(f)) keep.push_back(f);
            if (keep.size() == 1)
            {
                flag = keep[0];
                ++rescued;
                if (rescued <= 12)
                    rescue_log += (rescue_log.empty() ? "" : ", ") + std::to_string(s.entity) +
                                  "->" + std::to_string(flag);
            }
            else
            {
                ++ambiguous;
                if (ambiguous <= 12)
                {
                    ambiguous_log += (ambiguous_log.empty() ? "" : ", ") +
                                     std::to_string(s.entity) + "[";
                    for (size_t k = 0; k < keep.size(); ++k)
                        ambiguous_log += (k ? " " : "") + std::to_string(keep[k]);
                    ambiguous_log += "]";
                }
            }
        }
        defeats().emplace(s.entity, flag);  // never overrides a call site (assigned above)
    }
    spdlog::info("[LOOTDISK] defeat flags: {} resettable entities ({} rescued to a persistent flag, "
                 "{} ambiguous); reset group = {} flags", reset_entities, rescued, ambiguous,
                 (int)resettable.size());
    if (!rescue_log.empty())    spdlog::info("[LOOTDISK]   rescued: {}", rescue_log);
    if (!ambiguous_log.empty()) spdlog::info("[LOOTDISK]   ambiguous (kept entity): {}", ambiguous_log);

    // ── GameAreaParam: the GAME'S OWN declaration of each boss's defeat flag ─────────────────────
    // Row id = the boss ENTITY id, `defeatBossFlagId` @+0x44 = the flag to read. This outranks
    // everything mined from the EMEVD: for an ordinary boss it simply restates flag == entity, but
    // for a night/roaming boss it names the PERSISTENT flag the instruction could not
    // (1043370340 -> 1043370800, 1044320340 -> 1044320800, measured live 2026-07-27), and it covers
    // fights whose registration is parameterised and therefore invisible to a literal scan (the
    // Altus Tree Sentinel duo, 1041510800). A param, so it is live, mod-agnostic and free of every
    // parsing hazard that cost this feature three attempts. It also settles the "12-prefixed twin"
    // oddity: entity 1052380800 (Radahn) -> flag 1252380800, which is what EROverlay's curated list
    // carries — the param agrees with it, the instruction did not.
    //
    // The param was nearly written off as empty: probing row ids 0..399 found only stub rows 0/1.
    // It is keyed by ENTITY id — sampling could not see it, enumeration could (`param_rows`).
    //
    // Runs LAST, after every EMEVD-derived source, so it OVERRIDES them and so its own counters
    // measure something real: placed before the literal resolution it compared against a table that
    // was still almost empty and reported all 212 rows as new, which said nothing.
    {
        struct RawGameAreaRow { uint8_t b[0x58]; };  // defeatBossFlagId u32 @ +0x44
        int rows = 0, differ = 0, added = 0;
        try
        {
            for (auto [rowId, row] : from::params::get_param<RawGameAreaRow>(L"GameAreaParam"))
            {
                uint32_t flag = 0;
                std::memcpy(&flag, row.b + 0x44, sizeof(flag));
                if (!flag || (int32_t)flag <= 0) continue;   // stub rows (0/1/9999991/9999992)
                ++rows;
                const uint32_t entity = (uint32_t)rowId;
                BossArea ba;
                ba.area = row.b[0x54];
                ba.gx   = row.b[0x55];
                ba.gz   = row.b[0x56];
                std::memcpy(&ba.px, row.b + 0x48, sizeof(float));
                std::memcpy(&ba.pz, row.b + 0x50, sizeof(float));
                if (ba.area) boss_areas()[entity] = ba;
                auto it = defeats().find(entity);
                if (it == defeats().end()) ++added;
                else if (it->second != flag) ++differ;
                defeats()[entity] = flag;                    // the game's own word wins
            }
        }
        catch (...) { spdlog::warn("[LOOTDISK] GameAreaParam not readable — defeat flags stay EMEVD-only"); }
        spdlog::info("[LOOTDISK] GameAreaParam: {} boss rows ({} corrected an EMEVD-derived flag, "
                     "{} fights the EMEVD scan never saw)", rows, differ, added);
    }

    loaded = true;
    spdlog::info("[LOOTDISK] boss bars: {} entities named (from {} 2003[11] calls over {} EMEVD "
                 "files, {} KRAK skipped)", (int)cache.size(), calls, parsed, kraks);
    {
        int reflagged = 0;
        // distinct FLAGS = distinct fights (two entities can share one); sorted for the id dump
        std::vector<uint32_t> fights_v;
        std::unordered_set<uint32_t> fights;
        for (const auto &kv : defeats())
        {
            if (fights.insert(kv.second).second) fights_v.push_back(kv.second);
            if (kv.first != kv.second) ++reflagged;
        }
        std::sort(fights_v.begin(), fights_v.end());
        spdlog::info("[LOOTDISK] boss defeats: {} entities -> {} distinct fights (from {} literal "
                     "2003[12] calls + {} common-func call sites; {} entities carry a flag that is "
                     "NOT their own id)", (int)defeats().size(), (int)fights.size(), defeat_calls,
                     defeat_sites, reflagged);
        // Full sorted flag list, once per session. The COUNT alone can't be reconciled against
        // another tracker's boss list (EROverlay ships a curated 207-entry bosses.json): only the
        // ids say whether a difference is an extra of ours or an omission of theirs.
        std::string s;
        for (uint32_t f : fights_v) { if (!s.empty()) s += ","; s += std::to_string(f); }
        spdlog::info("[LOOTDISK] boss defeat ids: {}", s);
    }
    return cache;
}

uint32_t emevd_boss_defeat_flag(uint32_t entityId)
{
    if (!entityId) return 0;
    emevd_boss_bars();  // shared one-time EMEVD walk fills the defeat table too
    auto it = defeats().find(entityId);
    return it == defeats().end() ? 0u : it->second;
}

const std::unordered_map<uint32_t, uint32_t> &emevd_boss_defeats()
{
    emevd_boss_bars();
    return defeats();
}

bool boss_area_of(uint32_t entityId, BossArea &out)
{
    if (!entityId) return false;
    emevd_boss_bars();  // shared one-time pass fills the area table too
    auto it = boss_areas().find(entityId);
    if (it == boss_areas().end()) return false;
    out = it->second;
    return true;
}

uint32_t emevd_boss_bar_name_id(uint32_t entityId)
{
    if (!entityId) return 0;
    const auto &m = emevd_boss_bars();
    auto it = m.find(entityId);
    return it == m.end() ? 0u : it->second;
}

std::vector<esd::TalkShopRange> load_merchant_shop_ranges()
{
    // Session cache (2026-08-15): the ESD walk re-parses EVERY talkesdbnd of the install
    // (~3 s — bench build.disk_merchants) and used to re-pay that cost on EVERY bucket rebuild
    // (~30 s cadence; exposed loudly by GA's full loose talk dir). The talk ESDs are immutable
    // per session; key on the loose source dir (a capture redirect → exactly one re-run) + the
    // packed game dir.
    std::error_code ec;
    fs::path talkDir;
    const std::string capDir = captured_dir_for(".talkesdbnd.dcx");
    if (!capDir.empty())
    {
        talkDir = fs::path(capDir);  // the game's own talk dir (an NPC conversation happened)
    }
    else
    {
        // Derive script/talk from the RESOLVED MAP dir (2026-08-15): the game does NOT open
        // talk ESDs at boot, so the capture is empty at the first build — but the map dir IS
        // the game's own (capture-redirected to GA\map\MapStudio). The mod root is the
        // MapStudio dir's parent's parent: <root>/map/MapStudio → <root>/script/talk. Holds on
        // ERR (<root>/mod), GA (<root>/GA) and a UXM-unpacked vanilla (<game>) alike.
        ensure_map_dir_resolved();
        fs::path ms = disk_loot_dir();
        if (!ms.empty() && ms.filename() == "MapStudio")
        {
            fs::path cand = ms.parent_path().parent_path() / "script" / "talk";
            std::error_code ec2;
            if (fs::is_directory(cand, ec2)) talkDir = cand;
        }
        if (talkDir.empty())
            talkDir = resolve_root_file(fs::path("script") / "talk");
    }
    const fs::path gd = game_dir();
    static std::mutex                   mx;
    static std::vector<esd::TalkShopRange> cached;
    static std::string                   cached_key;
    static bool                          cached_valid = false;
    const std::string key = talkDir.string() + "|" + gd.string();
    {
        std::lock_guard<std::mutex> lk(mx);
        if (cached_valid && cached_key == key) return cached;
    }

    std::vector<esd::TalkShopRange> out;
    int skippedExpr = 0, looseBnds = 0, packedBnds = 0;
    std::unordered_set<std::string> done;  // stems parsed loose — shadow their packed twin
    msbe::OodleDecompressFn oodle = resolve_oodle();

    auto parse_bnd = [&](const std::vector<uint8_t> &bnd)
    {
        if (bnd.empty()) return false;
        auto r = esd::parse_talkbnd_shop_ranges(bnd.data(), bnd.size(), &skippedExpr);
        out.insert(out.end(), r.begin(), r.end());
        return true;
    };

    // 1) Loose script/talk dir — CAPTURED DIR FIRST (2026-08-14): the game opens its real talk
    //    ESD bnds through the loader's redirect (below CreateFileW), so the parent of a captured
    //    *.talkesdbnd.dcx IS the active mod's script/talk (GA mounts <root>/GA/, the walk
    //    misses it and falls back to vanilla). Falls back to the ancestor walk (mod overlay /
    //    UXM install) when nothing is captured yet.
    if (!talkDir.empty())
    {
        for (auto &de : fs::directory_iterator(talkDir, ec))
        {
            if (!de.is_regular_file(ec)) continue;
            std::string name = de.path().filename().string();
            constexpr const char *kExt = ".talkesdbnd.dcx";
            constexpr size_t kExtLen = 15;
            if (name.size() <= kExtLen ||
                name.compare(name.size() - kExtLen, kExtLen, kExt) != 0)
                continue;
            std::vector<uint8_t> dcx = slurp(de.path());
            if (dcx.empty()) continue;
            bool krak = false;
            if (parse_bnd(msbe::dcx_decompress(dcx.data(), dcx.size(), &krak, oodle)))
            {
                done.insert(name.substr(0, name.size() - kExtLen));
                ++looseBnds;
            }
        }
    }

    // 2) Packed dvdbnd probe for stems the mod does NOT ship loose. Candidate names come
    //    from the MSB tile listing — vanilla talk bnds are named like maps at three
    //    granularities (per-area m60_00_00_00, per-block m11_10_00_00, per-tile) — plus the
    //    common m00_00_00_00. A miss is a cheap in-memory BHD hash lookup (no log spam).
    std::unordered_set<std::string> candidates = {"m00_00_00_00"};
    ensure_map_dir_resolved();
    if (fs::path dir = disk_loot_dir(); !dir.empty())
        for (auto &de : fs::directory_iterator(dir, ec))
        {
            if (!de.is_regular_file(ec)) continue;
            std::string name = de.path().filename().string();
            int a = 0, x = 0, z = 0, lod = -1;
            if (std::sscanf(name.c_str(), "m%d_%d_%d_%d.msb.dcx", &a, &x, &z, &lod) != 4)
                continue;
            char stem[32];
            std::snprintf(stem, sizeof(stem), "m%02d_00_00_00", a);
            candidates.insert(stem);
            std::snprintf(stem, sizeof(stem), "m%02d_%02d_00_00", a, x);
            candidates.insert(stem);
            std::snprintf(stem, sizeof(stem), "m%02d_%02d_%02d_00", a, x, z);
            candidates.insert(stem);
        }
    if (!gd.empty())
        for (const std::string &stem : candidates)
        {
            if (done.count(stem)) continue;
            std::vector<uint8_t> raw =
                dvdbnd::read_packed_file(gd, "script/talk/" + stem + ".talkesdbnd.dcx");
            if (raw.size() < 4) continue;  // not in any archive (most probes)
            bool krak = false;
            if (parse_bnd(msbe::dcx_decompress(raw.data(), raw.size(), &krak, oodle)))
                ++packedBnds;
        }

    spdlog::info("[MERCHANTPINS] talk-ESD scan: {} OpenRegularShop(1:22) ranges from {} loose + {} "
                 "packed talkesdbnd ({} non-literal args skipped)",
                 (int)out.size(), looseBnds, packedBnds, skippedExpr);
    {
        std::lock_guard<std::mutex> lk(mx);
        cached = out;
        cached_key = key;
        cached_valid = true;
    }
    return out;
}
} // namespace goblin::worldmap
