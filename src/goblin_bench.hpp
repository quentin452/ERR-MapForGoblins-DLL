#pragma once
//
// Lightweight RAII scoped timer for localizing where time goes (map-open
// freeze, close micro-freezes, init cost). Logs "<label>: <ms> ms" at INFO
// on scope exit through the existing spdlog default logger ("mapforgoblins"),
// so timings land in the same logs/MapForGoblins.log file as everything else.
//
// All labels are prefixed "[BENCH] " so they are trivially greppable:
//     grep "\[BENCH\]" logs/MapForGoblins.log
//
// Every timed scope ALSO feeds a process-wide aggregate registry (count / total
// / avg / min / max per label). On DLL detach (game exit) Registry::dump_report()
// prints a sorted summary plus each label's share of the DLL wallclock lifetime
// — so a whole session's cost is one greppable block, no log-scraping math.
//
// Cost is a single steady_clock read on entry + one on exit + one log line + a
// mutexed map update; nothing is gated behind heavy macros and there is no
// behavior change.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

#include "goblin_dll_export.hpp"  // GOBLIN_RENDER_API (no-op unless GOBLIN_OVERLAY_HOTRELOAD_BUILD)
                                  // — this header is included by both host and render-side code,
                                  // so these two globals need the same dllexport/dllimport
                                  // treatment as goblin_overlay_render_api.hpp's declarations for
                                  // Slice C's real two-DLL split.

namespace goblin::config
{
    // Forward-declared (not #include "goblin_config.hpp") to keep this header's dependency
    // light, matching its own "no heavy macros" design note above. Independent gates: both
    // default true (matches pre-existing behavior unchanged); set both false to silence [BENCH]
    // entirely. [BENCH][SPIKE] lag-hitch warnings are NOT gated by either — those are anomaly
    // alerts, not routine noise, and fire even for quiet timers by design (see ScopedTimer).
    extern GOBLIN_RENDER_API bool benchLogIndividual;  // per-call "[BENCH] label: X ms" lines
    extern GOBLIN_RENDER_API bool benchLogSession;     // the "[BENCH] SESSION REPORT" dump at detach
}

namespace goblin::bench
{
    // ---- Lag-spike detection -------------------------------------------------
    // The naive aggregate (avg/min/max) hides one-off hitches: a single 200 ms
    // frame buried in 800 sub-ms frames only nudges the average and is invisible
    // in the live log (the hot render path is GOBLIN_BENCH_QUIET — no per-line
    // log at all). So a real "map-close micro-freeze" is near-impossible to spot.
    //
    // Spike = a sample that DWARFS that label's OWN running average. This is
    // relative, so it auto-adapts per label: a 0.3 ms/frame render path flags a
    // 12 ms hitch, while a one-shot 166 ms build span (no steady baseline) does
    // NOT spam — it never accrues enough samples to have an average to dwarf.
    // When a spike fires we emit a timestamped WARN immediately, EVEN for quiet
    // timers, so the freeze lands in the log right next to the action that caused
    // it:  grep "\[SPIKE\]" logs/MapForGoblins.log
    inline constexpr std::uint64_t kSpikeWarmup = 30;   // samples before a label can spike
    inline constexpr double        kSpikeFactor = 4.0;  // ms > factor*avg → spike
    inline constexpr double        kSpikeFloorMs = 2.0; // ...and ms > this, to skip sub-ms noise

    // Result of recording one sample, so the timer can decide whether to WARN.
    struct RecordResult
    {
        bool   spike = false;
        double over  = 0.0; // ms / avg at detection (how many× the baseline), for the WARN line
    };

    // Per-label running aggregate.
    struct Stat
    {
        std::uint64_t count = 0;
        double total = 0.0;
        double min = (std::numeric_limits<double>::max)(); // parens: dodge windows.h max macro
        double max = 0.0;
        std::uint64_t spikes = 0; // samples flagged as a lag spike (see above)
    };

    // Process-wide singleton accumulating every ScopedTimer sample. Inline static
    // local → exactly one instance across all TUs. Thread-safe (timers fire from
    // the init thread, the refresh loop and the render path).
    class Registry
    {
    public:
        static Registry &instance()
        {
            static Registry r;
            return r;
        }

        // Stamp the DLL-load instant so the report can express each label's cost as
        // a percentage of total DLL wallclock. Call once from DLL_PROCESS_ATTACH.
        void mark_load()
        {
            std::lock_guard<std::mutex> lk(mu_);
            load_ = std::chrono::steady_clock::now();
            have_load_ = true;
        }

        RecordResult record(const char *label, double ms)
        {
            std::lock_guard<std::mutex> lk(mu_);
            Stat &s = stats_[label];

            // Detect the spike against the baseline BUILT SO FAR (i.e. excluding this
            // sample), so a hitch is measured relative to the label's normal cost.
            RecordResult res;
            if (s.count >= kSpikeWarmup)
            {
                const double avg = s.total / s.count;
                if (avg > 0.0 && ms > kSpikeFloorMs && ms > avg * kSpikeFactor)
                {
                    res.spike = true;
                    res.over = ms / avg;
                    s.spikes++;
                }
            }

            s.count++;
            s.total += ms;
            if (ms < s.min)
                s.min = ms;
            if (ms > s.max)
                s.max = ms;
            return res;
        }

        // Dump a sorted (by total time desc) report. Safe to call once at detach.
        void dump_report()
        {
            if (!goblin::config::benchLogSession)
                return;
            std::lock_guard<std::mutex> lk(mu_);
            if (stats_.empty())
                return;
            const double wall =
                have_load_ ? std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - load_)
                                 .count()
                           : 0.0;

            std::vector<std::pair<std::string, Stat>> v(stats_.begin(), stats_.end());
            std::sort(v.begin(), v.end(),
                      [](const auto &a, const auto &b) { return a.second.total > b.second.total; });

            spdlog::info("[BENCH] ===== SESSION REPORT — DLL wallclock {:.1f} ms, {} labels =====",
                         wall, v.size());
            spdlog::info("[BENCH] {:<36}{:>7}{:>12}{:>11}{:>11}{:>11}{:>8}{:>8}", "label", "count",
                         "total ms", "avg ms", "min ms", "max ms", "%wall", "spikes");
            for (const auto &[label, s] : v)
            {
                const double avg = s.count ? s.total / s.count : 0.0;
                const double mn = (s.count ? s.min : 0.0);
                const double pct = wall > 0.0 ? s.total / wall * 100.0 : 0.0;
                spdlog::info("[BENCH] {:<36}{:>7}{:>12.2f}{:>11.2f}{:>11.2f}{:>11.2f}{:>7.2f}%{:>8}", label,
                             s.count, s.total, avg, mn, s.max, pct, s.spikes);
            }
            spdlog::info("[BENCH] ===== END REPORT =====");
        }

        Registry(const Registry &) = delete;
        Registry &operator=(const Registry &) = delete;

    private:
        Registry() = default;
        std::mutex mu_;
        std::unordered_map<std::string, Stat> stats_;
        std::chrono::steady_clock::time_point load_;
        bool have_load_ = false;
    };

    class ScopedTimer
    {
    public:
        // quiet = aggregate into the Registry but DON'T emit a per-line log. For hot
        // per-frame paths (the worldmap render) where a line every frame would flood
        // the log; the avg/min/max/total still show in the session report.
        explicit ScopedTimer(const char *label, bool quiet = false)
            : label_(label), start_(std::chrono::steady_clock::now()), quiet_(quiet)
        {
        }

        ~ScopedTimer()
        {
            const auto ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - start_)
                                .count();
            const RecordResult r = Registry::instance().record(label_, ms);
            if (r.spike)
                // A lag spike fires a WARN even for quiet timers — this is the whole point: the
                // hitch lands in the log, timestamped, right next to whatever triggered it.
                spdlog::warn("[BENCH][SPIKE] {}: {:.2f} ms (~{:.0f}x its {:.2f} ms avg) — frame hitch",
                             label_, ms, r.over, (r.over > 0.0 ? ms / r.over : ms));
            else if (!quiet_ && goblin::config::benchLogIndividual)
                spdlog::info("[BENCH] {}: {:.2f} ms", label_, ms);
        }

        ScopedTimer(const ScopedTimer &) = delete;
        ScopedTimer &operator=(const ScopedTimer &) = delete;

    private:
        const char *label_;
        std::chrono::steady_clock::time_point start_;
        bool quiet_;
    };
}

#define GOBLIN_BENCH_CONCAT2(a, b) a##b
#define GOBLIN_BENCH_CONCAT(a, b) GOBLIN_BENCH_CONCAT2(a, b)

// Wrap the current scope in a named timer. `label` should be a stable,
// greppable string literal, e.g. GOBLIN_BENCH("map.inject").
#define GOBLIN_BENCH(label) \
    ::goblin::bench::ScopedTimer GOBLIN_BENCH_CONCAT(goblin_bench_timer_, __LINE__)(label)

// Like GOBLIN_BENCH but aggregate-only (no per-line log) — for hot per-frame paths
// that would otherwise flood the log. Numbers still appear in the session report.
#define GOBLIN_BENCH_QUIET(label) \
    ::goblin::bench::ScopedTimer GOBLIN_BENCH_CONCAT(goblin_bench_timer_, __LINE__)(label, true)
