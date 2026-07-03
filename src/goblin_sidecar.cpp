#include "goblin_sidecar.hpp"

#include "goblin_config.hpp"  // config::sidecarSave

#include <mini/ini.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace fs = std::filesystem;

namespace goblin::sidecar
{
namespace
{
std::mutex g_mtx;
fs::path g_save_path;      // the game's real save file (ER0000.err / .sl2)
fs::path g_mfg_path;       // <save>.mfg — our sidecar
bool g_loaded = false;     // a load() has run for the current path
bool g_dirty = false;      // in-memory state changed since the last save

std::set<uint32_t> g_flags;
std::unordered_map<std::string, std::string> g_kv;
std::string g_guid;        // self-stamped binding id (forward-compat; identity RE is Phase 1c)

// A stable-ish id for binding the sidecar to a save. Character-identity RE (steam id + slot)
// is deferred (Phase 1c); until then the sidecar binds by living next to the save file, and
// this guid is stamped for the later identity cross-check. Uniqueness across saves on one
// machine is enough here — QPC ^ tick ^ pid, hex.
std::string make_guid()
{
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    uint64_t v = (uint64_t)qpc.QuadPart ^ (GetTickCount64() << 1) ^
                 ((uint64_t)GetCurrentProcessId() << 32);
    char b[24];
    std::snprintf(b, sizeof(b), "%016llx", (unsigned long long)v);
    return b;
}

// Does the path look like an ER save file we should shadow? ER0000.sl2 (vanilla),
// ER0000.err (ERR ME3 redirect), co-op variants — match the `.sl2` / `.err` extension
// on a basename starting "ER" (case-insensitive), skipping `.bak` backups.
bool is_er_save(const fs::path &p)
{
    std::string ext = p.extension().string();
    for (auto &c : ext) c = (char)towlower(c);
    if (ext != ".sl2" && ext != ".err") return false;
    std::string stem = p.stem().string();
    return stem.size() >= 2 && (stem[0] == 'E' || stem[0] == 'e') &&
           (stem[1] == 'R' || stem[1] == 'r');
}

std::vector<std::string> split_csv(const std::string &s)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ','))
        if (!tok.empty()) out.push_back(tok);
    return out;
}

// Serialize the current state to `ini` (caller holds the lock).
void to_ini(mINI::INIStructure &ini)
{
    ini["meta"]["version"] = "1";
    ini["meta"]["guid"] = g_guid;
    ini["meta"]["savefile"] = g_save_path.filename().string();
    std::string flags;
    for (uint32_t f : g_flags)
    {
        if (!flags.empty()) flags += ",";
        flags += std::to_string(f);
    }
    ini["flags"]["custom"] = flags;
    // kv rows live in their own section (mINI lowercases keys — acceptable for our own keys).
    ini["kv"].clear();
    for (auto &[k, v] : g_kv) ini["kv"][k] = v;
}

// Populate state from `ini` (caller holds the lock).
void from_ini(const mINI::INIStructure &ini)
{
    g_flags.clear();
    g_kv.clear();
    if (ini.has("meta") && ini.get("meta").has("guid"))
        g_guid = ini.get("meta").get("guid");
    if (g_guid.empty()) g_guid = make_guid();
    if (ini.has("flags") && ini.get("flags").has("custom"))
        for (auto &tok : split_csv(ini.get("flags").get("custom")))
        {
            try { g_flags.insert((uint32_t)std::stoul(tok)); } catch (...) {}
        }
    if (ini.has("kv"))
        for (auto &[k, v] : ini.get("kv"))
            g_kv[k] = v;
}

bool save_locked()
{
    if (g_mfg_path.empty()) return false;
    if (g_guid.empty()) g_guid = make_guid();
    mINI::INIStructure ini;
    to_ini(ini);
    // Atomic: generate to a temp sibling, then rename over the target so a crash mid-write
    // never leaves a truncated .mfg. mINI::generate(true) pretty-prints; write to temp path.
    fs::path tmp = g_mfg_path;
    tmp += ".tmp";
    try
    {
        mINI::INIFile f(tmp.string());
        if (!f.generate(ini, true)) return false;
        fs::rename(tmp, g_mfg_path);  // atomic on the same volume
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[SIDECAR] save failed: {}", e.what());
        std::error_code ec;
        fs::remove(tmp, ec);
        return false;
    }
    g_dirty = false;
    spdlog::info("[SIDECAR] saved {} ({} flags, {} kv)", g_mfg_path.string(), g_flags.size(),
                 g_kv.size());
    return true;
}

bool load_locked()
{
    if (g_mfg_path.empty()) return false;
    std::error_code ec;
    if (!fs::exists(g_mfg_path, ec))
    {
        // No sidecar yet for this save — a fresh, empty store bound to this path.
        g_flags.clear();
        g_kv.clear();
        g_guid = make_guid();
        g_loaded = true;
        g_dirty = false;
        spdlog::info("[SIDECAR] no existing sidecar at {} — fresh store", g_mfg_path.string());
        return true;
    }
    mINI::INIStructure ini;
    mINI::INIFile f(g_mfg_path.string());
    if (!f.read(ini))
    {
        spdlog::warn("[SIDECAR] read failed: {}", g_mfg_path.string());
        return false;
    }
    from_ini(ini);
    g_loaded = true;
    g_dirty = false;
    spdlog::info("[SIDECAR] loaded {} ({} flags, {} kv)", g_mfg_path.string(), g_flags.size(),
                 g_kv.size());
    return true;
}
}  // namespace

void note_save_file_opened(const wchar_t *path, bool for_write)
{
    if (!config::sidecarSave || !path) return;
    fs::path p;
    try { p = fs::path(path); } catch (...) { return; }
    if (!is_er_save(p)) return;

    std::lock_guard<std::mutex> lk(g_mtx);
    // First sighting (or the game switched save files): bind + load the matching sidecar.
    if (g_save_path != p)
    {
        g_save_path = p;
        g_mfg_path = p;
        g_mfg_path.replace_extension(".mfg");
        g_loaded = false;
        spdlog::info("[SIDECAR] save file {} -> sidecar {}", p.string(), g_mfg_path.string());
        load_locked();
    }
    // A write open = the game is saving → persist our sidecar alongside it.
    if (for_write && g_loaded)
        save_locked();
}

std::string sidecar_path_utf8()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_mfg_path.string();
}

void add_custom_flag(uint32_t flag_id)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_flags.insert(flag_id).second) g_dirty = true;
}

void remove_custom_flag(uint32_t flag_id)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_flags.erase(flag_id)) g_dirty = true;
}

bool has_custom_flag(uint32_t flag_id)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_flags.count(flag_id) != 0;
}

std::vector<uint32_t> custom_flags()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return {g_flags.begin(), g_flags.end()};
}

void set_kv(const std::string &key, const std::string &value)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_kv[key] = value;
    g_dirty = true;
}

std::string get_kv(const std::string &key)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_kv.find(key);
    return it == g_kv.end() ? std::string{} : it->second;
}

bool load()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return load_locked();
}

bool save()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return save_locked();
}

std::string status_line()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    std::string p = g_mfg_path.empty() ? "(none)" : g_mfg_path.string();
    return "path=" + p + " loaded=" + (g_loaded ? "1" : "0") +
           " flags=" + std::to_string(g_flags.size()) + " kv=" + std::to_string(g_kv.size()) +
           " dirty=" + (g_dirty ? "1" : "0");
}
}  // namespace goblin::sidecar
