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
// Cost is a single steady_clock read on entry + one on exit + one log line;
// nothing is gated behind heavy macros and there is no behavior change.

#include <chrono>
#include <spdlog/spdlog.h>

namespace goblin::bench
{
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
