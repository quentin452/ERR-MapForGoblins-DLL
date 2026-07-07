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
// FUN_1406260e0(worldInfo, int* mapId, u32 p3, u8 p4, void* posData) — the spawn/warp applier.
// posData is the 5th arg (stack under MS x64); we treat it opaquely and copy 16 bytes for the rescue.
using ApplyFn = void (*)(void *worldInfo, int *mapId, uint32_t p3, uint32_t p4, void *posData);
ApplyFn g_orig = nullptr;

std::atomic<bool> g_installed{false};
std::atomic<bool> g_armed{false};       // area==0 → substitute the safe target
std::atomic<bool> g_verbose{true};      // per-call logging (loud during diagnosis)

// Safe rescue target. Default map = The First Step (m60_42_35 = 0x3C2A2300). The position is only
// valid once captured from a real applier call (pos frame is opaque) — g_safe_pos_valid gates its use.
std::atomic<uint32_t> g_safe_map{0x3C2A2300};
std::atomic<bool> g_capture_next{false};
std::mutex g_mtx;
bool g_safe_pos_valid = false;
uint8_t g_safe_pos[16] = {};

struct Seen { uint32_t mapId; int area; uint64_t pos8; bool rescued; };
std::deque<Seen> g_log;                 // last N applier calls (for `status`)

inline int area_of(uint32_t mapId) { return (int)((mapId >> 24) & 0xff); }

// Read *mapId + posData[0..15] under SEH (posData may be short/unmapped mid-load).
__declspec(noinline) static bool read_spawn(int *mapId, void *posData, uint32_t *out_map, uint64_t *out_pos8,
                                            uint8_t out_pos16[16])
{
    __try
    {
        *out_map = mapId ? (uint32_t)*mapId : 0;
        if (posData)
        {
            std::memcpy(out_pos16, posData, 16);
            *out_pos8 = *reinterpret_cast<uint64_t *>(posData);
        }
        else { std::memset(out_pos16, 0, 16); *out_pos8 = 0; }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Overwrite *mapId + posData with the safe target under SEH. Returns false if the write faults.
__declspec(noinline) static bool write_spawn(int *mapId, void *posData, uint32_t safe_map,
                                             const uint8_t *safe_pos, bool have_pos)
{
    __try
    {
        if (mapId) *mapId = (int)safe_map;
        if (have_pos && posData) std::memcpy(posData, safe_pos, 16);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void record(uint32_t mapId, int area, uint64_t pos8, bool rescued)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_log.push_back({mapId, area, pos8, rescued});
    if (g_log.size() > 16) g_log.pop_front();
}

void hk_apply(void *worldInfo, int *mapId, uint32_t p3, uint32_t p4, void *posData)
{
    uint32_t m = 0; uint64_t pos8 = 0; uint8_t pos16[16] = {};
    bool ok = read_spawn(mapId, posData, &m, &pos8, pos16);
    int area = ok ? area_of(m) : -1;
    bool rescued = false;

    if (ok)
    {
        // Capture a KNOWN-GOOD spawn (valid area) as the rescue target when asked.
        if (area != 0 && g_capture_next.load())
        {
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_safe_map.store(m);
                std::memcpy(g_safe_pos, pos16, 16);
                g_safe_pos_valid = true;
            }
            g_capture_next.store(false);
            spdlog::warn("[LOADRESCUE] captured safe target: map={:#010x} pos8={:#018x}", m, pos8);
        }

        // The rescue: an INVALID (area 0) incoming spawn = the poisoned-save case → substitute.
        if (g_armed.load() && area == 0)
        {
            uint32_t safe_map; uint8_t safe_pos[16]; bool have_pos;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                safe_map = g_safe_map.load();
                have_pos = g_safe_pos_valid;
                std::memcpy(safe_pos, g_safe_pos, 16);
            }
            rescued = write_spawn(mapId, posData, safe_map, safe_pos, have_pos);
            spdlog::warn("[LOADRESCUE] INVALID spawn (area 0, map={:#010x}) → RESCUE to map={:#010x} "
                         "pos={} ({})", m, safe_map, have_pos ? "captured" : "MAP-ONLY(no pos yet)",
                         rescued ? "written" : "WRITE FAILED");
        }
    }

    if (g_verbose.load())
        spdlog::warn("[LOADRESCUE] applier map={:#010x} area={} pos8={:#018x}{}",
                     m, area, pos8, rescued ? "  [RESCUED]" : "");
    record(ok ? m : 0, area, pos8, rescued);

    g_orig(worldInfo, mapId, p3, p4, posData);
}
}  // namespace

void install()
{
    if (g_installed.exchange(true)) return;
    void *fn = modutils::scan<void>({.aob = goblin::sig::SPAWN_APPLIER});
    if (!fn)
    {
        spdlog::warn("[LOADRESCUE] SPAWN_APPLIER AOB not found — load-rescue disabled");
        return;
    }
    try
    {
        modutils::hook(fn, reinterpret_cast<void *>(hk_apply), reinterpret_cast<void **>(&g_orig));
        spdlog::info("[LOADRESCUE] applier hooked @ {} (log-only; `load_rescue on` to arm the "
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

    if (sub == "on") { g_armed.store(true); return "ok load_rescue armed (area 0 → safe target)"; }
    if (sub == "off") { g_armed.store(false); return "ok load_rescue disarmed (log-only)"; }
    if (sub == "verbose")
    {
        g_verbose.store(arg != "0");
        return std::string("ok load_rescue verbose=") + (g_verbose.load() ? "1" : "0");
    }
    if (sub == "capture")
    {
        g_capture_next.store(true);
        return "ok load_rescue: will capture the next VALID applier call (warp/cross a tile) as the safe target";
    }
    if (sub == "set")
    {
        try { g_safe_map.store((uint32_t)std::stoul(arg, nullptr, 0)); }
        catch (...) { return "err usage: load_rescue set <mapHex>"; }
        char b2[64]; std::snprintf(b2, sizeof(b2), "ok safe map=%#010x (pos still needs capture)",
                                   g_safe_map.load());
        return std::string(b2);
    }
    // status (default)
    std::lock_guard<std::mutex> lk(g_mtx);
    char head[192];
    std::snprintf(head, sizeof(head),
                  "ok load_rescue installed=%d armed=%d verbose=%d safe_map=%#010x safe_pos=%s | last %d:",
                  g_installed.load() ? 1 : 0, g_armed.load() ? 1 : 0, g_verbose.load() ? 1 : 0,
                  g_safe_map.load(), g_safe_pos_valid ? "captured" : "none", (int)g_log.size());
    std::string out = head;
    for (auto it = g_log.rbegin(); it != g_log.rend(); ++it)
    {
        char b2[80];
        std::snprintf(b2, sizeof(b2), " [map=%#010x a=%d%s]", it->mapId, it->area,
                      it->rescued ? " RESCUED" : "");
        out += b2;
    }
    return out;
}
}  // namespace goblin::load_rescue
