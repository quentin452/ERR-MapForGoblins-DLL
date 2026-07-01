#pragma once

// Slice C of docs/plans/overlay_hot_reload_playwright_plan.md: the host-side half of the
// host→render call direction. Consolidates ALL calls the host makes into render-side code (not
// just the 3 draw functions — dllmain.cpp/input_wndproc.cpp/goblin_section_visibility.cpp also
// call render-defined goblin::worldmap::prebuild_markers/inworld_hovered/refresh_overlay_census,
// a gap only the real link-time split surfaced) behind one set of call_*() functions. Callers use
// these unconditionally; internally they either call directly (default single-DLL build) or
// through a GetProcAddress-resolved function pointer (GOBLIN_OVERLAY_HOTRELOAD_BUILD) — no
// branching needed at the call site either way.

#include "goblin_overlay_render.hpp"  // OverlayFrameCtx

namespace goblin::overlay_render_loader
{
    // One-time LoadLibrary(goblin_overlay_render.dll) + GetProcAddress for every export below.
    // No-op returning true in the default single-DLL build. Called once from
    // goblin::overlay::initialize(); false → caller sets g_failed (mod disables gracefully).
    bool load();

    void call_draw_panel(const goblin::overlay::OverlayFrameCtx &ctx);
    void call_draw_worldmap_markers(bool menu_open, const goblin::overlay::OverlayFrameCtx &ctx);
    void call_draw_minimap_hud(const goblin::overlay::OverlayFrameCtx &ctx);

    void call_prebuild_markers();
    bool call_inworld_hovered();
    void call_refresh_overlay_census();
}
