#pragma once

// Slice C of docs/plans/overlay_hot_reload_playwright_plan.md: the host-side half of the
// host→render call direction. Consolidates ALL calls the host makes into render-side code (not
// just the 3 draw functions — dllmain.cpp/input_wndproc.cpp/goblin_section_visibility.cpp also
// call render-defined goblin::worldmap::prebuild_markers/inworld_hovered/refresh_overlay_census,
// a gap only the real link-time split surfaced) behind one set of call_*() functions. Callers use
// these unconditionally; internally they either call directly (default single-DLL build) or
// through a GetProcAddress-resolved function pointer (GOBLIN_OVERLAY_HOTRELOAD_BUILD) — no
// branching needed at the call site either way.

#include <string>

#include "goblin_overlay_render.hpp"  // OverlayFrameCtx

namespace goblin::overlay_render_loader
{
    // One-time LoadLibrary(goblin_overlay_render.dll) + GetProcAddress for every export below.
    // No-op returning true in the default single-DLL build. Called once from
    // goblin::overlay::initialize(); false → caller sets g_failed (mod disables gracefully).
    // Slice D: loads a per-generation COPY (goblin_overlay_render.hot<N>.dll — Windows locks a
    // loaded module's file, so loading the original would break the first rebuild) and arms a
    // 500ms file-watcher thread that flags a reload when a newly-linked DLL appears.
    bool load();

    // Slice D: consume a watcher-flagged render-DLL swap. Present-thread only, called at the top of
    // hk_present (between frames). Gates on the old module's disk-build worker being idle
    // (MFG_RenderIdle), swaps under the same SRW lock the call_*() below hold shared, re-runs the
    // new module's prebuild_markers/refresh_overlay_census (fresh statics), and defers the old
    // module's FreeLibrary by one reload (grace for the worker thread's post-idle tail). No-op in
    // the default single-DLL build and when nothing is pending.
    void maybe_reload();

    // Phase 3 debug-RPC surface. request_reload() force-flags a swap exactly as the file watcher
    // would (consumed by the next frame's maybe_reload); false in the default single-DLL build.
    // render_generation() = how many swaps have happened (0 = initial module, and always 0 in the
    // default build); an RPC client polls it to see its rebuild go live. reload_pending() = a swap
    // is flagged but not yet consumed.
    bool request_reload();
    unsigned render_generation();
    bool reload_pending();

    void call_draw_panel(const goblin::overlay::OverlayFrameCtx &ctx);
    void call_draw_worldmap_markers(bool menu_open, const goblin::overlay::OverlayFrameCtx &ctx);
    void call_draw_minimap_hud(const goblin::overlay::OverlayFrameCtx &ctx);
    void call_draw_virtual_map(const goblin::overlay::OverlayFrameCtx &ctx);

    void call_prebuild_markers();
    bool call_inworld_hovered();
    void call_refresh_overlay_census();
    // Host→render calls the host makes into render-resident marker/relief/panel code (2026-07-05
    // split resync). Same both-build indirection as above.
    void call_rebuild_markers();
    void call_service_pending_warp();
    std::string call_build_far_relief(int group, int cellSize);
    std::string call_far_relief_probe();
    std::string call_vmap_command(const std::string &rest);
    void call_request_f1_tab(int idx);
}
