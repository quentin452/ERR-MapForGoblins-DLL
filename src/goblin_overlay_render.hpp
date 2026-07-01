#pragma once

// Phase 2 (Slice B) of docs/plans/overlay_hot_reload_playwright_plan.md: the ImGui draw layer
// (draw_panel/draw_worldmap_markers/draw_minimap_hud + their private helpers), now living in
// src/goblin_overlay_render.cpp — a separate translation unit from the host (src/goblin_overlay.cpp,
// which keeps hk_present/D3D12 device-swapchain-lifecycle ownership). Still ONE binary today
// (GOBLIN_OVERLAY_HOTRELOAD scaffolded but not yet acted on, see CMakeLists.txt) — this header is
// the boundary Slice C's real DLL split will reuse.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <imgui.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace goblin::overlay
{
    // Per-item icon GPU SRV cache entry (host-owned storage, draw layer reads it via OverlayFrameCtx).
    struct ItemIconSrv
    {
        ID3D12Resource *tex = nullptr;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
        bool ok = false;
        ImVec2 uv0{0, 0}, uv1{1, 1};
    };

    // One harvested SB_ERR_Grace_* candidate's debug-preview SRV (F1 "GPU debug" grace viewer).
    struct GraceDbg
    {
        ImTextureID tex;
        ImVec2 uv0, uv1;
        std::string name;
        int w, h;
    };

    // Explicit per-frame context the draw functions take instead of reaching into host file-statics
    // directly (Phase 1). Fields are pointers/handles into host-owned statics, not copies — other
    // code outside the draw layer (hk_present's XInput poll, flush_item_icon_batch) still reads/
    // writes the originals every frame.
    struct OverlayFrameCtx
    {
        void *atlas_srv;              // g_atlas_ready ? g_atlas_gpu.ptr : nullptr, host-resolved
        HWND hwnd;                    // g_hwnd, host-owned window handle
        std::atomic<int> *nav_frames; // &g_nav_frames, host-owned nav-jitter keepalive counter

        bool *gamepad_combo_recording;            // &g_gamepad_combo_recording, also R+W by hk_present's XInput poll
        const bool *gamepad_combo_ready;          // &g_gamepad_combo_ready, host-written only
        std::string *gamepad_combo_reject_reason; // &g_gamepad_combo_reject_reason, also written by hk_present
        std::map<int, ItemIconSrv> *item_icon_srvs; // &g_item_icon_srvs, also written by host-called flush_item_icon_batch
    };

    void draw_worldmap_markers(bool menu_open, const OverlayFrameCtx &ctx);
    void draw_minimap_hud(const OverlayFrameCtx &ctx);
    void draw_panel(const OverlayFrameCtx &ctx);

    // Slice B wrapper surface: mechanical forwards to host implementations that stay in
    // goblin_overlay.cpp because they operate on host-owned g_device/g_command_queue/g_srv_heap/
    // g_frames/g_command_list — the same per-frame D3D12 state hk_present resets every frame. Found
    // NOT self-contained during the Slice B move (see docs/plans/overlay_hot_reload_playwright_plan.md);
    // Slice C may revisit this boundary when the real DLL split happens.
    UINT64 ensure_item_icon_srv(int iconId);
    bool ensure_grace_srv();
    bool ensure_grace_dungeon_srv();
    void force_rebuild_grace();
    void ensure_grace_debug();
    UINT64 copy_er_sheet_direct(int &outW, int &outH);
    UINT64 create_tex_from_dds_mem(const uint8_t *data, size_t len, int &outW, int &outH, DXGI_FORMAT &outFmt);

    int grace_state();
    int grace_dbg_fmt_used();
    int *grace_dbg_srgb_ptr();
    int *grace_dbg_swiz_ptr();
    void grace_srv_info(void *&gpu, ImVec2 &uv0, ImVec2 &uv1, int &native_w, int &native_h);
    void grace_dungeon_srv_info(void *&gpu, ImVec2 &uv0, ImVec2 &uv1);
    const std::vector<GraceDbg> &grace_debug_candidates();
}
