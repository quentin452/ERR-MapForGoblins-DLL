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

namespace goblin::bench
{
    // Per-label running aggregate.
    struct Stat
    {
        std::uint64_t count = 0;
        double total = 0.0;
        double min = (std::numeric_limits<double>::max)(); // parens: dodge windows.h max macro
        double max = 0.0;
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

        void record(const char *label, double ms)
        {
            std::lock_guard<std::mutex> lk(mu_);
            Stat &s = stats_[label];
            s.count++;
            s.total += ms;
            if (ms < s.min)
                s.min = ms;
            if (ms > s.max)
                s.max = ms;
        }

        // Dump a sorted (by total time desc) report. Safe to call once at detach.
        void dump_report()
        {
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
            spdlog::info("[BENCH] {:<36}{:>7}{:>12}{:>11}{:>11}{:>11}{:>8}", "label", "count",
                         "total ms", "avg ms", "min ms", "max ms", "%wall");
            for (const auto &[label, s] : v)
            {
                const double avg = s.count ? s.total / s.count : 0.0;
                const double mn = (s.count ? s.min : 0.0);
                const double pct = wall > 0.0 ? s.total / wall * 100.0 : 0.0;
                spdlog::info("[BENCH] {:<36}{:>7}{:>12.2f}{:>11.2f}{:>11.2f}{:>11.2f}{:>7.2f}%", label,
                             s.count, s.total, avg, mn, s.max, pct);
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
        explicit ScopedTimer(const char *label)
            : label_(label), start_(std::chrono::steady_clock::now())
        {
        }

        ~ScopedTimer()
        {
            const auto ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - start_)
                                .count();
            spdlog::info("[BENCH] {}: {:.2f} ms", label_, ms);
            Registry::instance().record(label_, ms);
        }

        ScopedTimer(const ScopedTimer &) = delete;
        ScopedTimer &operator=(const ScopedTimer &) = delete;

    private:
        const char *label_;
        std::chrono::steady_clock::time_point start_;
    };
}

#define GOBLIN_BENCH_CONCAT2(a, b) a##b
#define GOBLIN_BENCH_CONCAT(a, b) GOBLIN_BENCH_CONCAT2(a, b)

// Wrap the current scope in a named timer. `label` should be a stable,
// greppable string literal, e.g. GOBLIN_BENCH("map.inject").
#define GOBLIN_BENCH(label) \
    ::goblin::bench::ScopedTimer GOBLIN_BENCH_CONCAT(goblin_bench_timer_, __LINE__)(label)
