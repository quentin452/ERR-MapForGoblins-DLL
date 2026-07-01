#include "goblin_overlay_render_loader.hpp"

#include <spdlog/spdlog.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

#if !defined(GOBLIN_OVERLAY_HOTRELOAD_BUILD)
#include "worldmap/map_entry_layer.hpp"  // prebuild_markers, refresh_overlay_census (same binary)
#include "worldmap/map_renderer.hpp"     // inworld_hovered (same binary)
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

        HMODULE g_render_module = nullptr;
        DrawPanelFn g_fn_draw_panel = nullptr;
        DrawWorldmapMarkersFn g_fn_draw_worldmap_markers = nullptr;
        DrawMinimapHudFn g_fn_draw_minimap_hud = nullptr;
        PrebuildMarkersFn g_fn_prebuild_markers = nullptr;
        InworldHoveredFn g_fn_inworld_hovered = nullptr;
        RefreshOverlayCensusFn g_fn_refresh_overlay_census = nullptr;
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
        std::wstring render_path = dir + L"goblin_overlay_render.dll";

        g_render_module = LoadLibraryW(render_path.c_str());
        if (!g_render_module)
        {
            spdlog::error("[OVERLAY] LoadLibrary(goblin_overlay_render.dll) failed, gle={}", GetLastError());
            return false;
        }
        g_fn_draw_panel = reinterpret_cast<DrawPanelFn>(GetProcAddress(g_render_module, "MFG_DrawPanel"));
        g_fn_draw_worldmap_markers =
            reinterpret_cast<DrawWorldmapMarkersFn>(GetProcAddress(g_render_module, "MFG_DrawWorldmapMarkers"));
        g_fn_draw_minimap_hud =
            reinterpret_cast<DrawMinimapHudFn>(GetProcAddress(g_render_module, "MFG_DrawMinimapHud"));
        g_fn_prebuild_markers =
            reinterpret_cast<PrebuildMarkersFn>(GetProcAddress(g_render_module, "MFG_PrebuildMarkers"));
        g_fn_inworld_hovered =
            reinterpret_cast<InworldHoveredFn>(GetProcAddress(g_render_module, "MFG_InworldHovered"));
        g_fn_refresh_overlay_census =
            reinterpret_cast<RefreshOverlayCensusFn>(GetProcAddress(g_render_module, "MFG_RefreshOverlayCensus"));
        if (!g_fn_draw_panel || !g_fn_draw_worldmap_markers || !g_fn_draw_minimap_hud ||
            !g_fn_prebuild_markers || !g_fn_inworld_hovered || !g_fn_refresh_overlay_census)
        {
            spdlog::error("[OVERLAY] GetProcAddress failed for one or more render exports");
            return false;
        }
        int n = WideCharToMultiByte(CP_UTF8, 0, render_path.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string render_path_utf8(n > 0 ? n - 1 : 0, '\0');
        if (n > 0)
            WideCharToMultiByte(CP_UTF8, 0, render_path.c_str(), -1, render_path_utf8.data(), n, nullptr, nullptr);
        spdlog::info("[OVERLAY] render module loaded: {}", render_path_utf8);
        s_ok = true;
        return true;
    }

    // draw_panel/draw_worldmap_markers/draw_minimap_hud are safe-by-construction: hk_present (their
    // only caller) never installs if goblin::overlay::initialize() saw load() fail (g_failed gates
    // it). prebuild_markers/inworld_hovered/refresh_overlay_census are called from OTHER unrelated
    // init/input/watcher-thread code that does NOT check g_failed, so those three DO need a null
    // guard for the load-failed case.
    void call_draw_panel(const goblin::overlay::OverlayFrameCtx &ctx) { g_fn_draw_panel(&ctx); }
    void call_draw_worldmap_markers(bool menu_open, const goblin::overlay::OverlayFrameCtx &ctx)
    {
        g_fn_draw_worldmap_markers(menu_open, &ctx);
    }
    void call_draw_minimap_hud(const goblin::overlay::OverlayFrameCtx &ctx) { g_fn_draw_minimap_hud(&ctx); }
    void call_prebuild_markers() { if (g_fn_prebuild_markers) g_fn_prebuild_markers(); }
    bool call_inworld_hovered() { return g_fn_inworld_hovered && g_fn_inworld_hovered() != 0; }
    void call_refresh_overlay_census() { if (g_fn_refresh_overlay_census) g_fn_refresh_overlay_census(); }
#else
    // Default single-DLL build: direct calls, no indirection, nothing to load.
    bool load() { return true; }

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
