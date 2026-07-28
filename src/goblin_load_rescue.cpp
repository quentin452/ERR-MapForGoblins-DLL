#include "goblin_load_rescue.hpp"

#include "modutils.hpp"
#include "re_signatures.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace goblin::load_rescue
{
namespace
{
// FUN_140627fc0(worldInfo, int* mapId, u32 subId) — the universal map-change SETTER. `*mapId` = the
// incoming MapId; area byte 0 = a poisoned save's unloadable spawn. MAP-ONLY rescue (this funnel has
// no position; fixing the map to a valid one un-wedges — the player then falls→dies→respawns at grace).
using SetMapFn = void (*)(void *worldInfo, int *mapId, uint32_t subId);
SetMapFn g_orig = nullptr;

std::atomic<bool> g_installed{false};
std::atomic<bool> g_armed{false};       // area==0 → substitute the safe map
// Per-call logging. Default OFF: the map setter fires on every streaming transition (~460 WARN lines
// in one play session, user 2026-07-28), which buries the events that actually matter in the log. The
// RESCUE itself still logs unconditionally, `load_rescue status` still replays the last 16 calls from
// the ring below, and `load_rescue verbose 1` turns the firehose back on for a diagnosis session.
std::atomic<bool> g_verbose{false};

// Safe rescue map. Default = The First Step (m60_42_35 = 0x3C2A2300).
std::atomic<uint32_t> g_safe_map{0x3C2A2300};
std::mutex g_mtx;

struct Seen { uint32_t mapId; int area; bool rescued; };
std::deque<Seen> g_log;                 // last N setter calls (for `status`)

inline int area_of(uint32_t mapId) { return (int)((mapId >> 24) & 0xff); }

// Read *mapId under SEH (may be unmapped mid-load).
__declspec(noinline) static bool read_map(int *mapId, uint32_t *out_map)
{
    __try { *out_map = mapId ? (uint32_t)*mapId : 0; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Overwrite *mapId with the safe map under SEH. Returns false if the write faults.
__declspec(noinline) static bool write_map(int *mapId, uint32_t safe_map)
{
    __try { if (mapId) *mapId = (int)safe_map; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void record(uint32_t mapId, int area, bool rescued)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_log.push_back({mapId, area, rescued});
    if (g_log.size() > 16) g_log.pop_front();
}

void hk_setmap(void *worldInfo, int *mapId, uint32_t subId)
{
    uint32_t m = 0;
    bool ok = read_map(mapId, &m);
    int area = ok ? area_of(m) : -1;
    bool rescued = false;

    // The rescue: an INVALID (area 0) incoming map = the poisoned-save case → substitute a valid map.
    if (ok && g_armed.load() && area == 0)
    {
        uint32_t safe_map = g_safe_map.load();
        rescued = write_map(mapId, safe_map);
        // re-read for the log (mapId now holds safe_map if the write stuck)
        spdlog::warn("[LOADRESCUE] INVALID map (area 0, {:#010x}) → RESCUE to {:#010x} ({})",
                     m, safe_map, rescued ? "written" : "WRITE FAILED");
    }

    if (g_verbose.load())
        spdlog::warn("[LOADRESCUE] setmap in={:#010x} area={} sub={}{}", m, area, subId,
                     rescued ? "  [RESCUED]" : "");
    record(ok ? m : 0, area, rescued);

    g_orig(worldInfo, mapId, subId);
}
}  // namespace

void install()
{
    if (g_installed.exchange(true)) return;
    void *fn = modutils::scan<void>({.aob = goblin::sig::MAP_SETTER});
    if (!fn)
    {
        spdlog::warn("[LOADRESCUE] MAP_SETTER AOB not found — load-rescue disabled");
        return;
    }
    try
    {
        modutils::hook(fn, reinterpret_cast<void *>(hk_setmap), reinterpret_cast<void **>(&g_orig));
        spdlog::info("[LOADRESCUE] map-setter hooked @ {} (log-only; `load_rescue on` to arm the "
                     "area-0 → First Step substitution)", fn);
    }
    catch (const std::exception &e)
    {
        spdlog::error("[LOADRESCUE] hook failed: {}", e.what());
    }
}

std::string command(const std::string &rest)
{
    // parse first token
    size_t b = rest.find_first_not_of(" \t");
    std::string sub = b == std::string::npos ? std::string{} : rest.substr(b);
    size_t e = sub.find_first_of(" \t");
    std::string arg = e == std::string::npos ? std::string{} : sub.substr(e + 1);
    if (e != std::string::npos) sub = sub.substr(0, e);

    if (sub == "on") { g_armed.store(true); return "ok load_rescue armed (area 0 → safe map)"; }
    if (sub == "off") { g_armed.store(false); return "ok load_rescue disarmed (log-only)"; }
    if (sub == "verbose")
    {
        g_verbose.store(arg != "0");
        return std::string("ok load_rescue verbose=") + (g_verbose.load() ? "1" : "0");
    }
    if (sub == "set")
    {
        try { g_safe_map.store((uint32_t)std::stoul(arg, nullptr, 0)); }
        catch (...) { return "err usage: load_rescue set <mapHex>"; }
        char b2[64]; std::snprintf(b2, sizeof(b2), "ok safe map=%#010x", g_safe_map.load());
        return std::string(b2);
    }
    // status (default)
    std::lock_guard<std::mutex> lk(g_mtx);
    char head[160];
    std::snprintf(head, sizeof(head),
                  "ok load_rescue installed=%d armed=%d verbose=%d safe_map=%#010x | last %d:",
                  g_installed.load() ? 1 : 0, g_armed.load() ? 1 : 0, g_verbose.load() ? 1 : 0,
                  g_safe_map.load(), (int)g_log.size());
    std::string out = head;
    for (auto it = g_log.rbegin(); it != g_log.rend(); ++it)
    {
        char b2[64];
        std::snprintf(b2, sizeof(b2), " [%#010x a=%d%s]", it->mapId, it->area,
                      it->rescued ? " RESCUED" : "");
        out += b2;
    }
    return out;
}
}  // namespace goblin::load_rescue
