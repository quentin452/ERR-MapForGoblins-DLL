// Compiled ONLY into goblin_overlay_render.dll (CMake GOBLIN_OVERLAY_HOTRELOAD block), like
// goblin_render_new_override.cpp.
//
// Why: each /MT DLL has its own spdlog registry. Without this bridge, render-side spdlog calls
// ([BENCH] scoped timers, [LANDMARKLIVE], [LOOTDISK] build logs, ...) lazily create the render
// module's OWN default stdout logger — so (a) every render-side log line silently vanishes from
// MapForGoblins.log, and (b) that lazy first-touch initialization happens inside hooked render
// paths on whatever thread logs first (present thread vs. the disk-build worker), which crashed
// intermittently at boot (AV in spdlog::logger::should_log — symbolized from the live crash dump,
// 2026-07-02, twice). Fix: the host loader calls MFG_RenderInitLogging IMMEDIATELY after
// GetProcAddress resolution — single-threaded, before any other render code runs — installing a
// default logger whose only sink forwards each formatted line to the host's spdlog via the
// MFG_HostLogLine C export. Deterministic init kills the race; forwarding unifies the log file.

#if defined(GOBLIN_OVERLAY_HOTRELOAD_BUILD)

#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>

#include <memory>
#include <mutex>
#include <string>

extern "C" __declspec(dllimport) void MFG_HostLogLine(int level, const char *msg);

namespace
{
    // Forwards the formatted PAYLOAD only — the host logger re-applies its own pattern
    // (timestamp/name/level), so forwarded lines look identical to host lines in the file.
    class HostForwardSink : public spdlog::sinks::base_sink<std::mutex>
    {
    protected:
        void sink_it_(const spdlog::details::log_msg &msg) override
        {
            std::string payload(msg.payload.data(), msg.payload.size());
            MFG_HostLogLine(static_cast<int>(msg.level), payload.c_str());
        }
        void flush_() override {}
    };
}

extern "C" __declspec(dllexport) void MFG_RenderInitLogging(int host_level)
{
    auto logger = std::make_shared<spdlog::logger>("render", std::make_shared<HostForwardSink>());
    logger->set_level(static_cast<spdlog::level::level_enum>(host_level));
    logger->flush_on(spdlog::level::trace);  // forwarding is synchronous anyway
    spdlog::set_default_logger(std::move(logger));
}

#endif  // GOBLIN_OVERLAY_HOTRELOAD_BUILD
