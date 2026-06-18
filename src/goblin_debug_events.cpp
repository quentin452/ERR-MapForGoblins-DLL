#include "goblin_debug_events.hpp"

#include "modutils.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace goblin::debug_events
{
namespace
{
// SetEventFlag — same id-by-pointer convention as IsEventFlag (see
// goblin_markers.cpp resolve_flag_api): rcx = EventFlagMan*, rdx = uint32_t*
// flag id, r8b = value (0/1). The AOB's `41 0F B6 F8` (movzx edi,r8b) value
// capture is what distinguishes this SETTER from the getter (which lacks it).
// A 4th param is declared purely as a safety pad so the trampoline forwards r9
// faithfully if the real routine happens to take one — extra Win64 integer
// args are caller-cleaned and harmless when the callee ignores them.
using SetEventFlagFn = uint64_t (*)(void * /*flagman*/, uint32_t * /*flag_id*/,
                                    uint8_t /*value*/, uint64_t /*pad*/);

SetEventFlagFn g_orig_set_event_flag = nullptr;

// AOB for EventFlag_C1 (eldenring.exe), from Hexinton all-in-one CT v6.0.
constexpr const char *SET_EVENT_FLAG_AOB =
    "48 89 5C 24 08 48 89 74 24 18 57 48 83 EC 30 48 8B DA 41 0F B6 F8 "
    "8B 12 48 8B F1 85 D2 0F 84";

// Hot-path → drain-thread inbox. The detour only records (id,value) under a
// short lock and returns; the drain thread does the dedup + file I/O off the
// game's flag-set path. Mirrors the mod's "post intent, single owner applies"
// discipline. Capped so a stalled drain can't grow it without bound.
constexpr size_t INBOX_CAP = 8192;
std::mutex g_inbox_mtx;
std::vector<std::pair<uint32_t, uint8_t>> g_inbox;
std::atomic<uint64_t> g_dropped{0};

std::shared_ptr<spdlog::logger> g_log;

uint64_t hk_set_event_flag(void *flagman, uint32_t *flag_id, uint8_t value,
                           uint64_t pad)
{
    if (flag_id)
    {
        uint32_t id = *flag_id;
        std::lock_guard<std::mutex> lk(g_inbox_mtx);
        if (g_inbox.size() < INBOX_CAP)
            g_inbox.emplace_back(id, value);
        else
            g_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    return g_orig_set_event_flag(flagman, flag_id, value, pad);
}

void drain_loop()
{
    // Log each flag id only once per session (its first set) — the game sets a
    // huge number of flags, but each distinct id is interesting only once for
    // coverage discovery. Value is recorded so a later set-then-clear is visible
    // via the value column on that first sighting.
    std::unordered_set<uint32_t> seen;
    std::vector<std::pair<uint32_t, uint8_t>> batch;
    uint64_t last_dropped = 0;

    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        batch.clear();
        {
            std::lock_guard<std::mutex> lk(g_inbox_mtx);
            g_inbox.swap(batch);
        }

        for (auto &[id, value] : batch)
        {
            if (seen.insert(id).second)
                g_log->info("flag {} = {}", id, static_cast<int>(value));
        }

        uint64_t dropped = g_dropped.load(std::memory_order_relaxed);
        if (dropped != last_dropped)
        {
            g_log->warn("inbox overflow: {} flag-set events dropped (drain behind)",
                        dropped);
            last_dropped = dropped;
        }
    }
}
} // namespace

void initialize(const std::filesystem::path &log_path)
{
    try
    {
        g_log = spdlog::basic_logger_st("mfg-events", log_path.string(), true);
        g_log->set_pattern("[%H:%M:%S.%e] %v");
        g_log->flush_on(spdlog::level::info);
    }
    catch (const std::exception &e)
    {
        spdlog::error("[DEBUG-EVENTS] could not open log {}: {}", log_path.string(),
                      e.what());
        return;
    }

    void *fn = modutils::scan<void>({.aob = SET_EVENT_FLAG_AOB});
    if (!fn)
    {
        spdlog::warn("[DEBUG-EVENTS] SetEventFlag AOB not found (game patch?) — "
                     "observer disabled");
        return;
    }

    try
    {
        modutils::hook(fn, reinterpret_cast<void *>(&hk_set_event_flag),
                       reinterpret_cast<void **>(&g_orig_set_event_flag));
        // enable_hooks() (MH_ApplyQueued) already ran during setup_mod, so apply
        // again to activate this late-queued hook.
        modutils::enable_hooks();
    }
    catch (const std::exception &e)
    {
        spdlog::error("[DEBUG-EVENTS] hook install failed: {} — observer disabled",
                      e.what());
        return;
    }

    std::thread(drain_loop).detach();
    spdlog::info("[DEBUG-EVENTS] SetEventFlag observer active @ {} → logging to {}",
                 fn, log_path.string());
    g_log->info("=== SetEventFlag observer started (id = value, first set only) ===");
}

} // namespace goblin::debug_events
