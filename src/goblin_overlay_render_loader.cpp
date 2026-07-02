#include "goblin_overlay_render_loader.hpp"

#include <spdlog/spdlog.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

#if defined(GOBLIN_OVERLAY_HOTRELOAD_BUILD)
#include <atomic>
#include <cstdlib>
#include <thread>
#include "worldmap/loot_disk.hpp"  // set_build_trigger — host-held render fn ptr, de/re-registered around a swap
#else
#include "worldmap/map_entry_layer.hpp"  // prebuild_markers, refresh_overlay_census (same binary)
#include "worldmap/map_renderer.hpp"     // inworld_hovered (same binary)
#endif

#if defined(GOBLIN_OVERLAY_HOTRELOAD_BUILD)
// Slice D heap unification: /MT gives each DLL its own CRT heap, so any C++ allocation that crosses
// the host↔render boundary (std::string/vector returned by value, moved, or filled via out-param —
// ~8 overlay_api functions) would be allocated on one heap and freed on the other (corruption).
// Instead of marshalling every signature, the render DLL overrides its GLOBAL operator new/delete
// (src/goblin_render_new_override.cpp) to forward here — one heap for every C++ allocation on both
// sides, the whole api surface safe by construction. (ImGui allocates via malloc, not new — handled
// separately: host allocator fns travel in OverlayFrameCtx, trampolines SetAllocatorFunctions.)
extern "C"
{
    __declspec(dllexport) void *MFG_HostAlloc(size_t n) { return std::malloc(n); }
    __declspec(dllexport) void MFG_HostFree(void *p) { std::free(p); }
    __declspec(dllexport) void *MFG_HostAllocAligned(size_t n, size_t a) { return _aligned_malloc(n, a); }
    __declspec(dllexport) void MFG_HostFreeAligned(void *p) { _aligned_free(p); }
}
#endif

namespace goblin::overlay_render_loader
{
#if defined(GOBLIN_OVERLAY_HOTRELOAD_BUILD)
    namespace
    {
        using DrawPanelFn = void (*)(const goblin::overlay::OverlayFrameCtx *);
        using DrawWorldmapMarkersFn = void (*)(bool, const goblin::overlay::OverlayFrameCtx *);
        using DrawMinimapHudFn = void (*)(const goblin::overlay::OverlayFrameCtx *);
        using PrebuildMarkersFn = void (*)();
        using InworldHoveredFn = int (*)();
        using RefreshOverlayCensusFn = void (*)();
        using RenderIdleFn = int (*)();

        struct RenderExports
        {
            HMODULE module = nullptr;
            DrawPanelFn draw_panel = nullptr;
            DrawWorldmapMarkersFn draw_worldmap_markers = nullptr;
            DrawMinimapHudFn draw_minimap_hud = nullptr;
            PrebuildMarkersFn prebuild_markers = nullptr;
            InworldHoveredFn inworld_hovered = nullptr;
            RefreshOverlayCensusFn refresh_overlay_census = nullptr;
            RenderIdleFn render_idle = nullptr;
        };

        RenderExports g_cur;             // the live render module; swapped under g_lock exclusive
        HMODULE g_prev_module = nullptr; // freed one reload LATER (grace for the old worker's tail —
                                         // g_disk_running flips false a few instructions before the
                                         // detached build thread actually leaves render code)
        SRWLOCK g_lock = SRWLOCK_INIT;   // shared: every call_* below; exclusive: the swap itself
        thread_local int t_lock_depth = 0; // SRW is non-reentrant — only the outermost frame acquires
        std::atomic<bool> g_reload_pending{false};
        unsigned g_generation = 0;
        std::wstring g_dir;              // host DLL's directory, trailing slash
        // Source-file identity the watcher compares against (mtime<<0 | size), atomics because the
        // watcher thread reads while maybe_reload (present thread) writes after a successful swap.
        std::atomic<uint64_t> g_loaded_mtime{0}, g_loaded_size{0};

        // Outermost-frame shared lock: call_* nested under another call_* on the same thread (or
        // under the reload path, which calls raw fn pointers on purpose) must not re-acquire.
        struct SharedGuard
        {
            bool held = false;
            SharedGuard()
            {
                if (t_lock_depth++ == 0)
                {
                    AcquireSRWLockShared(&g_lock);
                    held = true;
                }
            }
            ~SharedGuard()
            {
                --t_lock_depth;
                if (held) ReleaseSRWLockShared(&g_lock);
            }
        };

        std::string utf8(const std::wstring &w)
        {
            int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string s(n > 0 ? n - 1 : 0, '\0');
            if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
            return s;
        }

        std::wstring source_path() { return g_dir + L"goblin_overlay_render.dll"; }

        bool read_source_id(uint64_t &mtime, uint64_t &size)
        {
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (!GetFileAttributesExW(source_path().c_str(), GetFileExInfoStandard, &fa)) return false;
            mtime = (static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime;
            size = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
            return true;
        }

        // The built DLL is COPIED before LoadLibrary (Windows locks a loaded module's file — loading
        // the original directly would make the very first rebuild fail to link). Copies are
        // per-generation names because the currently-loaded copy is itself locked.
        bool copy_and_load(unsigned gen, RenderExports &out)
        {
            std::wstring dest = g_dir + L"goblin_overlay_render.hot" + std::to_wstring(gen) + L".dll";
            if (!CopyFileW(source_path().c_str(), dest.c_str(), FALSE))
            {
                spdlog::error("[HOTRELOAD] CopyFile → {} failed, gle={}", utf8(dest), GetLastError());
                return false;
            }
            HMODULE m = LoadLibraryW(dest.c_str());
            if (!m)
            {
                spdlog::error("[HOTRELOAD] LoadLibrary({}) failed, gle={}", utf8(dest), GetLastError());
                return false;
            }
            RenderExports e;
            e.module = m;
            e.draw_panel = reinterpret_cast<DrawPanelFn>(GetProcAddress(m, "MFG_DrawPanel"));
            e.draw_worldmap_markers =
                reinterpret_cast<DrawWorldmapMarkersFn>(GetProcAddress(m, "MFG_DrawWorldmapMarkers"));
            e.draw_minimap_hud = reinterpret_cast<DrawMinimapHudFn>(GetProcAddress(m, "MFG_DrawMinimapHud"));
            e.prebuild_markers = reinterpret_cast<PrebuildMarkersFn>(GetProcAddress(m, "MFG_PrebuildMarkers"));
            e.inworld_hovered = reinterpret_cast<InworldHoveredFn>(GetProcAddress(m, "MFG_InworldHovered"));
            e.refresh_overlay_census =
                reinterpret_cast<RefreshOverlayCensusFn>(GetProcAddress(m, "MFG_RefreshOverlayCensus"));
            e.render_idle = reinterpret_cast<RenderIdleFn>(GetProcAddress(m, "MFG_RenderIdle"));
            if (!e.draw_panel || !e.draw_worldmap_markers || !e.draw_minimap_hud || !e.prebuild_markers ||
                !e.inworld_hovered || !e.refresh_overlay_census || !e.render_idle)
            {
                spdlog::error("[HOTRELOAD] GetProcAddress failed for one or more render exports in {}", utf8(dest));
                FreeLibrary(m);
                return false;
            }
            out = e;
            return true;
        }

        void delete_stale_copies()
        {
            WIN32_FIND_DATAW fd{};
            HANDLE h = FindFirstFileW((g_dir + L"goblin_overlay_render.hot*.dll").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) return;
            do
                DeleteFileW((g_dir + fd.cFileName).c_str());  // best-effort; a locked leftover just stays
            while (FindNextFileW(h, &fd));
            FindClose(h);
        }

        // Dev-only poll (500ms) of the BUILT DLL next to the host. Flags a reload only once the new
        // file is stable across two polls AND exclusively openable (the linker is done writing) —
        // maybe_reload() on the present thread consumes the flag between frames.
        void watcher_main()
        {
            uint64_t prev_mtime = 0, prev_size = 0;
            for (;;)
            {
                Sleep(500);
                if (g_reload_pending.load(std::memory_order_relaxed)) continue;  // one at a time
                uint64_t mt = 0, sz = 0;
                if (!read_source_id(mt, sz)) continue;  // mid-relink delete/replace window
                bool changed = mt != g_loaded_mtime.load(std::memory_order_relaxed) ||
                               sz != g_loaded_size.load(std::memory_order_relaxed);
                bool stable = mt == prev_mtime && sz == prev_size;
                prev_mtime = mt;
                prev_size = sz;
                if (!changed || !stable) continue;
                HANDLE f = CreateFileW(source_path().c_str(), GENERIC_READ, 0 /* no sharing */, nullptr,
                                       OPEN_EXISTING, 0, nullptr);
                if (f == INVALID_HANDLE_VALUE) continue;  // writer still holds it
                CloseHandle(f);
                spdlog::info("[HOTRELOAD] new goblin_overlay_render.dll detected → swap on next frame");
                g_reload_pending.store(true, std::memory_order_release);
            }
        }
    }

    // goblin_overlay_render.dll lives next to the host DLL — resolve via THIS module's own path
    // (GetModuleHandleExW + GetModuleFileNameW), not default DLL search order. Idempotent: safe to
    // call from multiple init sites (dllmain.cpp's early init needs prebuild_markers() resolved
    // before goblin::overlay::initialize() runs — see call order note in the header).
    bool load()
    {
        static bool s_attempted = false, s_ok = false;
        if (s_attempted) return s_ok;
        s_attempted = true;

        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                reinterpret_cast<LPCWSTR>(&load), &self))
        {
            spdlog::error("[OVERLAY] GetModuleHandleExW(self) failed, gle={}", GetLastError());
            return false;
        }
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(self, path, MAX_PATH);
        std::wstring dir(path);
        size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) dir.resize(slash + 1);
        g_dir = dir;

        delete_stale_copies();
        if (!copy_and_load(0, g_cur)) return false;
        uint64_t mt = 0, sz = 0;
        if (read_source_id(mt, sz))
        {
            g_loaded_mtime.store(mt, std::memory_order_relaxed);
            g_loaded_size.store(sz, std::memory_order_relaxed);
        }
        std::thread(watcher_main).detach();  // host never unloads; process exit reaps it
        spdlog::info("[OVERLAY] render module loaded (hot-reload watcher armed): {}gen0", utf8(g_dir));
        s_ok = true;
        return true;
    }

    // Present-thread only (top of hk_present, between frames — no draw call can be mid-flight on
    // THIS thread). Other threads' calls (inworld_hovered from wndproc, refresh_overlay_census from
    // the section-visibility watcher) hold g_lock shared, so the exclusive acquire below fences them.
    // The old module's detached disk-build worker does NOT go through call_* — MFG_RenderIdle gates
    // on it, and the deferred g_prev_module free covers its post-idle tail.
    void maybe_reload()
    {
        if (!g_reload_pending.load(std::memory_order_acquire)) return;
        if (!g_cur.render_idle())
        {
            // Old module's disk-build worker still running (~0.7s worst case) — retry next frame.
            return;
        }

        AcquireSRWLockExclusive(&g_lock);
        // De-register the host-held pointer to the OLD module's kick_disk_build before anything else
        // (loot_disk would otherwise call into a freed module). Re-registered by the new module's
        // prebuild_markers below.
        goblin::worldmap::set_build_trigger(nullptr);
        if (!g_cur.render_idle())
        {
            // A call_* that slipped in between the check above and the exclusive acquire started a
            // worker — restore the trigger (old module's prebuild re-registers it) and retry next frame.
            ReleaseSRWLockExclusive(&g_lock);
            g_cur.prebuild_markers();
            return;
        }

        RenderExports next;
        if (!copy_and_load(++g_generation, next))
        {
            ReleaseSRWLockExclusive(&g_lock);
            g_cur.prebuild_markers();  // restore the build trigger on the still-live old module
            g_reload_pending.store(false, std::memory_order_release);
            spdlog::error("[HOTRELOAD] reload failed — old render module kept; rebuild to retry");
            return;
        }

        if (g_prev_module) FreeLibrary(g_prev_module);
        g_prev_module = g_cur.module;
        g_cur = next;
        uint64_t mt = 0, sz = 0;
        if (read_source_id(mt, sz))
        {
            g_loaded_mtime.store(mt, std::memory_order_relaxed);
            g_loaded_size.store(sz, std::memory_order_relaxed);
        }
        ReleaseSRWLockExclusive(&g_lock);

        // Fresh module = fresh statics: rebuild the marker buckets (also re-registers the build
        // trigger) + census. Raw fn pointers on purpose — call_* would shared-acquire needlessly.
        g_cur.prebuild_markers();
        g_cur.refresh_overlay_census();
        g_reload_pending.store(false, std::memory_order_release);
        spdlog::info("[HOTRELOAD] render gen{} live (prev module free deferred one reload)", g_generation);
    }

    // Phase 3 debug RPC: force the same flag the watcher sets (the next frame's maybe_reload
    // consumes it — same stability gates apply since copy_and_load re-reads the file).
    bool request_reload()
    {
        g_reload_pending.store(true, std::memory_order_release);
        return true;
    }
    unsigned render_generation() { return g_generation; }
    bool reload_pending() { return g_reload_pending.load(std::memory_order_acquire); }

    // draw_panel/draw_worldmap_markers/draw_minimap_hud are safe-by-construction: hk_present (their
    // only caller) never installs if goblin::overlay::initialize() saw load() fail (g_failed gates
    // it). prebuild_markers/inworld_hovered/refresh_overlay_census are called from OTHER unrelated
    // init/input/watcher-thread code that does NOT check g_failed, so those three DO need a null
    // guard for the load-failed case.
    void call_draw_panel(const goblin::overlay::OverlayFrameCtx &ctx)
    {
        SharedGuard g;
        g_cur.draw_panel(&ctx);
    }
    void call_draw_worldmap_markers(bool menu_open, const goblin::overlay::OverlayFrameCtx &ctx)
    {
        SharedGuard g;
        g_cur.draw_worldmap_markers(menu_open, &ctx);
    }
    void call_draw_minimap_hud(const goblin::overlay::OverlayFrameCtx &ctx)
    {
        SharedGuard g;
        g_cur.draw_minimap_hud(&ctx);
    }
    void call_prebuild_markers()
    {
        SharedGuard g;
        if (g_cur.prebuild_markers) g_cur.prebuild_markers();
    }
    bool call_inworld_hovered()
    {
        SharedGuard g;
        return g_cur.inworld_hovered && g_cur.inworld_hovered() != 0;
    }
    void call_refresh_overlay_census()
    {
        SharedGuard g;
        if (g_cur.refresh_overlay_census) g_cur.refresh_overlay_census();
    }
#else
    // Default single-DLL build: direct calls, no indirection, nothing to load or reload.
    bool load() { return true; }
    void maybe_reload() {}
    bool request_reload() { return false; }
    unsigned render_generation() { return 0; }
    bool reload_pending() { return false; }

    void call_draw_panel(const goblin::overlay::OverlayFrameCtx &ctx) { goblin::overlay::draw_panel(ctx); }
    void call_draw_worldmap_markers(bool menu_open, const goblin::overlay::OverlayFrameCtx &ctx)
    {
        goblin::overlay::draw_worldmap_markers(menu_open, ctx);
    }
    void call_draw_minimap_hud(const goblin::overlay::OverlayFrameCtx &ctx) { goblin::overlay::draw_minimap_hud(ctx); }
    void call_prebuild_markers() { goblin::worldmap::prebuild_markers(); }
    bool call_inworld_hovered() { return goblin::worldmap::inworld_hovered(); }
    void call_refresh_overlay_census() { goblin::worldmap::refresh_overlay_census(); }
#endif
}
