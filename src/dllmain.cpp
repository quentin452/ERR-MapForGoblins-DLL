#define WIN32_LEAN_AND_MEAN
#include <filesystem>
#include <memory>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <thread>
#include <windows.h>

#include "from/params.hpp"
#include "modutils.hpp"
#include "re_signatures.hpp"
#include "goblin_log_archive.hpp"

#include "goblin_collected.hpp"
#include "goblin_config.hpp"
#include "goblin_debug_events.hpp"
#include "goblin_field_probe.hpp"
#include "goblin_worldmap_probe.hpp"
#include "goblin_inject.hpp"
#include "goblin_kindling.hpp"
#include "goblin_logic.hpp"
#include "goblin_markers.hpp"
#include "goblin_messages.hpp"
#include "goblin_overlay.hpp"
#include "goblin_bench.hpp"
#include "goblin_crashdump.hpp"
#include "worldmap/loot_disk.hpp"
#include "worldmap/name_fmg_en.hpp"  // load_english_name_index (F1 English search aliases)
#include "worldmap/loot_open_probe.hpp"
#include "goblin_overlay_render_loader.hpp"

#include "version.h"

static std::thread mod_thread;

// SEH wrapper — catches access violations from refresh() during multiplayer transitions
static int safe_refresh_seh()
{
    __try
    {
        return goblin::collected::refresh();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

static int safe_kindling_refresh_seh()
{
    __try
    {
        return goblin::kindling::refresh();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

static void safe_flag_or_pairs_seh()
{
    __try
    {
        goblin::apply_flag_or_pairs();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

static void safe_fragment_eviction_seh()
{
    __try
    {
        goblin::refresh_category_census();  // per-category uncollected counts (overlay)
        goblin::refresh_quest_finishable(); // Quest Browser: grey out unfinishable lines
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// ── SEH-guarded init-phase wrappers ──
// MSVC's /EHsc disallows __try in functions that contain C++ objects with
// destructors, so each init step goes through a plain C-style adapter +
// a shared invoker. If any step access-violates (e.g. another mod shifted
// the game's memory map mid-init), we log and continue — losing that
// feature is better than the DLL crashing the entire game.

using InitFn = void (*)();

static bool seh_invoke_void(InitFn fn)
{
    __try { fn(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void init_modutils()         { modutils::initialize(); }
// Must run before init_prebuild_markers (below) and any other host code that calls into the
// render module — goblin::overlay::initialize() also calls load() (idempotent) as a second gate
// specifically for the overlay/draw-function path, see docs/plans/overlay_hot_reload_playwright_plan.md.
static void init_render_module()    { goblin::overlay_render_loader::load(); }
static void init_from_params()      { from::params::initialize(); }
static void init_collected()        { goblin::collected::initialize(); }
static void init_kindling()         { goblin::kindling::initialize(); }
static void init_prebuild_markers() { goblin::overlay_render_loader::call_prebuild_markers(); }
static void init_tutorial_popup()   { goblin::inject_tutorial_popup_rows(); }
static void init_setup_messages()   { goblin::setup_messages(); }
static void init_icon_tex_probe()   { goblin::install_icon_texture_probe(); }
static void init_grace_suppress()   { goblin::install_grace_suppression_hook(); }
static void init_field_probe()      { goblin::field_probe::initialize(goblin::config::probeFieldSpec); }

static void safe_init_step(InitFn fn, const char *name)
{
    if (!seh_invoke_void(fn))
        spdlog::error("SEH exception in init step '{}' — feature may be degraded", name);
}

static void setup_logger(std::filesystem::path log_file)
{
    auto logger = std::make_shared<spdlog::logger>("mapforgoblins");
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] %^[%l]%$ %v");
    // Truncate: one fresh file per session. Previous sessions are preserved by the
    // log-archive lifecycle (zipped to logs/archive/ at startup), so no append needed.
    logger->sinks().push_back(
        std::make_shared<spdlog::sinks::basic_file_sink_st>(log_file.string(), true));
    logger->flush_on(spdlog::level::info);

#if _DEBUG
    AllocConsole();
    FILE *stream;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    freopen_s(&stream, "CONIN$", "r", stdin);
    logger->sinks().push_back(std::make_shared<spdlog::sinks::stdout_color_sink_st>());
    logger->set_level(spdlog::level::trace);
#endif

    spdlog::set_default_logger(logger);
    // The daily sink APPENDS, so restarts concatenate into one file. Emit a loud
    // separator so the latest session is unmistakable (no more reading a stale
    // session's startup and mis-diagnosing). Search "NEW SESSION" → last match.
    spdlog::info("");
    spdlog::info("==================== NEW SESSION ====================");
}

static std::filesystem::path g_mod_folder;

static void setup_mod()
{
    safe_init_step(&init_modutils,    "modutils::initialize");
    safe_init_step(&init_render_module, "overlay_render_loader::load");
    {
        GOBLIN_BENCH("init.from_params");
        safe_init_step(&init_from_params, "from::params::initialize");
    }

    // Hand the disk-MSB loot source our mod folder for map-dir auto-detect (used
    // lazily on first map build when config loot_from_disk_msb is on).
    goblin::worldmap::set_mod_folder(g_mod_folder);

    // Arm the CreateFileW map-open observer now — BEFORE enable_hooks() applies the
    // queued batch and before the game streams any map. It serves two roles (armed
    // when loot_from_disk_msb OR diag_map_opens is on): the map-dir DISCOVERY
    // fallback (completes a Searching state when the ancestor-walk missed the mod's
    // map folder) and the verbose [MAPOPEN] diagnostic log.
    goblin::worldmap::install_map_open_probe();

    // AOB signature health check — logs PASS/FAIL for every centralized signature
    // (src/re_signatures.hpp). After a game update, a FAIL line names exactly which
    // signature broke. Cheap, read-only; runs once at init.
    {
        GOBLIN_BENCH("init.signatures");
        safe_init_step(&goblin::sig::resolve_all_signatures, "AOB signature health check");
    }

    // Robust init wait: POLL for the WorldMapPointParam table (the real dependency
    // of inject_map_entries) instead of sleeping a fixed load_delay — on a slow PC
    // the params can take well over 5s to load, and a too-short fixed delay makes
    // the inject find nothing / init incorrectly. We still honor load_delay as a
    // MINIMUM total wait (settle margin + backward-compat), and cap the poll so we
    // never hang forever if the probe never goes ready.
    {
        using namespace std::chrono;
        GOBLIN_BENCH("init.param_poll");
        constexpr int POLL_MS = 200;
        constexpr int HARD_CAP_S = 180;  // never wait longer than this
        auto t0 = steady_clock::now();
        spdlog::info("Waiting for WorldMapPointParam to load (min {}s, cap {}s)...",
                     goblin::config::loadDelay, HARD_CAP_S);
        bool ready = false;
        for (int waited = 0; waited < HARD_CAP_S * 1000; waited += POLL_MS)
        {
            if (goblin::world_map_param_ready()) { ready = true; break; }
            std::this_thread::sleep_for(milliseconds(POLL_MS));
        }
        auto elapsed = duration_cast<milliseconds>(steady_clock::now() - t0).count();
        if (ready)
            spdlog::info("WorldMapPointParam ready after {} ms", elapsed);
        else
            spdlog::warn("WorldMapPointParam not ready after {}s cap — proceeding "
                         "anyway (init may degrade)", HARD_CAP_S);
        // Honor load_delay as a minimum total wait (settle + old behaviour).
        auto min_ms = static_cast<long long>(goblin::config::loadDelay) * 1000;
        if (elapsed < min_ms)
            std::this_thread::sleep_for(milliseconds(min_ms - elapsed));
    }

    {
        GOBLIN_BENCH("init.heavy.total");
        safe_init_step(&init_collected,       "collected::initialize");
        safe_init_step(&init_kindling,        "kindling::initialize");
        // Snapshot the real graces from the LIVE WorldMapPointParam — the ImGui overlay
        // draws from this (no baked grace data).
        safe_init_step(&goblin::capture_live_graces, "capture_live_graces");
        // Read the English (engus) Name FMGs off disk for the F1 search's English
        // aliases — mod-agnostic, replaces the ERR-frozen baked alias table.
        safe_init_step(&goblin::load_english_name_index, "load_english_name_index");
        // Seed the per-category visibility / cluster / threshold + master gates from
        // config — the overlay reads these.
        safe_init_step(&goblin::seed_runtime_gates, "seed_runtime_gates");
        // Pre-build the overlay marker buckets now (on this init thread, params
        // ready) instead of lazily on the first world-map open — the disk-MSB loot
        // parse + projection (~0.5s) would otherwise hitch that first open. Lazy
        // markers()/census still work (call_once no-ops if this already ran).
        safe_init_step(&init_prebuild_markers, "worldmap::prebuild_markers");
        // The ImGui overlay is the sole world map (the native WorldMapPointParam
        // injection was removed — no native page-build = no freeze, no double-draw).
        safe_init_step(&init_tutorial_popup,  "inject_tutorial_popup_rows");
        safe_init_step(&init_setup_messages,  "setup_messages");
        // Queue the live-refresh hook (FUN_140a82a80) — kept for the native-pin
        // suppression path; no-op until enabled.
        safe_init_step(&init_icon_tex_probe,  "install_icon_texture_probe");
        safe_init_step(&init_grace_suppress,  "install_grace_suppression_hook");
        // Dev RE tool: embedded find-what-accesses for the offset source-of-truth work.
        // Arms a HW breakpoint on a live param row+offset; the [FWA] hit names the game's
        // own field-read RIP (mod reads filtered). Off unless probe_field_access is set.
        if (goblin::config::probeFieldAccess)
            safe_init_step(&init_field_probe, "field_probe::initialize");
    }

    try
    {
        GOBLIN_BENCH("init.enable_hooks");
        modutils::enable_hooks();
    }
    catch (const std::exception &e)
    {
        spdlog::error("enable_hooks() FAILED: {}", e.what());
    }

    spdlog::info("Initialization complete");

    // Re-assert our crash filter on top of whatever the game/Seamless installed
    // during their startup, so the non-deterministic post-load crash is caught.
    goblin::install_crash_handler(g_mod_folder / "logs");

    if (GetModuleHandleA("ersc.dll"))
        spdlog::info("Seamless Co-op detected (ersc.dll)");

    // hotkey_loop also services the F8 icon find-by-name dev probe (gated dump_icon_textures),
    // so start it for EITHER flag.
    if (goblin::config::enableMarkerDump || goblin::config::dumpIconTextures)
    {
        goblin::markers::set_output_path(g_mod_folder / "logs" / "MapForGoblins_markers.log");
        std::thread(goblin::markers::hotkey_loop).detach();
        if (goblin::config::enableMarkerDump)
            spdlog::info("Marker dump hotkey: VK 0x{:X}", goblin::config::markerDumpKey);
        if (goblin::config::dumpIconTextures)
            spdlog::info("[FIND2] icon find-by-name probe armed on F8 (open inventory, press F8)");
    }

    // Thread 7 — opt-in coverage-gap observers (SetEventFlag / AddItemFunc). Each
    // self-disables on resolve/hook failure without touching the rest of the mod.
    if (goblin::config::debugEventFlags || goblin::config::debugItemGrants ||
        goblin::config::debugFlagCapture)
        goblin::debug_events::initialize(g_mod_folder / "logs" / "MapForGoblins_events.log",
                                         goblin::config::debugEventFlags,
                                         goblin::config::debugItemGrants,
                                         goblin::config::debugFlagCapture);

    // The overlay IS the map now (native injection removed), so the probe loop always runs
    // — it publishes the active cursor + live view the overlay renders against.
    goblin::worldmap_probe::initialize(g_mod_folder / "logs" / "MapForGoblins_wmprobe.log");

    // The watcher is the single owner of the WorldMapPointParam state — it
    // applies the overlay menu's master-off / per-section / per-category /
    // cluster intents and persists them. (Legacy F-key hotkeys F6/F7/F8/F10/F11
    // were removed in P3c; the in-game overlay menu (F1) drives all of this now.
    // The map-based auto-hide it used to run was removed after the 16-align fix
    // made the expanded table safe during hosting — see
    // docs/ersc_hosting_and_map_autohide.md.) Always runs.
    std::thread(goblin::menu_auto_toggle_loop).detach();
    spdlog::info("Icon-state watcher started (icons EXPANDED always; overlay menu drives show/hide)");

    // Thread 6 — in-game ImGui overlay. Phase 1: hello window toggled by F1.
    // Installs a DX12 Present hook; on any failure it self-disables and the mod
    // continues unaffected.
    goblin::overlay::initialize();

    bool first_read = true;
    auto start = std::chrono::steady_clock::now();
    while (true)
    {
        // Fast polling (100ms) for first 30 seconds to catch NonActive GEOF data
        // before it transitions to WGM. Then slow down to 2 seconds.
        auto elapsed = std::chrono::steady_clock::now() - start;
        bool fast_phase = elapsed < std::chrono::seconds(30);
        std::this_thread::sleep_for(fast_phase ? std::chrono::milliseconds(100) : std::chrono::seconds(2));

        try
        {
            int newly = safe_refresh_seh();
            if (first_read && newly > 0)
            {
                spdlog::info("Initial state: {} pieces hidden",
                             goblin::collected::collected_count());
                first_read = false;
            }
        }
        catch (...)
        {
        }

        try
        {
            safe_kindling_refresh_seh();
        }
        catch (...)
        {
        }

        try
        {
            safe_flag_or_pairs_seh();
        }
        catch (...)
        {
        }

        try
        {
            safe_fragment_eviction_seh();
        }
        catch (...)
        {
        }

        // Periodic [BENCH] session report. DLL_PROCESS_DETACH does NOT fire under
        // Proton/Wine (the game hard-terminates → no clean unload, confirmed: no
        // deinit/shutdown line ever reaches the log), so an exit-time dump is
        // impossible. Instead snapshot the aggregate every 30s; spdlog flushes per
        // line, so the log always ends with a recent full report even on a hard kill.
        {
            static auto last_report = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (now - last_report >= std::chrono::seconds(30))
            {
                goblin::bench::Registry::instance().dump_report();
                last_report = now;
            }
        }
    }
}

bool WINAPI DllMain(HINSTANCE dll_instance, unsigned int fdw_reason, void *lpv_reserved)
{
    if (fdw_reason == DLL_PROCESS_ATTACH)
    {
        // Stamp the load instant so the [BENCH] session report (dumped at detach)
        // can express each timer's cost as a share of total DLL wallclock.
        goblin::bench::Registry::instance().mark_load();

        wchar_t dll_filename[MAX_PATH] = {0};
        GetModuleFileNameW(dll_instance, dll_filename, MAX_PATH);
        auto folder = std::filesystem::path(dll_filename).parent_path();
        g_mod_folder = folder;

        // Log lifecycle: zip the PREVIOUS session's logs into logs/archive/ and start
        // clean. MUST run before any logger opens its file. Keep the newest 20 archives.
        goblin::log_archive::archive_and_rotate(folder / "logs", 20);

        setup_logger(folder / "logs" / "MapForGoblins.log");

        // Last-resort in-process crash dumper (WER LocalDumps does not work
        // under Proton/Wine). Installed first so even an early fault is caught;
        // re-installed at the end of setup_mod to sit on top of the filter the
        // game installs during its own startup, covering the post-load window.
        goblin::install_crash_handler(folder / "logs");

        spdlog::info("Map For Goblins DLL v{}", PROJECT_VERSION);
        goblin::load_config(folder / "MapForGoblins.ini");

        if (goblin::config::debugLogging)
            spdlog::default_logger()->set_level(spdlog::level::debug);

        mod_thread = std::thread([]()
                                 {
            try
            {
                setup_mod();
            }
            catch (std::runtime_error const &e)
            {
                spdlog::error("Error initializing mod: {}", e.what());
                modutils::deinitialize();
                spdlog::shutdown();
            } });
    }
    else if (fdw_reason == DLL_PROCESS_DETACH && lpv_reserved != nullptr)
    {
        try
        {
            mod_thread.join();
            modutils::deinitialize();
        }
        catch (std::runtime_error const &e)
        {
            spdlog::error("Error deinitializing: {}", e.what());
        }
        // Full [BENCH] session summary (avg/min/max/total per label + %wall) before
        // the logger closes — one greppable block instead of scraping per-line timings.
        goblin::bench::Registry::instance().dump_report();
        spdlog::shutdown();
    }
    return true;
}
