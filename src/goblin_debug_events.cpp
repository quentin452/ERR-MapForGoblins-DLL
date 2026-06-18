#include "goblin_debug_events.hpp"

#include "goblin_inject.hpp"
#include "goblin_map_data.hpp"
#include "modutils.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace goblin::debug_events
{
namespace
{
// Shared sink. _mt: the flag drain thread AND the item detour (game thread) both
// write it, so it must be thread-safe.
std::shared_ptr<spdlog::logger> g_log;

// Every collect/reveal flag the mod's baked markers carry (textDisableFlagId1-8
// + clearedEventFlagId). A placed world lot's getItemFlagId IS its marker's
// textDisableFlagId, so when the game sets a collectible-shaped flag NOT in this
// set, the player collected a placed item the map has no marker for — the
// "missing placed item" signal. Built once at init; read-only after (drain reads).
std::unordered_set<uint32_t> g_known_collect_flags;

// ER map-instance flag id = `10 AA BB C DDD`; the thousands digit C is the flag
// TYPE. The mod's collect flags cluster on categories 0 and 7 (treasure/item);
// other categories are non-collectible events (doors/cutscenes/boss-phase), so a
// collectible-shaped flag is one in the map-instance range with cat 0 or 7.
constexpr uint32_t MAP_INSTANCE_MIN = 1'000'000'000u;
bool is_collectible_shaped(uint32_t id)
{
    if (id < MAP_INSTANCE_MIN)
        return false;
    uint32_t cat = (id / 1000u) % 10u;
    return cat == 0 || cat == 7;
}

void build_known_collect_flags()
{
    using namespace goblin::generated;
    for (size_t i = 0; i < MAP_ENTRY_COUNT; i++)
    {
        const auto &d = MAP_ENTRIES[i].data;
        const uint32_t fs[] = {d.clearedEventFlagId, d.textDisableFlagId1,
                               d.textDisableFlagId2, d.textDisableFlagId3,
                               d.textDisableFlagId4, d.textDisableFlagId5,
                               d.textDisableFlagId6, d.textDisableFlagId7,
                               d.textDisableFlagId8};
        for (uint32_t f : fs)
            if (f)
                g_known_collect_flags.insert(f);
    }
}

// ── Correlation state (P3) ───────────────────────────────────────────────
//
// A genuine placed-item pickup fires a collectible-shaped collect flag AND an
// AddItemFunc grant within the same brief window. Load-time/region/script flag
// bursts (runs of 10-20 flags in one instant) have NO accompanying grant, so
// requiring grant-correlation rejects them. A burst guard is a cheap backstop
// for the rare case a burst overlaps a real grant.
int64_t now_ns()
{
    return std::chrono::steady_clock::now().time_since_epoch().count();
}
constexpr int64_t CORRELATION_WINDOW_NS = 1'500'000'000; // ±1.5 s flag↔grant
constexpr int64_t BURST_SPAN_NS = 100'000'000;           // 100 ms
constexpr int BURST_MIN = 4; // >=this many collectible-unknowns in BURST_SPAN = bulk init

// Whether to require flag↔grant correlation (only when the item hook is also on;
// without grant data we can't correlate, so fall back to the looser P2 report).
bool g_correlate = false;

// Recent AddItemFunc grants (time + granted item id), for flag↔grant correlation
// and to name the gap toast by item category.
struct GrantEvt
{
    int64_t t;
    uint32_t item_id;
};
std::mutex g_grant_mtx;
std::vector<GrantEvt> g_grants;

void record_grant(int64_t t, uint32_t item_id)
{
    std::lock_guard<std::mutex> lk(g_grant_mtx);
    g_grants.push_back({t, item_id});
}

// If a grant falls within ±window of t, return true and set out_id to the
// NEAREST such grant's item id (0 if its entry was unreadable).
bool grant_within(int64_t t, int64_t window, uint32_t &out_id)
{
    std::lock_guard<std::mutex> lk(g_grant_mtx);
    bool found = false;
    int64_t best = window + 1;
    for (const GrantEvt &g : g_grants)
    {
        int64_t d = g.t > t ? g.t - t : t - g.t;
        if (d <= window && d < best)
        {
            best = d;
            out_id = g.item_id;
            found = true;
        }
    }
    return found;
}

void prune_grants(int64_t before)
{
    std::lock_guard<std::mutex> lk(g_grant_mtx);
    auto &v = g_grants;
    v.erase(std::remove_if(v.begin(), v.end(),
                           [before](const GrantEvt &g) { return g.t < before; }),
            v.end());
}

// Item category from the runtime item id's high nibble. Index matches
// goblin::GAP_CAT_NAMES / gap_cat_toast_id order.
const char *const GAP_CAT_LOG[] = {"Armament", "Armour", "Talisman",
                                   "Goods", "Ash of War", "item"};
int item_category_index(uint32_t id)
{
    switch ((id >> 28) & 0xF)
    {
    case 0x0: return 0; // Armament / weapon
    case 0x1: return 1; // Armour / protector
    case 0x2: return 2; // Talisman / accessory
    case 0x4: return 3; // Goods
    case 0x8: return 4; // Ash of War / gem
    default:  return 5; // other
    }
}

// SEH-guarded raw copy: the detours read pointers the game hands us; a bad/borderline
// pointer must degrade to "skip this event", never crash. Body is POD-only (no C++
// object unwinding allowed inside __try under clang-cl/MSVC).
bool safe_copy(const void *src, void *dst, size_t n)
{
    __try
    {
        memcpy(dst, src, n);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// ── SetEventFlag observer ────────────────────────────────────────────────
//
// Same id-by-pointer convention as IsEventFlag (see goblin_markers.cpp): rcx =
// EventFlagMan*, rdx = uint32_t* flag id, r8b = value (0/1). The AOB's
// `41 0F B6 F8` (movzx edi,r8b) value capture distinguishes this SETTER from
// the getter (which lacks it). 4th param = safety pad so the trampoline forwards
// r9 faithfully if the routine takes one (extra Win64 int args are caller-cleaned
// and harmless when ignored).
using SetEventFlagFn = uint64_t (*)(void *, uint32_t *, uint8_t, uint64_t);
SetEventFlagFn g_orig_set_event_flag = nullptr;
constexpr const char *SET_EVENT_FLAG_AOB =
    "48 89 5C 24 08 48 89 74 24 18 57 48 83 EC 30 48 8B DA 41 0F B6 F8 "
    "8B 12 48 8B F1 85 D2 0F 84";

// Hot-path → drain-thread inbox. The flag detour fires a LOT, so it only records
// the event (id, value, set-time) under a short lock and returns; the drain
// thread dedups + correlates + logs off the game's flag-set path. The set-time
// is captured here (not at drain) so correlation and burst detection are precise.
// Capped so a stalled drain can't grow it unbounded.
struct FlagEvt
{
    uint32_t id;
    uint8_t val;
    int64_t t_ns;
};
constexpr size_t INBOX_CAP = 8192;
std::mutex g_inbox_mtx;
std::vector<FlagEvt> g_inbox;
std::atomic<uint64_t> g_dropped{0};

uint64_t hk_set_event_flag(void *flagman, uint32_t *flag_id, uint8_t value,
                           uint64_t pad)
{
    uint32_t id;
    if (flag_id && safe_copy(flag_id, &id, sizeof(id)))
    {
        int64_t t = now_ns();
        std::lock_guard<std::mutex> lk(g_inbox_mtx);
        if (g_inbox.size() < INBOX_CAP)
            g_inbox.push_back({id, value, t});
        else
            g_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    return g_orig_set_event_flag(flagman, flag_id, value, pad);
}

void flag_drain_loop()
{
    // Log each flag id only once per session (first set) — the game sets a huge
    // number, but each distinct id is interesting only once for discovery.
    std::unordered_set<uint32_t> seen;
    std::vector<FlagEvt> batch;
    // Collectible-shaped UNKNOWN first-sets awaiting flag↔grant correlation. Held
    // CORRELATION_WINDOW_NS so a grant arriving slightly AFTER the flag is seen.
    std::vector<FlagEvt> pending;
    uint64_t last_dropped = 0;
    // Suppression tallies (drain-thread-local). Logged as a throttled summary so
    // burst/uncorrelated drops are VISIBLE without per-flag spam — proves the
    // correlation filter is working vs simply nothing happening.
    uint64_t sup_burst = 0, sup_uncorr = 0, reported = 0;
    uint64_t last_sum_burst = 0, last_sum_uncorr = 0, last_sum_reported = 0;
    int64_t last_summary_ns = now_ns();

    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        batch.clear();
        {
            std::lock_guard<std::mutex> lk(g_inbox_mtx);
            g_inbox.swap(batch);
        }

        for (const FlagEvt &e : batch)
        {
            if (!seen.insert(e.id).second)
                continue;
            bool unknown_collectible = e.val == 1 && is_collectible_shaped(e.id) &&
                                       !g_known_collect_flags.count(e.id);
            if (!unknown_collectible)
            {
                g_log->info("flag {} = {}", e.id, static_cast<int>(e.val));
                continue;
            }
            if (!g_correlate)
            {
                // No item hook → can't correlate; fall back to the looser report.
                g_log->warn("UNKNOWN placed-item flag {} (cat {}) — no marker "
                            "(uncorrelated)",
                            e.id, (e.id / 1000u) % 10u);
                continue;
            }
            pending.push_back(e); // finalize later, once grants have had time to land
        }

        int64_t now = now_ns();
        prune_grants(now - 10'000'000'000); // keep ~10 s of grant history

        // Finalize pending unknowns old enough that any correlated grant has landed.
        std::vector<FlagEvt> still;
        for (const FlagEvt &e : pending)
        {
            if (now - e.t_ns < CORRELATION_WINDOW_NS)
            {
                still.push_back(e); // not ripe yet
                continue;
            }
            // Burst guard: many collectible-unknowns in one instant = bulk init,
            // not a pickup — drop the whole cluster regardless of grants.
            int cluster = 0;
            for (const FlagEvt &o : pending)
                if (o.t_ns >= e.t_ns - BURST_SPAN_NS && o.t_ns <= e.t_ns + BURST_SPAN_NS)
                    cluster++;
            if (cluster >= BURST_MIN)
            {
                sup_burst++;
                continue;
            }
            uint32_t gid = 0;
            if (grant_within(e.t_ns, CORRELATION_WINDOW_NS, gid))
            {
                reported++;
                int cat = item_category_index(gid);
                g_log->warn("UNKNOWN placed-item flag {} (flag-cat {}) — collected "
                            "{} {:#010x} with NO map marker; coverage gap candidate",
                            e.id, (e.id / 1000u) % 10u, GAP_CAT_LOG[cat], gid);
                // Live in-game notice, named by item category (queued; watcher
                // spaces multiple). gid==0 (unreadable entry) → "item" bucket.
                goblin::enqueue_toast(goblin::gap_cat_toast_id(cat));
            }
            else
                sup_uncorr++; // collect-shaped flag, no nearby grant → script/region
        }
        pending.swap(still);

        // Throttled suppression summary (≥10 s apart, only when a tally moved) so
        // the filter's work is provable from the log.
        if (now - last_summary_ns >= 10'000'000'000 &&
            (sup_burst != last_sum_burst || sup_uncorr != last_sum_uncorr ||
             reported != last_sum_reported))
        {
            g_log->info("[correlation] reported {} gap candidate(s); suppressed {} "
                        "burst-init + {} uncorrelated collectible flags",
                        reported, sup_burst, sup_uncorr);
            last_sum_burst = sup_burst;
            last_sum_uncorr = sup_uncorr;
            last_sum_reported = reported;
            last_summary_ns = now;
        }

        uint64_t dropped = g_dropped.load(std::memory_order_relaxed);
        if (dropped != last_dropped)
        {
            g_log->warn("inbox overflow: {} flag-set events dropped", dropped);
            last_dropped = dropped;
        }
    }
}

// ── AddItemFunc observer ─────────────────────────────────────────────────
//
// The game's inventory-add routine (Hexinton CT "AddItemFunc"). Convention from
// the CT's ItemGib helper:
//   rcx = inventory/MapItemMan accessor
//   rdx = pointer to an array of item ENTRIES (8 bytes each in the CT template
//         F00006AE00000001 = {qty:u32 @+0, item_id:u32 @+4}, but that's the
//         cheat's placeholder — the exact offset is confirmed against a known
//         pickup, so for now we LOG RAW and both dword interpretations).
//   r8  = entry-array base, r9 = count.
// Low volume (a few grants/min) → log directly; g_log is _mt so the game-thread
// write races safely with the flag drain thread.
using AddItemFn = uint64_t (*)(void *, uint8_t *, uint8_t *, uint64_t, uint64_t);
AddItemFn g_orig_add_item = nullptr;
constexpr const char *ADD_ITEM_AOB =
    "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 70 FF FF FF 48 81 EC "
    "90 01 00 00 48 C7 45 C8 FE FF FF FF 48 89 9C 24 D8 01 00 00 48 8B 05";

uint64_t hk_add_item(void *inv, uint8_t *entries, uint8_t *base, uint64_t count,
                     uint64_t pad)
{
    int64_t t = now_ns();
    uint32_t item_id = 0;
    uint8_t buf[24];
    if (entries && safe_copy(entries, buf, sizeof(buf)))
    {
        uint32_t f0, f4;
        std::memcpy(&f0, buf + 0, 4);
        std::memcpy(&f4, buf + 4, 4);
        item_id = f4; // entry+4 = item id (confirmed in-game; entry+0 = quantity)
        // raw hex of the first entry + the two candidate id/qty fields, so a
        // controlled pickup pins which is the item id.
        g_log->info(
            "item grant: count={} entry+0={:#010x} entry+4={:#010x} "
            "raw={:02x}{:02x}{:02x}{:02x} {:02x}{:02x}{:02x}{:02x} "
            "{:02x}{:02x}{:02x}{:02x} {:02x}{:02x}{:02x}{:02x}",
            count, f0, f4, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6],
            buf[7], buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14],
            buf[15]);
    }
    record_grant(t, item_id); // for flag↔grant correlation + gap-toast category
    return g_orig_add_item(inv, entries, base, count, pad);
}

bool install(const char *aob, void *detour, void **trampoline, const char *name)
{
    void *fn = modutils::scan<void>({.aob = aob});
    if (!fn)
    {
        spdlog::warn("[DEBUG-EVENTS] {} AOB not found (game patch?) — skipped", name);
        return false;
    }
    try
    {
        modutils::hook(fn, detour, trampoline);
        spdlog::info("[DEBUG-EVENTS] {} hooked @ {}", name, fn);
        return true;
    }
    catch (const std::exception &e)
    {
        spdlog::error("[DEBUG-EVENTS] {} hook failed: {}", name, e.what());
        return false;
    }
}
} // namespace

void initialize(const std::filesystem::path &log_path, bool hook_flags,
                bool hook_items)
{
    if (!hook_flags && !hook_items)
        return;

    try
    {
        // Append (not truncate): a relaunch must not wipe a capture in progress.
        // Sessions are separated by the "=== observer started ===" marker below.
        g_log = spdlog::basic_logger_mt("mfg-events", log_path.string(), false);
        g_log->set_pattern("[%H:%M:%S.%e] %v");
        g_log->flush_on(spdlog::level::info);
    }
    catch (const std::exception &e)
    {
        spdlog::error("[DEBUG-EVENTS] could not open log {}: {}", log_path.string(),
                      e.what());
        return;
    }

    bool any = false;
    bool flag_on = false;
    if (hook_flags)
    {
        build_known_collect_flags();
        spdlog::info("[DEBUG-EVENTS] {} known marker collect-flags loaded",
                     g_known_collect_flags.size());
        any |= flag_on = install(SET_EVENT_FLAG_AOB,
                                 reinterpret_cast<void *>(&hk_set_event_flag),
                                 reinterpret_cast<void **>(&g_orig_set_event_flag),
                                 "SetEventFlag");
    }
    if (hook_items)
    {
        bool item_on = install(ADD_ITEM_AOB, reinterpret_cast<void *>(&hk_add_item),
                               reinterpret_cast<void **>(&g_orig_add_item),
                               "AddItemFunc");
        any |= item_on;
        // Correlate flag↔grant only when both hooks are live; otherwise the flag
        // classifier has no grant data and falls back to the looser report.
        g_correlate = flag_on && item_on;
    }

    if (!any)
        return;

    try
    {
        // enable_hooks() (MH_ApplyQueued) already ran during setup_mod; apply
        // again to activate these late-queued hooks.
        modutils::enable_hooks();
    }
    catch (const std::exception &e)
    {
        spdlog::error("[DEBUG-EVENTS] enable_hooks failed: {} — disabled", e.what());
        return;
    }

    if (flag_on)
        std::thread(flag_drain_loop).detach();

    g_log->info("=== observer started (flags={} items={}) ===", hook_flags,
                hook_items);
    spdlog::info("[DEBUG-EVENTS] active → logging to {}", log_path.string());
}

} // namespace goblin::debug_events
