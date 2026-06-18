#pragma once

// Thread 6 — in-game ImGui settings overlay.
//
// Draws a Dear ImGui panel ON TOP of the game's DX12 framebuffer via a
// swapchain Present hook (the panel floats over whatever is on screen, e.g. the
// 2D world map). All rendering happens INSIDE the Present hook on the game's
// render thread, using the game's own command queue (captured from
// ExecuteCommandLists) — this is the discipline that keeps it stable under
// vkd3d-proton, where a mis-threaded overlay (EROverlay) crashes.
//
// Phase 1: resolve the vtables, install the hooks, and draw a hello window
// toggled by a hotkey. No settings binding yet — that is Phase 3.

namespace goblin::overlay
{
    // Resolve the DXGI/D3D12 vtables (via a throwaway device+swapchain) and
    // install the Present / ExecuteCommandLists / ResizeBuffers hooks. Safe to
    // call once, after the game's renderer is up. No-op (logs) on failure — the
    // mod keeps working without the overlay.
    void initialize();

    // Uninstall hooks + release ImGui/D3D resources. Called on detach.
    void shutdown();

    // True once the first frame has initialised ImGui against the live swapchain.
    bool is_ready();
}
