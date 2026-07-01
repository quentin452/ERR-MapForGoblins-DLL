#pragma once

// Slice C of docs/plans/overlay_hot_reload_playwright_plan.md: cross-DLL export macro for the
// render→host call direction (the ~110+ goblin::overlay_api::* wrappers + native_item_icon
// family declared in goblin_overlay_render_api.hpp). Host is never reloaded, so this direction
// uses ordinary load-time dllexport/dllimport linking — only the host→render direction (the 3
// draw functions) needs the GetProcAddress-based mechanism, since render IS the module that gets
// swapped out live (see the plan's "Host → render" design note).
//
// No-op (empty macro) unless GOBLIN_OVERLAY_HOTRELOAD_BUILD is defined — the default single-DLL
// build (GOBLIN_OVERLAY_HOTRELOAD=OFF) never sets it, so this header changes nothing there.
// CMake auto-defines `<target>_EXPORTS` (here `MapForGoblins_EXPORTS`) only when compiling a
// source file that belongs to that target — no manual wiring needed beyond passing
// GOBLIN_OVERLAY_HOTRELOAD_BUILD to both the host and render CMake targets.

#if defined(GOBLIN_OVERLAY_HOTRELOAD_BUILD)
  #if defined(MapForGoblins_EXPORTS)
    #define GOBLIN_RENDER_API __declspec(dllexport)
  #else
    #define GOBLIN_RENDER_API __declspec(dllimport)
  #endif
#else
  #define GOBLIN_RENDER_API
#endif
