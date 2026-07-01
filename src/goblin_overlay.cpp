#include "goblin_overlay.hpp"
#include "goblin_overlay_render.hpp"
#include "goblin_config.hpp"
#include "goblin_quest_steps.hpp"
#include "goblin_debug_events.hpp"

#include <chrono>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <Xinput.h>   // XINPUT_STATE struct used by hk_present's own poll (real state, via the trampoline)

#include <MinHook.h>
#include <spdlog/spdlog.h>

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include "goblin_inject.hpp"   // goblin::world_map_open()
#include "goblin_markers.hpp"   // goblin::markers::set_event_flag()
#include "goblin_worldmap_probe.hpp"   // get_live_view() for the marker prototype
#include "goblin_map_data.hpp"         // generated::MAP_ENTRIES (graces for Phase 1)
#include "worldmap/grace_layer.hpp"      // goblin::worldmap::GraceLayer
#include "worldmap/quest_npc_layer.hpp"  // goblin::worldmap::QuestNpcLayer
#include "worldmap/map_entry_layer.hpp"  // goblin::worldmap::MapEntryLayer
#include "worldmap/map_renderer.hpp"     // goblin::worldmap::render_markers
#include "worldmap/category_meta.hpp"    // baked→GPU icon migration counters (F1 panel)
#include "worldmap/loot_disk.hpp"        // disk_loot_state — F1 "maps not found" error
#include "re_signatures.hpp"             // sig_health — F1 "signatures unresolved" error
#include "goblin_messages.hpp"           // lookup_text_utf8 (item-search name resolution)
#include "generated_shared/goblin_overlay_icons.hpp" // ATLAS_PNG category-icon atlas
#include "generated_shared/dejavu_sans_ttf.h"         // embedded DejaVu Sans (extended-Latin glyphs)
#include "stb_image.h"                                // stbi_load_from_memory (PNG decode)
#include "goblin_bench.hpp"                           // GOBLIN_BENCH scoped timers
#include "input/input_shared.hpp"                     // goblin::input::menu_open()
#include "input/input_directinput.hpp"                // goblin::input::install_directinput_hooks()
#include "input/input_gamepad.hpp"                    // goblin::input::install_xinput_hook() etc.
#include "input/input_cursor.hpp"                     // goblin::input::install_cursor_hooks() etc.
#include "input/input_rawinput.hpp"                   // goblin::input::install_rawinput_hooks() etc.
#include "input/input_wndproc.hpp"                    // goblin::input::install_wndproc_hook() etc.
#include "input/input_keyboard_poll.hpp"              // goblin::input::poll_keyboard_text_input()

#include <vector>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <string>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <mutex>
#include <thread>

// ImGui's Win32 backend message handler (defined in imgui_impl_win32.cpp).
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
    // Phase 2 Slice B (docs/plans/overlay_hot_reload_playwright_plan.md): these types now live in
    // goblin_overlay_render.hpp (shared with the extracted draw layer, src/goblin_overlay_render.cpp)
    // — pulled in unqualified here so the rest of this file's existing code doesn't need touching.
    using goblin::overlay::ItemIconSrv;
    using goblin::overlay::GraceDbg;
    using goblin::overlay::OverlayFrameCtx;

    // ── Hooked function typedefs ──────────────────────────────────────────
    using PresentFn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain3 *, UINT, UINT);
    using ResizeBuffersFn =
        HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain3 *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    using ExecuteCommandListsFn =
        void(STDMETHODCALLTYPE *)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);

    PresentFn o_present = nullptr;
    ResizeBuffersFn o_resize_buffers = nullptr;
    ExecuteCommandListsFn o_execute_command_lists = nullptr;

    // Cursor hooks (SetCursorPos/ClipCursor/GetCursorPos) moved to src/input/input_cursor.cpp
    // (goblin::input::install_cursor_hooks() / set_cursor_pos_real() / clip_cursor_real() /
    // get_cursor_pos_real() / set_imgui_reading_cursor()) — third slice of
    // docs/plans/input_module_refactor_plan.md.

    // XInputGetState hook moved to src/input/input_gamepad.cpp (goblin::input::
    // install_xinput_hook() / xinput_available() / xinput_get_state_real()) — second slice
    // of docs/plans/input_module_refactor_plan.md.

    // Raw input hooks moved to src/input/input_rawinput.cpp (goblin::input::
    // install_rawinput_hooks() / virtual_cursor_x() / virtual_cursor_y()) — fourth slice of
    // docs/plans/input_module_refactor_plan.md.

    // DirectInput8 hooks moved to src/input/input_directinput.cpp (goblin::input::
    // install_directinput_hooks()) — first slice of docs/plans/input_module_refactor_plan.md.

    // ── D3D12 state captured from the live game ───────────────────────────
    ID3D12Device *g_device = nullptr;
    ID3D12CommandQueue *g_command_queue = nullptr;   // captured from ExecuteCommandLists
    ID3D12DescriptorHeap *g_rtv_heap = nullptr;
    ID3D12DescriptorHeap *g_srv_heap = nullptr;       // [0] ImGui font, [1] icon atlas
    ID3D12GraphicsCommandList *g_command_list = nullptr;

    // Category-icon atlas (SRV index 1) — uploaded once from the baked RGBA so the
    // overlay-rendered map can draw real icons instead of coloured circles.
    ID3D12Resource *g_atlas_tex = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE g_atlas_gpu{};
    bool g_atlas_ready = false;

    struct FrameContext
    {
        ID3D12CommandAllocator *allocator = nullptr;
        ID3D12Resource *render_target = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle{};
    };
    std::vector<FrameContext> g_frames;
    UINT g_buffer_count = 0;

    HWND g_hwnd = nullptr;
    // Message-driven focus state (dx-bugs 2026-07-01 "alt-tab back, ImGui receives no input"
    // followup). Was previously re-polled every present frame via `GetForegroundWindow() ==
    // g_hwnd`; [FOCUSDIAG] logging showed g_show flapping true/false SEVEN times in the ~20s
    // after a single real Alt+Tab-back (one WM_SETFOCUS), with no further real focus change —
    // GetForegroundWindow() briefly returns something other than g_hwnd for a few frames during
    // the Wine/compositor alt-tab transition, and the per-frame poll caught those transient
    // states. Each flap closed+reopened the ImGui window (draw is gated on g_show), resetting
    // ImGui's per-window hover/focus continuity every time — nothing had a stable frame to
    // register a click on. WM_SETFOCUS/WM_KILLFOCUS are event-driven and only fire on REAL
    // transitions, so tracking focus from them instead of polling is immune to this.
    std::atomic<bool> g_has_focus{true};

    bool g_imgui_init = false;   // ImGui + D3D resources built against live swapchain
    bool g_failed = false;       // gave up (no overlay), mod continues
    bool g_show = false;         // panel visible this frame
    // diag (docs/re/proton11_cursor_lock_re_prompt.md step 1): call counts for the 5 cursor/raw-input
    // detours, logged once/sec while the panel is open. On Proton 11 the F1 cursor can be frozen at
    // screen-centre with no hover/click/move at all -- these counters tell us whether ER's mouse
    // capture even reaches the user32 exports we hook (0 calls while ER clearly captures the mouse =
    // it's bypassing user32 entirely, e.g. native Wayland pointer-lock or a win32u-only path).
    // (All 6 hook-owned counters — the 3 cursor-hook ones, the raw-input pair, and wm_char/
    // wm_keydown/wndproc_lbdown_while_open — moved to src/input/input_cursor.cpp /
    // input_rawinput.cpp / input_wndproc.cpp's diag_*_exchange() accessors along with their
    // owning hooks.)
    // Visual cursor diagnostic (dx-bugs 2026-07-01 Alt+Tab followup, config::debugCursorDiagnostic).
    // Set in the mouse-poll block (client-relative, always captured regardless of whether the
    // baseline gate feeds it to ImGui) and drawn as a crosshair after ImGui::NewFrame(), further
    // down in the same function — file-scope so it survives across that gap without threading it
    // through as a parameter.
    POINT g_diag_raw_cursor_client{};
    bool g_diag_raw_cursor_valid = false;

    // Virtual cursor (accumulated from raw mouse deltas, kept only as the [DIAG] on-screen
    // comparison value) moved to src/input/input_rawinput.cpp along with the raw-input hooks
    // that feed it — goblin::input::virtual_cursor_x()/virtual_cursor_y().

    // Item-search nav window: while > 0, a locate/page-switch is in flight and the input hooks inject a
    // tiny net-zero mouse jitter so the game keeps processing its world-map (otherwise, with the panel
    // open, input is blanked -> the map's view/page step doesn't run -> our switch+pan only apply once
    // F1 closes). Set on a result click, counted down each Present frame.
    std::atomic<int> g_nav_frames{0};
    bool g_user_show = false;    // F1 master open/close (works anywhere = the menu keybind)
    // Debounce for the VK_F1 toggle READ itself (dx-bugs 2026-07-01 "after Alt+Tab, can't click
    // in F1" followup — [FOCUSDIAG] showed the SAME repeated-g_show-flapping-with-no-focus-event
    // signature already found+fixed for the gamepad combo below, but on a session that already
    // has that fix — so either the user is mashing F1 out of frustration at the click bug, or
    // this GetAsyncKeyState poll has an analogous glitch. Debounce is cheap insurance either way
    // (2 frames, ~33ms, not perceptible as a real press); the [TOGGLEDIAG] log on commit (see the
    // two commit sites below) tells them apart definitively next time it happens.
    int g_toggle_kb_streak = 0;
    bool g_toggle_kb_armed = true;
    static constexpr int kToggleKbDebounceFrames = 2;
    // Debounce for the gamepad toggle-combo READ itself (dx-bugs 2026-07-01 "search bar loses
    // keyboard, no alt-tab" followup). [KBDIAG]/[FOCUSDIAG] logs showed g_show flapping 4+ MORE
    // times after a single WM_SETFOCUS with NO further focus message in between — meaning
    // g_user_show itself was toggling repeatedly, not `fg`. `combo_down` above was a raw
    // single-frame OR-across-4-pads read with a single-frame edge check (line below) — no
    // debounce at all, so a burst of stale/glitchy XInput reads right after a focus regain
    // (a known XInput behavior: the first reads after an app was backgrounded can be a stale/
    // resync burst) could bounce it several times, each bounce flipping g_user_show and
    // resetting ImGui's per-window focus stack — which is why the search bar's InputText never
    // got its keyboard capture back even once the panel visually reappeared for good.
    int g_toggle_gamepad_streak = 0;
    bool g_toggle_gamepad_armed = true;   // ready to fire on the next debounced rising edge
    static constexpr int kToggleGamepadDebounceFrames = 3;  // ~50ms at 60fps
    bool g_last_input_was_gamepad = false;   // cleared on mouse/kb msgs in hk_wndproc
    // Debounce for the mouse/kb->gamepad switch edge (dx-bugs 2026-07-01 followup): a single
    // frame of pad "active" (stick drift, idle hand on the stick) used to be enough to flip
    // g_last_input_was_gamepad, so any real gamepad presence re-armed the recenter within 1
    // frame of a genuine mouse move clearing it — every mouse interaction fought a snap-to-
    // center. Require this many CONSECUTIVE active frames before treating it as a real switch.
    int g_gamepad_active_streak = 0;
    static constexpr int kGamepadSwitchDebounceFrames = 5;  // ~80ms at 60fps
    bool g_ignore_next_mousemove_for_gamepad_flag = false;  // set by our own recenter SetCursorPos call
    bool g_gamepad_combo_recording = false;  // armed by the settings "Record gamepad combo" button
    bool g_gamepad_combo_ready = false;      // false = still waiting for the arming press to release
    std::string g_gamepad_combo_reject_reason;   // non-empty = last recording attempt was rejected, shown in settings

    // ── Helpers ───────────────────────────────────────────────────────────

    // Read a COM object's vtable slot (function pointer) by index.
    void *vtable_entry(void *com_object, int index)
    {
        void **vtable = *reinterpret_cast<void ***>(com_object);
        return vtable[index];
    }

    void release_render_targets()
    {
        for (auto &f : g_frames)
        {
            if (f.render_target) { f.render_target->Release(); f.render_target = nullptr; }
        }
    }

    void cleanup_imgui_device()
    {
        release_render_targets();
        for (auto &f : g_frames)
            if (f.allocator) { f.allocator->Release(); f.allocator = nullptr; }
        g_frames.clear();
        if (g_command_list) { g_command_list->Release(); g_command_list = nullptr; }
        if (g_atlas_tex) { g_atlas_tex->Release(); g_atlas_tex = nullptr; }
        g_atlas_ready = false;
        g_atlas_gpu = D3D12_GPU_DESCRIPTOR_HANDLE{};
        if (g_rtv_heap) { g_rtv_heap->Release(); g_rtv_heap = nullptr; }
        if (g_srv_heap) { g_srv_heap->Release(); g_srv_heap = nullptr; }
    }

    // ── Category-icon atlas upload (SRV index 1) ──────────────────────────
    // One-time GPU upload of the baked RGBA atlas so the overlay map can draw
    // real icons. Ported from upstream (VirusAlex) goblin_overlay.cpp.
    bool upload_rgba(const unsigned char *rgba, int w, int h, UINT srv_index,
                     ID3D12Resource **out_tex, D3D12_GPU_DESCRIPTOR_HANDLE *out_gpu)
    {
        const UINT inc =
            g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_HEAP_PROPERTIES hp_def{};
        hp_def.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = static_cast<UINT64>(w);
        td.Height = static_cast<UINT>(h);
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (FAILED(g_device->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &td,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(out_tex))))
            return false;

        const UINT row = static_cast<UINT>(w) * 4;
        const UINT arow = (row + 255u) & ~255u; // 256-byte row alignment
        const UINT64 upsize = static_cast<UINT64>(arow) * h;
        D3D12_HEAP_PROPERTIES hp_up{};
        hp_up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = upsize;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource *upbuf = nullptr;
        if (FAILED(g_device->CreateCommittedResource(&hp_up, D3D12_HEAP_FLAG_NONE, &bd,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&upbuf))))
        {
            (*out_tex)->Release(); *out_tex = nullptr;
            return false;
        }

        void *mapped = nullptr;
        D3D12_RANGE no_read{0, 0};
        if (SUCCEEDED(upbuf->Map(0, &no_read, &mapped)) && mapped)
        {
            for (int y = 0; y < h; ++y)
                memcpy(static_cast<char *>(mapped) + static_cast<size_t>(y) * arow,
                       rgba + static_cast<size_t>(y) * row, row);
            upbuf->Unmap(0, nullptr);
        }

        g_frames[0].allocator->Reset();
        g_command_list->Reset(g_frames[0].allocator, nullptr);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = *out_tex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = upbuf;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = 0;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        src.PlacedFootprint.Footprint.Width = static_cast<UINT>(w);
        src.PlacedFootprint.Footprint.Height = static_cast<UINT>(h);
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = arow;
        g_command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = *out_tex;
        b.Transition.Subresource = 0;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g_command_list->ResourceBarrier(1, &b);
        g_command_list->Close();
        ID3D12CommandList *lists[] = {g_command_list};
        g_command_queue->ExecuteCommandLists(1, lists);

        ID3D12Fence *fence = nullptr;
        if (SUCCEEDED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        {
            HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            g_command_queue->Signal(fence, 1);
            if (ev && fence->GetCompletedValue() < 1)
            {
                fence->SetEventOnCompletion(1, ev);
                WaitForSingleObject(ev, 1000);
            }
            if (ev) CloseHandle(ev);
            fence->Release();
        }
        upbuf->Release();

        D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_srv_heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(inc) * srv_index;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        g_device->CreateShaderResourceView(*out_tex, &sd, cpu);
        *out_gpu = g_srv_heap->GetGPUDescriptorHandleForHeapStart();
        out_gpu->ptr += static_cast<SIZE_T>(inc) * srv_index;
        return true;
    }

    // ── Per-item icon: copy the live engine sheet sub-rect → our own SRV (sprite findings P2b) ──
    // Looks up a HARVESTED icon (goblin::harvested_icon), CopyTextureRegion's its rect GPU→GPU from
    // the engine's BCn menu sheet into a small dest texture, and makes an SRV (heap slot 2+). Cached
    // per iconId (incl. failures, to not retry every frame). Runs on the render thread. Returns the
    // GPU handle ptr (ImTextureID) or 0 on miss/fail → caller falls back to the baked atlas.
    std::map<int, ItemIconSrv> g_item_icon_srvs;
    UINT g_next_item_srv = 2;   // 0 = ImGui font, 1 = category atlas

    // Sheet-as-atlas: copy each game icon SHEET WHOLE into our own texture ONCE (cached by the game
    // resource ptr), then markers draw with a per-icon UV sub-rect into it. All icons on one sheet
    // share one ImTextureID → ONE ImGui draw call (vs one texture + draw call per cropped icon).
    struct SheetTex { UINT64 gpu = 0; int w = 0, h = 0; };
    std::map<ID3D12Resource *, SheetTex> g_sheet_cache;

    // Icons newly requested this frame: their GPU→GPU copy is RECORDED into a single shared
    // command list and finalized once per frame in flush_item_icon_batch() — one ExecuteCommandLists
    // + one fence wait for the WHOLE batch instead of one submit+blocking-wait per icon (the per-icon
    // WaitForSingleObject(1000) micro-stuttered when many icons appeared at once, e.g. first map open).
    // A requested icon falls back this frame (ensure_* returns 0) and is drawn next frame once the
    // batch completes — 1-frame latency, no stall. Render-thread only (no locking).
    struct PendingIcon { int iconId; ID3D12Resource *tex; DXGI_FORMAT fmt; UINT srv_idx; int w, h; };
    std::vector<PendingIcon> g_pending_icons;
    bool g_icon_batch_open = false;

    // Lazily reset the shared command list for this frame's copy batch (once, on the first icon).
    void begin_icon_batch()
    {
        if (g_icon_batch_open) return;
        g_frames[0].allocator->Reset();
        g_command_list->Reset(g_frames[0].allocator, nullptr);
        g_icon_batch_open = true;
    }

    // ── Shared GPU sub-rect copy (factored out of ensure_item_icon_srv / ensure_grace_srv /
    //    ensure_grace_dungeon_srv, which all did the identical snap-rect → CreateCommittedResource
    //    → barriers → CopyTextureRegion → UV chain). ────────────────────────────────────────────
    struct SrvCopy
    {
        ID3D12Resource *tex = nullptr;  // dest, left in PIXEL_SHADER_RESOURCE
        int w = 0, h = 0;               // dest dims (4-aligned, block-snapped)
        ImVec2 uv0, uv1;                // crop the exact sprite rect back out of the snapped copy
    };

    // Snap the sprite rect OUT to 4-aligned BC blocks (item cells / grace sprites aren't multiples
    // of 4, so a raw sub-rect copy is rejected), create the dest texture, and RECORD into the OPEN
    // g_command_list: src→COPY_SOURCE, CopyTextureRegion, then restore src + dest→PIXEL_SHADER_RESOURCE.
    // Caller must have the list recording (begin_icon_batch / own Reset) and owns submit + the SRV.
    // Returns false (no resource created, nothing recorded) on an invalid rect / create failure.
    bool record_sprite_copy(ID3D12Resource *src_res, DXGI_FORMAT fmt,
                            const goblin::ItemSprite &sp, SrvCopy &out)
    {
        D3D12_RESOURCE_DESC rd = src_res->GetDesc();
        int sw = static_cast<int>(rd.Width), sh = static_cast<int>(rd.Height);
        int x0 = sp.x0 & ~3, y0 = sp.y0 & ~3;
        int x1 = (sp.x1 + 3) & ~3, y1 = (sp.y1 + 3) & ~3;
        if (x1 > sw) x1 = sw;
        if (y1 > sh) y1 = sh;
        int w = x1 - x0, h = y1 - y0;
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024)
            return false;

        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = static_cast<UINT64>(w); td.Height = static_cast<UINT>(h);
        td.DepthOrArraySize = 1; td.MipLevels = 1; td.Format = fmt;
        td.SampleDesc.Count = 1; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ID3D12Resource *tex = nullptr;
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex))))
            return false;

        // src sheet: assume PIXEL_SHADER_RESOURCE → COPY_SOURCE (sampled engine texture).
        D3D12_RESOURCE_BARRIER bs{};
        bs.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bs.Transition.pResource = src_res; bs.Transition.Subresource = 0;
        bs.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        bs.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        g_command_list->ResourceBarrier(1, &bs);

        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = tex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION csrc{}; csrc.pResource = src_res;
        csrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; csrc.SubresourceIndex = 0;
        D3D12_BOX box{}; box.left = x0; box.top = y0; box.front = 0;
        box.right = x1; box.bottom = y1; box.back = 1;
        g_command_list->CopyTextureRegion(&dst, 0, 0, 0, &csrc, &box);

        D3D12_RESOURCE_BARRIER after[2]{};
        after[0] = bs;  // restore src: COPY_SOURCE → PIXEL_SHADER_RESOURCE
        after[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        after[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        after[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        after[1].Transition.pResource = tex; after[1].Transition.Subresource = 0;
        after[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        after[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g_command_list->ResourceBarrier(2, after);

        out.tex = tex; out.w = w; out.h = h;
        out.uv0 = ImVec2(static_cast<float>(sp.x0 - x0) / w, static_cast<float>(sp.y0 - y0) / h);
        out.uv1 = ImVec2(static_cast<float>(sp.x1 - x0) / w, static_cast<float>(sp.y1 - y0) / h);
        return true;
    }

    void submit_and_wait(); // defined just below; used by the DDS loader

    // ── OUR-OWN texture manager: load a raw DDS (BCn) into OUR D3D12 resource, no game bind ───────
    // The game binds its menu sheets lazily on render (so we can only harvest what it draws). To
    // escape that, we load the source DDS ourselves: parse the header, CreateCommittedResource in the
    // file's BC format (GPU samples BCn natively — no CPU decode), upload the blocks, make an SRV.
    // This is the foundation for drawing ANY icon (crop a rect) without waiting for the game.
    DXGI_FORMAT dds_fourcc_to_dxgi(const uint8_t *h, size_t len, size_t &dataOff)
    {
        uint32_t fourcc; std::memcpy(&fourcc, h + 84, 4);
        if (fourcc == 0x30315844) // 'DX10'
        {
            if (len < 148) return DXGI_FORMAT_UNKNOWN;
            uint32_t dxgi; std::memcpy(&dxgi, h + 128, 4);
            dataOff = 148;
            return static_cast<DXGI_FORMAT>(dxgi);
        }
        dataOff = 128;
        switch (fourcc)
        {
        case 0x31545844: return DXGI_FORMAT_BC1_UNORM; // 'DXT1'
        case 0x33545844: return DXGI_FORMAT_BC2_UNORM; // 'DXT3'
        case 0x35545844: return DXGI_FORMAT_BC3_UNORM; // 'DXT5'
        default: return DXGI_FORMAT_UNKNOWN;
        }
    }

    int bc_block_bytes(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM: return 8;
        default: return 16; // BC2/BC3/BC5/BC6H/BC7
        }
    }

    // Load a DDS file from disk into our own SRV. Returns the gpu handle (0 on fail) + dims + format.
    // Synchronous (submit_and_wait). Allocates an SRV slot from g_next_item_srv.
    // Upload a raw DDS blob (in memory — file OR the game's decompressed RAM TPF) into our own SRV.
    UINT64 create_tex_from_dds_mem(const uint8_t *data, size_t len, int &outW, int &outH, DXGI_FORMAT &outFmt)
    {
        outW = outH = 0; outFmt = DXGI_FORMAT_UNKNOWN;
        if (!g_device || !g_command_queue || !g_srv_heap || g_next_item_srv >= 256) return 0;
        if (!data || len < 148 || std::memcmp(data, "DDS ", 4) != 0) { spdlog::warn("[TEXMGR] not DDS"); return 0; }

        uint32_t h, w; std::memcpy(&h, data + 12, 4); std::memcpy(&w, data + 16, 4);
        size_t dataOff = 128;
        DXGI_FORMAT fmt = dds_fourcc_to_dxgi(data, len, dataOff);
        if (fmt == DXGI_FORMAT_UNKNOWN || w == 0 || h == 0) { spdlog::warn("[TEXMGR] bad fmt"); return 0; }
        const int blockBytes = bc_block_bytes(fmt);
        const UINT blocksW = (w + 3) / 4, blocksH = (h + 3) / 4;
        const UINT srcRowPitch = blocksW * blockBytes;
        if (dataOff + (size_t)srcRowPitch * blocksH > len) { spdlog::warn("[TEXMGR] short data ({} < need)", len); return 0; }

        D3D12_HEAP_PROPERTIES dp{}; dp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = fmt; td.SampleDesc.Count = 1; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ID3D12Resource *tex = nullptr;
        if (FAILED(g_device->CreateCommittedResource(&dp, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)))) return 0;

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp0{}; UINT numRows = 0; UINT64 rowSize = 0, total = 0;
        g_device->GetCopyableFootprints(&td, 0, 1, 0, &fp0, &numRows, &rowSize, &total);

        D3D12_HEAP_PROPERTIES up{}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC ub{};
        ub.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; ub.Width = total; ub.Height = 1;
        ub.DepthOrArraySize = 1; ub.MipLevels = 1; ub.Format = DXGI_FORMAT_UNKNOWN;
        ub.SampleDesc.Count = 1; ub.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource *upbuf = nullptr;
        if (FAILED(g_device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &ub,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upbuf)))) { tex->Release(); return 0; }
        uint8_t *map = nullptr; D3D12_RANGE nr{0, 0};
        if (FAILED(upbuf->Map(0, &nr, reinterpret_cast<void **>(&map)))) { upbuf->Release(); tex->Release(); return 0; }
        for (UINT r = 0; r < numRows; ++r)
            std::memcpy(map + fp0.Offset + (size_t)r * fp0.Footprint.RowPitch,
                        data + dataOff + (size_t)r * srcRowPitch, srcRowPitch);
        upbuf->Unmap(0, nullptr);

        // DEDICATED reset (NOT begin_icon_batch): this path always submit_and_wait()s synchronously,
        // so it owns the shared list for one self-contained copy — mirroring ensure_map_sym_srv. Using
        // begin_icon_batch here would record onto (then Close) the per-frame harvested-icon batch and
        // corrupt it (the shared-command-list bug). Callers run after flush_item_icon_batch (ImGui draw).
        g_frames[0].allocator->Reset();
        g_command_list->Reset(g_frames[0].allocator, nullptr);
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = tex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = upbuf;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp0;
        g_command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex; b.Transition.Subresource = 0;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g_command_list->ResourceBarrier(1, &b);
        submit_and_wait();
        g_icon_batch_open = false;  // list is closed/idle now — keep the invariant (next begin resets)
        upbuf->Release(); // copy done (blocked)

        UINT idx = g_next_item_srv++;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_srv_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_srv_heap->GetGPUDescriptorHandleForHeapStart();
        UINT inc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        cpu.ptr += (SIZE_T)idx * inc; gpu.ptr += (UINT64)idx * inc;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Format = fmt; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
        g_device->CreateShaderResourceView(tex, &sd, cpu);
        outW = (int)w; outH = (int)h; outFmt = fmt;
        spdlog::info("[TEXMGR] uploaded DDS {}x{} fmt={} slot={} -> gpu={:#x}", w, h, (int)fmt, idx, gpu.ptr);
        return gpu.ptr;
    }

    // LOAD DIRECT: copy ER's already-resident sheet (the harvested grace sheet's ID3D12Resource)
    // WHOLE into our own texture via CopyResource. No file/DCX — we own a copy of the game's live
    // GPU image. Works once the game has the sheet bound (map open near a grace).
    UINT64 copy_er_sheet_direct(int &outW, int &outH)
    {
        outW = outH = 0;
        if (!g_device || !g_command_queue || !g_srv_heap || g_next_item_srv >= 256) return 0;
        goblin::ItemSprite sp;
        if (!goblin::harvested_grace(sp) || !sp.sheet)
        {
            spdlog::warn("[TEXMGR] no harvested ER sheet yet (open the map near a grace first)");
            return 0;
        }
        auto *src = reinterpret_cast<ID3D12Resource *>(sp.sheet);
        D3D12_RESOURCE_DESC rd = src->GetDesc();
        if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || rd.Format == DXGI_FORMAT_UNKNOWN)
        {
            spdlog::warn("[TEXMGR] ER sheet not bound (GetDesc UNKNOWN)");
            return 0;
        }
        D3D12_HEAP_PROPERTIES dp{}; dp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = rd; td.Flags = D3D12_RESOURCE_FLAG_NONE;
        ID3D12Resource *tex = nullptr;
        if (FAILED(g_device->CreateCommittedResource(&dp, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)))) return 0;

        begin_icon_batch();
        D3D12_RESOURCE_BARRIER bs{}; bs.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bs.Transition.pResource = src; bs.Transition.Subresource = 0;
        bs.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        bs.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        g_command_list->ResourceBarrier(1, &bs);
        g_command_list->CopyResource(tex, src);
        D3D12_RESOURCE_BARRIER after[2]{};
        after[0] = bs;
        after[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        after[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        after[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        after[1].Transition.pResource = tex; after[1].Transition.Subresource = 0;
        after[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        after[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g_command_list->ResourceBarrier(2, after);
        submit_and_wait();

        UINT idx = g_next_item_srv++;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_srv_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_srv_heap->GetGPUDescriptorHandleForHeapStart();
        UINT inc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        cpu.ptr += (SIZE_T)idx * inc; gpu.ptr += (UINT64)idx * inc;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Format = rd.Format; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
        g_device->CreateShaderResourceView(tex, &sd, cpu);
        outW = (int)rd.Width; outH = (int)rd.Height;
        spdlog::info("[TEXMGR] DIRECT-copied ER sheet {}x{} fmt={} -> our gpu={:#x}",
                     (int)rd.Width, (int)rd.Height, (int)rd.Format, gpu.ptr);
        return gpu.ptr;
    }

    // Copy a game icon sheet WHOLE into our own texture ONCE, cached by the source ptr (sheet-as-atlas).
    // Returns our {gpu handle, dims} or nullptr if the sheet isn't a bound TEXTURE2D yet (retry next
    // frame, not cached as a failure). The copy persists → survives ER unbinding/freeing the sheet.
    const SheetTex *copy_sheet_cached(ID3D12Resource *src)
    {
        auto it = g_sheet_cache.find(src);
        if (it != g_sheet_cache.end())
            return it->second.gpu ? &it->second : nullptr;
        if (!g_device || !g_command_queue || !g_srv_heap || g_next_item_srv >= 256)
            return nullptr;
        D3D12_RESOURCE_DESC rd = src->GetDesc();
        if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || rd.Format == DXGI_FORMAT_UNKNOWN)
            return nullptr; // not bound yet → retry (don't poison the cache)
        D3D12_HEAP_PROPERTIES dp{}; dp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = rd; td.Flags = D3D12_RESOURCE_FLAG_NONE;
        ID3D12Resource *tex = nullptr;
        if (FAILED(g_device->CreateCommittedResource(&dp, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex))))
            return nullptr;
        begin_icon_batch();
        D3D12_RESOURCE_BARRIER bs{}; bs.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bs.Transition.pResource = src; bs.Transition.Subresource = 0;
        bs.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        bs.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        g_command_list->ResourceBarrier(1, &bs);
        g_command_list->CopyResource(tex, src);
        D3D12_RESOURCE_BARRIER after[2]{};
        after[0] = bs;
        after[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        after[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        after[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        after[1].Transition.pResource = tex; after[1].Transition.Subresource = 0;
        after[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        after[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g_command_list->ResourceBarrier(2, after);
        submit_and_wait();

        UINT idx = g_next_item_srv++;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_srv_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_srv_heap->GetGPUDescriptorHandleForHeapStart();
        UINT inc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        cpu.ptr += (SIZE_T)idx * inc; gpu.ptr += (UINT64)idx * inc;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Format = rd.Format; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
        g_device->CreateShaderResourceView(tex, &sd, cpu);
        SheetTex st{gpu.ptr, (int)rd.Width, (int)rd.Height};
        g_sheet_cache[src] = st;
        spdlog::info("[TEXMGR] cached ER sheet {:#x} {}x{} -> our gpu={:#x} (atlas)",
                     reinterpret_cast<uintptr_t>(src), st.w, st.h, gpu.ptr);
        return &g_sheet_cache[src];
    }

    // Close + submit g_command_list and block until the GPU finishes — the synchronous one-shot
    // path the grace copies use (item icons batch via flush_item_icon_batch instead).
    void submit_and_wait()
    {
        g_command_list->Close();
        ID3D12CommandList *lists[] = {g_command_list};
        g_command_queue->ExecuteCommandLists(1, lists);
        ID3D12Fence *fence = nullptr;
        if (SUCCEEDED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        {
            HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            g_command_queue->Signal(fence, 1);
            if (ev && fence->GetCompletedValue() < 1)
            { fence->SetEventOnCompletion(1, ev); WaitForSingleObject(ev, 1000); }
            if (ev) CloseHandle(ev);
            fence->Release();
        }
        // The list is now Closed + drained. Drop the batch-open flag so the NEXT begin_icon_batch()
        // actually Reset()s it. Without this, a second one-shot caller (e.g. copy_sheet_cached on the
        // 2nd map sheet at map-open) sees open==true, skips Reset, and records onto a CLOSED list →
        // vkd3d device-removed hard crash, no SEH dump. (Was previously cleared only at the two manual
        // call sites; centralizing here covers every submit path. See dvdbnd/category-icon notes.)
        g_icon_batch_open = false;
    }

    // Create an inline TEXTURE2D SRV for `tex` at SRV slot `*idx` (allocated from g_next_item_srv
    // and REUSED across rebuilds when already >=0, so debug re-applies don't leak heap slots).
    // `mapping` = the channel swizzle. Returns the slot's GPU descriptor handle.
    D3D12_GPU_DESCRIPTOR_HANDLE write_inline_srv(ID3D12Resource *tex, DXGI_FORMAT fmt,
                                                 int &idx, UINT mapping)
    {
        const UINT inc = g_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if (idx < 0) idx = static_cast<int>(g_next_item_srv++);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_srv_heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(inc) * idx;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = fmt; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = mapping; sd.Texture2D.MipLevels = 1;
        g_device->CreateShaderResourceView(tex, &sd, cpu);
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_srv_heap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += static_cast<SIZE_T>(inc) * idx;
        return gpu;
    }

    // Request an item icon SRV. If cached → return its GPU handle (ok) or 0 (known miss).
    // If new → CREATE the dest texture + RECORD its copy into the frame's batch and return 0
    // (drawn next frame after flush). Never blocks; never submits per-icon.
    UINT64 ensure_item_icon_srv(int iconId)
    {
        auto it = g_item_icon_srvs.find(iconId);
        if (it != g_item_icon_srvs.end()) return it->second.ok ? it->second.gpu.ptr : 0;

        ItemIconSrv slot;  // stored even on failure → no per-frame retry
        goblin::ItemSprite sp;
        bool harvested = goblin::harvested_icon(iconId, sp);
        // One-shot per id: log WHY a request fails to enqueue (gate booleans) — distinguishes
        // "panel not reached" (no log at all) from "harvest empty" / "GPU not ready" / dims.
        static int s_miss_logged = 0;
        if (s_miss_logged < 40 &&
            !(g_device && g_command_queue && g_srv_heap && g_next_item_srv < 256 && harvested && sp.sheet))
        {
            ++s_miss_logged;
            spdlog::info("[ICONSRV] iconId={} no-enqueue: dev={} q={} heap={} slot<256={} harvested={} sheet={:#x}",
                         iconId, (void *)g_device != nullptr, (void *)g_command_queue != nullptr,
                         (void *)g_srv_heap != nullptr, g_next_item_srv < 256, harvested,
                         reinterpret_cast<uintptr_t>(sp.sheet));
        }
        if (g_device && g_command_queue && g_srv_heap && g_next_item_srv < 256 &&
            harvested && sp.sheet)
        {
            auto *src_res = reinterpret_cast<ID3D12Resource *>(sp.sheet);
            // Authoritative format from D3D12 (render thread, sheet bound) — NOT the RPM-read
            // sp.format (0 pre-bind, findings §2). If the sheet isn't a ready TEXTURE2D yet, return
            // WITHOUT storing a permanent miss so a later frame retries.
            D3D12_RESOURCE_DESC rd = src_res->GetDesc();
            DXGI_FORMAT fmt = rd.Format;
            if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || fmt == DXGI_FORMAT_UNKNOWN)
                return 0;   // sheet not bound yet → retry next frame (no permanent-miss store)
            // BC formats (BC1/BC7) copy on 4x4 BLOCKS. Most item cells are 270x270 / 54x54 — NOT
            // multiples of 4 — so a raw sub-rect copy was rejected (the "131 harvested, 3 drawn" bug:
            // only the 160x160 cells passed). record_sprite_copy snaps the rect OUT to 4-aligned
            // bounds + stores UVs so ImGui crops back to the exact icon. Unlike the grace paths this
            // one BATCHES (no per-icon submit) — the SRV descriptor is written later in flush.
            begin_icon_batch();
            SrvCopy cp;
            if (record_sprite_copy(src_res, fmt, sp, cp))
            {
                slot.tex = cp.tex; slot.uv0 = cp.uv0; slot.uv1 = cp.uv1;
                // Reserve the SRV slot now; the descriptor itself is written at flush (after the
                // copy completes). Park the slot as not-ok so it isn't re-enqueued; flush flips it.
                UINT idx = g_next_item_srv++;
                g_pending_icons.push_back({iconId, cp.tex, fmt, idx, cp.w, cp.h});
                g_item_icon_srvs[iconId] = slot;  // ok=false, tex set → pending
                return 0;                          // drawn next frame, after flush
            }
        }
        if (!slot.ok && slot.tex) { slot.tex->Release(); slot.tex = nullptr; }
        g_item_icon_srvs[iconId] = slot;   // known miss → never retried
        return 0;
    }

    // Finalize this frame's icon copies: one ExecuteCommandLists + one fence wait for the whole
    // batch, then create each pending SRV and mark its slot ok. Call once per frame AFTER all
    // ensure_item_icon_srv() requests and BEFORE g_command_list is reused for the ImGui render.
    void flush_item_icon_batch()
    {
        if (!g_icon_batch_open) return;
        submit_and_wait();   // one ExecuteCommandLists + one fence wait for the whole batch

        const UINT inc = g_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (const PendingIcon &p : g_pending_icons)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_srv_heap->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += static_cast<SIZE_T>(inc) * p.srv_idx;
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = p.fmt; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            g_device->CreateShaderResourceView(p.tex, &sd, cpu);
            ItemIconSrv &s = g_item_icon_srvs[p.iconId];
            s.gpu = g_srv_heap->GetGPUDescriptorHandleForHeapStart();
            s.gpu.ptr += static_cast<SIZE_T>(inc) * p.srv_idx;
            s.ok = true;
            spdlog::info("[ICONSRV] iconId={} {}x{} fmt={} -> slot {} gpu={:#x} (batched)",
                         p.iconId, p.w, p.h, static_cast<int>(p.fmt), p.srv_idx, s.gpu.ptr);
        }
        g_pending_icons.clear();
        g_icon_batch_open = false;
    }

    // ── Grace sprite SRV (RE e4b3f6a §6, step 2) ──────────────────────────────────────
    // Copy the harvested discovered-grace rect (SB_ERR_Grace_Morning_Color) from the engine sheet
    // into our own texture, make an SRV, and expose tex+UV to the map renderer so the overlay draws
    // graces itself (discovered = full colour, undiscovered = grey-tinted). The rect (74x74) isn't
    // 4-aligned but the sheet is BC7 (block 4x4) → snap the copy box to 4-aligned bounds and inset
    // the UV onto the exact grace within the slightly-larger copied texture. One-shot synchronous
    // copy (runs once when the grace sprite is first harvested); retries until then.
    ID3D12Resource *g_grace_tex = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE g_grace_gpu{};
    ImVec2 g_grace_uv0{}, g_grace_uv1{};
    // Native pixel rect dims (sp.x1-sp.x0, sp.y1-sp.y0), before UV normalization -- dx-bugs
    // 2026-07-01 auto-scale-ratio followup.
    int g_grace_native_w = 0, g_grace_native_h = 0;
    int g_grace_state = 0;   // 0 = not ready (retry), 1 = ok, 2 = failed (give up)
    int g_grace_srv_idx = -1;   // reuse one SRV slot across rebuilds (no slot leak on re-apply)
    // ── Grace texture DEBUG (F1 panel) — live format/swizzle/source override ────────────────────────
    int g_grace_dbg_srgb = 0;     // 0 = auto (GetDesc) | 1 = force BC7_UNORM (linear) | 2 = force BC7_UNORM_SRGB
    int g_grace_dbg_swiz = 0;     // SRV component mapping: 0 default RGBA | 1 R<->B | 2 R<->G | 3 force A=1
    int g_grace_dbg_fmt_used = 0; // last format actually used (for the panel readout)

    // SRV component mapping for the debug swizzle test (diagnoses channel-order / green-red issues).
    UINT grace_dbg_mapping()
    {
        switch (g_grace_dbg_swiz)
        {
        case 1: return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(2, 1, 0, 3);  // R<->B
        case 2: return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(1, 0, 2, 3);  // R<->G
        case 3: return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 1, 2,
                       D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);          // force opaque alpha
        default: return D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        }
    }

    bool ensure_grace_srv()
    {
        if (g_grace_state == 1) return true;
        if (g_grace_state == 2) return false;
        goblin::ItemSprite sp;
        if (!(g_device && g_command_queue && g_srv_heap && g_next_item_srv < 256 &&
              goblin::harvested_grace(sp) && sp.sheet))
            return false;   // not harvested yet → retry next frame (do NOT mark failed)

        auto *src_res = reinterpret_cast<ID3D12Resource *>(sp.sheet);
        // Authoritative format/dims from D3D12 (render thread, sheet bound) — NOT the RPM-read
        // sp.format (0 pre-bind, and the RPM dim/format offsets drifted; see dump_icon_textures_live).
        // Mirrors ensure_item_icon_srv. If not a ready TEXTURE2D yet, retry next frame (no permanent fail).
        D3D12_RESOURCE_DESC rd = src_res->GetDesc();
        DXGI_FORMAT fmt = rd.Format;
        if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || fmt == DXGI_FORMAT_UNKNOWN)
            return false;
        // DEBUG sRGB override (BC7 only — UNORM/SRGB share a copy group, so the GPU→GPU copy stays valid).
        if (fmt == DXGI_FORMAT_BC7_UNORM || fmt == DXGI_FORMAT_BC7_UNORM_SRGB)
        {
            if (g_grace_dbg_srgb == 1) fmt = DXGI_FORMAT_BC7_UNORM;
            else if (g_grace_dbg_srgb == 2) fmt = DXGI_FORMAT_BC7_UNORM_SRGB;
        }
        g_grace_dbg_fmt_used = static_cast<int>(fmt);

        // Open the shared list, record the snap+copy, submit synchronously, then write the SRV
        // (with the debug swizzle) into the REUSED grace slot. See record_sprite_copy / submit_and_wait
        // / write_inline_srv.
        g_frames[0].allocator->Reset();
        g_command_list->Reset(g_frames[0].allocator, nullptr);
        SrvCopy cp;
        if (!record_sprite_copy(src_res, fmt, sp, cp)) { g_grace_state = 2; return false; }
        g_grace_tex = cp.tex;
        submit_and_wait();

        g_grace_gpu = write_inline_srv(g_grace_tex, fmt, g_grace_srv_idx, grace_dbg_mapping());
        g_grace_uv0 = cp.uv0; g_grace_uv1 = cp.uv1;
        g_grace_native_w = sp.x1 - sp.x0; g_grace_native_h = sp.y1 - sp.y0;
        g_grace_state = 1;
        spdlog::info("[GRACE-SRV] copied {}x{} (snapped from {},{}-{},{}) fmt={} -> slot {} gpu={:#x}",
                     cp.w, cp.h, sp.x0, sp.y0, sp.x1, sp.y1, static_cast<int>(fmt), g_grace_srv_idx,
                     g_grace_gpu.ptr);
        return true;
    }

    // Re-run the grace copy + SRV next frame (with the current debug format/swizzle) — for the F1
    // Grace-debug panel's "Re-apply" so format/swizzle/source changes take effect live.
    void force_rebuild_grace()
    {
        if (g_grace_tex) { g_grace_tex->Release(); g_grace_tex = nullptr; }
        g_grace_state = 0;   // ensure_grace_srv re-copies + rewrites the (reused) SRV slot next frame
    }

    // ── ERR dungeon grace SRV (2nd grace sprite) ─────────────────────────────────────────────────
    // Mirror of ensure_grace_srv for goblin::harvested_grace_dungeon (MENU_MAP_ERR_GraceUnderground).
    // No debug overrides; GetDesc at copy time. Valid only when ERR is installed (sprite captured).
    ID3D12Resource *g_grace_dgn_tex = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE g_grace_dgn_gpu{};
    ImVec2 g_grace_dgn_uv0{}, g_grace_dgn_uv1{};
    int g_grace_dgn_state = 0;       // 0 retry | 1 ok | 2 failed
    int g_grace_dgn_srv_idx = -1;

    bool ensure_grace_dungeon_srv()
    {
        if (g_grace_dgn_state == 1) return true;
        if (g_grace_dgn_state == 2) return false;
        goblin::ItemSprite sp;
        if (!(g_device && g_command_queue && g_srv_heap && g_next_item_srv < 256 &&
              goblin::harvested_grace_dungeon(sp) && sp.sheet))
            return false;
        auto *src_res = reinterpret_cast<ID3D12Resource *>(sp.sheet);
        D3D12_RESOURCE_DESC rd = src_res->GetDesc();
        DXGI_FORMAT fmt = rd.Format;
        if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || fmt == DXGI_FORMAT_UNKNOWN) return false;

        g_frames[0].allocator->Reset();
        g_command_list->Reset(g_frames[0].allocator, nullptr);
        SrvCopy cp;
        if (!record_sprite_copy(src_res, fmt, sp, cp)) { g_grace_dgn_state = 2; return false; }
        g_grace_dgn_tex = cp.tex;
        submit_and_wait();

        g_grace_dgn_gpu = write_inline_srv(g_grace_dgn_tex, fmt, g_grace_dgn_srv_idx,
                                           D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
        g_grace_dgn_uv0 = cp.uv0; g_grace_dgn_uv1 = cp.uv1;
        g_grace_dgn_state = 1;
        spdlog::info("[GRACE-SRV] DUNGEON copied {}x{} fmt={} -> slot {}", cp.w, cp.h, (int)fmt,
                     g_grace_dgn_srv_idx);
        return true;
    }

    // ── Generalized map-symbol SRV (boss & other MENU_MAP_* pins) ────────────────────────────────
    // The EXACT grace path (record_sprite_copy of the sub-rect + write_inline_srv), per-name cached
    // slot, retry-until-ready. Replaces the whole-sheet copy_sheet_cached path for named symbols
    // (which drew the boss invisible) so map symbols use the proven, grace-identical pipeline.
    struct MapSymSrv { int state = 0; int srv_idx = -1; ID3D12Resource *tex = nullptr;
                       D3D12_GPU_DESCRIPTOR_HANDLE gpu{}; ImVec2 uv0{}, uv1{}; };
    std::map<std::string, MapSymSrv> g_map_sym_srv;

    bool ensure_map_sym_srv(const char *name, MapSymSrv *&out)
    {
        MapSymSrv &s = g_map_sym_srv[name];
        out = &s;
        if (s.state == 1) return true;
        int x = 0, y = 0, w = 0, h = 0; void *sheet = nullptr;
        if (!goblin::map_icon_rect_by_name(name, x, y, w, h, sheet) || !sheet || w <= 0 || h <= 0)
            return false;
        if (!(g_device && g_command_queue && g_srv_heap && g_next_item_srv < 256))
            return false;
        auto *src = reinterpret_cast<ID3D12Resource *>(sheet);
        D3D12_RESOURCE_DESC rd = src->GetDesc();
        DXGI_FORMAT fmt = rd.Format;
        if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || fmt == DXGI_FORMAT_UNKNOWN)
            return false;                                  // not bound yet → retry next frame
        goblin::ItemSprite sp{}; sp.sheet = sheet; sp.x0 = x; sp.y0 = y; sp.x1 = x + w; sp.y1 = y + h;
        g_frames[0].allocator->Reset();
        g_command_list->Reset(g_frames[0].allocator, nullptr);
        SrvCopy cp;
        if (!record_sprite_copy(src, fmt, sp, cp)) return false;  // retry (do NOT mark failed)
        s.tex = cp.tex;
        submit_and_wait();
        s.gpu = write_inline_srv(s.tex, fmt, s.srv_idx, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
        s.uv0 = cp.uv0; s.uv1 = cp.uv1; s.state = 1;
        spdlog::info("[MAPSYM-SRV] '{}' copied {}x{} fmt={} -> slot {} gpu={:#x}", name, cp.w, cp.h,
                     (int)fmt, s.srv_idx, s.gpu.ptr);
        return true;
    }

    // ── No-bake item-icon sheet from disk (GAP #2 WIRE) ──────────────────────────────
    // When an item icon's atlas sheet isn't resident in-game (harvested_icon misses — the common
    // case for non-equipment icons), draw it from the sheet DDS we read out of the menu texture pack
    // ourselves (worldmap::read_item_icon_sheets → menu/hi/01_common.tpf, via the loose mod overlay
    // or the packed dvdbnd). The heavy read+decompress (~194 MB TPF) runs on a one-shot background
    // thread; the GPU upload happens on the render thread (dedicated reset, like ensure_map_sym_srv).
    // Cached per sheet name. See [[category-icons-00solo-atlas]] GAP #2 + [[dvdbnd-packed-reader]].
    struct DiskSheet { int state = 0; UINT64 gpu = 0; int w = 0, h = 0; };  // 0 pending,1 ready,2 failed
    std::map<std::string, DiskSheet>            g_disk_sheets;     // sheet name (no ext) -> uploaded SRV
    std::map<std::string, std::vector<uint8_t>> g_disk_sheet_dds;  // bg-loaded DDS awaiting GPU upload
    std::unordered_set<std::string>             g_disk_sheet_want; // names the renderer has asked for
    std::mutex                                  g_disk_sheet_mtx;
    std::atomic<bool>                           g_disk_sheet_loading{false};

    // Kick the one-shot background loader (no-op if one is already running — it re-scans the want set
    // before exiting): reads every wanted-but-unloaded sheet from the texture pack ONCE and parks the
    // DDS for the render thread to upload. The 194 MB decompress must never run on the render thread.
    void request_disk_sheets()
    {
        if (g_disk_sheet_loading.exchange(true)) return;
        std::thread([] {
            for (;;)
            {
                std::vector<std::string> names;
                {
                    std::lock_guard<std::mutex> lk(g_disk_sheet_mtx);
                    for (const std::string &n : g_disk_sheet_want)
                        if (g_disk_sheet_dds.find(n) == g_disk_sheet_dds.end() &&
                            g_disk_sheets[n].state == 0)
                            names.push_back(n);
                }
                if (names.empty()) break;
                auto got = goblin::worldmap::read_item_icon_sheets(names);  // heavy (read + decompress)
                {
                    std::lock_guard<std::mutex> lk(g_disk_sheet_mtx);
                    for (auto &kv : got)
                        g_disk_sheet_dds[kv.first] = std::move(kv.second);
                    for (const std::string &n : names)  // absent from the pack → don't retry forever
                        if (got.find(n) == got.end())
                            g_disk_sheets[n].state = 2;
                }
            }
            g_disk_sheet_loading.store(false);
        }).detach();
    }

    // Render-thread: ensure `name`'s sheet is GPU-uploaded; on success copy it into `out` (true).
    // Returns false while the DDS is still being read (registers the want + kicks the loader) or if
    // the sheet can't be sourced — the caller keeps the baked atlas meanwhile.
    bool ensure_disk_sheet(const std::string &name, DiskSheet &out)
    {
        std::lock_guard<std::mutex> lk(g_disk_sheet_mtx);
        DiskSheet &ds = g_disk_sheets[name];
        if (ds.state == 1) { out = ds; return true; }
        if (ds.state == 2) return false;
        auto it = g_disk_sheet_dds.find(name);
        if (it != g_disk_sheet_dds.end())
        {
            int sw = 0, sh = 0; DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
            UINT64 gpu = create_tex_from_dds_mem(it->second.data(), it->second.size(), sw, sh, f);
            g_disk_sheet_dds.erase(it);  // free the ~8 MB CPU DDS either way
            if (gpu && sw > 0 && sh > 0)
            { ds.gpu = gpu; ds.w = sw; ds.h = sh; ds.state = 1; out = ds; return true; }
            ds.state = 2;  // upload failed → don't retry
            return false;
        }
        g_disk_sheet_want.insert(name);  // not read yet → request + baked fallback this frame
        request_disk_sheets();
        return false;
    }

    // ── Grace-sprite GPU debug viewer (dev) ──────────────────────────────────────────
    // Draws EVERY harvested SB_ERR_Grace_* frame as a GPU image so we can visually pick the correct
    // grace rect (the captured 'Morning_Color' frame didn't look like a grace). Uses a FULL-SHEET SRV
    // over the engine resource + per-frame UV (NO copy → shows the exact rect content, isolates a
    // wrong-rect bug from a copy/UV bug). Engine-owned sheet (don't Release); dev/map-open only.
    std::vector<GraceDbg> g_grace_dbg;

    void ensure_grace_debug()
    {
        if (!g_device || !g_srv_heap) return;
        std::vector<goblin::GraceCandidate> cands = goblin::grace_candidates();
        // Build one SRV per candidate that has a bound sheet. Candidates are now LISTED before their
        // GPU sheet binds (vkd3d/Proton binds lazily — a candidate can appear unbound, then resolve a
        // few frames later), so we can't use a high-water mark: it would skip an entry permanently once
        // it binds. g_grace_dbg holds one entry per built NAME, so once every candidate is built the
        // sizes match and we early-out; until then we re-scan (cands is tiny, ≤64).
        if (g_grace_dbg.size() >= cands.size()) return;
        const UINT inc = g_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (size_t i = 0; i < cands.size(); ++i)
        {
            const goblin::GraceCandidate &c = cands[i];
            if (!c.spr.sheet || c.spr.sheetW == 0 || c.spr.sheetH == 0)
                continue;   // listed but not bound yet — its SRV gets built once the texture resolves
            bool already = false;
            for (const GraceDbg &d : g_grace_dbg) if (d.name == c.name) { already = true; break; }
            if (already || g_next_item_srv >= 256)
                continue;
            UINT idx = g_next_item_srv++;
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_srv_heap->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += static_cast<SIZE_T>(inc) * idx;
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = static_cast<DXGI_FORMAT>(c.spr.format);
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            g_device->CreateShaderResourceView(reinterpret_cast<ID3D12Resource *>(c.spr.sheet), &sd, cpu);
            D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_srv_heap->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += static_cast<SIZE_T>(inc) * idx;
            float W = static_cast<float>(c.spr.sheetW), H = static_cast<float>(c.spr.sheetH);
            GraceDbg d;
            d.tex = reinterpret_cast<ImTextureID>(gpu.ptr);
            d.uv0 = ImVec2(c.spr.x0 / W, c.spr.y0 / H);
            d.uv1 = ImVec2(c.spr.x1 / W, c.spr.y1 / H);
            d.name = c.name; d.w = c.spr.x1 - c.spr.x0; d.h = c.spr.y1 - c.spr.y0;
            g_grace_dbg.push_back(d);
        }
    }



    // Upload the atlas once g_command_queue is captured. Self-gates; on failure it
    // marks ready anyway so we don't retry every frame (just falls back to circles).
    void try_upload_atlas()
    {
        if (g_atlas_ready || !g_imgui_init || !g_command_queue || !g_device || !g_srv_heap)
            return;
        GOBLIN_BENCH("overlay.init.atlas");
        using namespace goblin::overlay_icons;
        // The atlas ships as a compressed PNG blob (small source); decode it to RGBA once.
        int w = 0, h = 0, ch = 0;
        unsigned char *rgba =
            stbi_load_from_memory(ATLAS_PNG, ATLAS_PNG_LEN, &w, &h, &ch, 4);
        if (rgba && w == ATLAS_W && h == ATLAS_H &&
            upload_rgba(rgba, w, h, 1, &g_atlas_tex, &g_atlas_gpu))
            spdlog::info("[OVERLAY] icon atlas {}x{} decoded + uploaded (SRV 1)", w, h);
        else
            spdlog::warn("[OVERLAY] icon atlas decode/upload failed → circle fallback");
        if (rgba)
            stbi_image_free(rgba);
        g_atlas_ready = true;
    }

    // Cursor hooks (hk_set_cursor_pos/hk_clip_cursor/hk_get_cursor_pos) moved to
    // src/input/input_cursor.cpp — third slice of docs/plans/input_module_refactor_plan.md.

    // Raw input hooks (hk_get_raw_input_data/hk_get_raw_input_buffer) moved to
    // src/input/input_rawinput.cpp — fourth slice of docs/plans/input_module_refactor_plan.md.

    // WndProc hook (hk_wndproc) moved to src/input/input_wndproc.cpp — fifth and last slice
    // of docs/plans/input_module_refactor_plan.md.

    // ── First-frame ImGui init against the live swapchain ─────────────────
    bool init_imgui(IDXGISwapChain3 *swapchain)
    {
        GOBLIN_BENCH("overlay.init.imgui");
        if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&g_device))))
        {
            spdlog::error("[OVERLAY] swapchain->GetDevice failed");
            return false;
        }

        DXGI_SWAP_CHAIN_DESC desc{};
        swapchain->GetDesc(&desc);
        g_buffer_count = desc.BufferCount;
        g_hwnd = desc.OutputWindow;

        // RTV heap: one per back buffer.
        D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
        rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv_desc.NumDescriptors = g_buffer_count;
        rtv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(g_device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&g_rtv_heap))))
        {
            spdlog::error("[OVERLAY] CreateDescriptorHeap(RTV) failed");
            return false;
        }
        UINT rtv_size = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_cpu = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();

        // SRV heap (shader-visible): [0] ImGui font, [1] the category-icon atlas, [2..] per-item
        // icons copied live from the engine's menu sheets (see ensure_item_icon_srv).
        D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
        srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv_desc.NumDescriptors = 256;
        srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_device->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&g_srv_heap))))
        {
            spdlog::error("[OVERLAY] CreateDescriptorHeap(SRV) failed");
            return false;
        }

        // Per-frame: command allocator + back-buffer RTV.
        g_frames.resize(g_buffer_count);
        for (UINT i = 0; i < g_buffer_count; i++)
        {
            if (FAILED(g_device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_frames[i].allocator))))
            {
                spdlog::error("[OVERLAY] CreateCommandAllocator failed");
                return false;
            }
            g_frames[i].rtv_handle = rtv_cpu;
            rtv_cpu.ptr += rtv_size;

            ID3D12Resource *back = nullptr;
            swapchain->GetBuffer(i, IID_PPV_ARGS(&back));
            g_device->CreateRenderTargetView(back, nullptr, g_frames[i].rtv_handle);
            g_frames[i].render_target = back;
        }

        if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               g_frames[0].allocator, nullptr,
                                               IID_PPV_ARGS(&g_command_list))))
        {
            spdlog::error("[OVERLAY] CreateCommandList failed");
            return false;
        }
        g_command_list->Close();

        // ImGui context + backends.
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;   // no imgui.ini on disk
        // Widget navigation via gamepad (dx-bugs-backlog PR C-2). The vendored Win32 backend
        // already polls XInput and feeds ImGuiKey_Gamepad* every frame (ImGui_ImplWin32_
        // UpdateGamepads, called unconditionally from NewFrame) — this flag is the only line
        // needed to turn that into actual focus/highlight navigation. See the XInputGetState
        // hook below for why the game doesn't ALSO react to the same stick/button input.
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

        // Fonts: ImGui's default (ProggyClean) only has Latin-1 glyphs (0x20-0xFF), so any
        // char beyond — œ (U+0153) in French item names, em-dash, Cyrillic, Greek — renders
        // as the fallback '?'. Keep the default for the ASCII look, then MERGE an EMBEDDED
        // broad-Latin font (DejaVu Sans, compressed into the DLL via dejavu_sans_ttf.h) for the
        // EXTENDED glyphs only. Embedded — NOT C:\Windows\Fonts — so it renders identically under
        // Wine/Proton/Linux where the Windows font paths differ or are absent. FMG labels are
        // already UTF-8 via lookup_text_utf8. CJK/Thai/Arabic need a far bigger atlas + a CJK
        // font — out of scope here (European coverage only). A system-font merge stays as a
        // last-ditch fallback if the embedded decompress ever fails.
        {
            ImGuiIO &io = ImGui::GetIO();
            io.Fonts->AddFontDefault();  // ProggyClean (preserves the existing ASCII look)
            static const ImWchar kExtRanges[] = {
                0x00A0, 0x00FF,  // Latin-1 Supplement (accents) — merge-safe overlap
                0x0100, 0x024F,  // Latin Extended-A + B (œ ligature, more accents)
                0x0370, 0x03FF,  // Greek
                0x0400, 0x04FF,  // Cyrillic
                0x2010, 0x2027,  // general punctuation (en/em dash, curly quotes, ellipsis)
                0x2190, 0x21FF,  // arrows (→ used in overlay labels e.g. "world→map fn")
                0x2200, 0x22FF,  // math operators (≤ ≥ ≠ × ÷ used in labels)
                0x20A0, 0x20BF,  // currency symbols
                0,
            };
            ImFontConfig cfg;
            cfg.MergeMode = true;            // graft these glyphs onto the default font
            cfg.PixelSnapH = true;
            // DejaVu Sans (Bitstream Vera / Arev license — freely redistributable, embeddable).
            // AddFontFromMemoryCompressedTTF decompresses into an atlas-owned buffer; the static
            // compressed array is only read, never retained.
            if (io.Fonts->AddFontFromMemoryCompressedTTF(
                    DejaVuSans_compressed_data, DejaVuSans_compressed_size, 13.0f, &cfg, kExtRanges))
            {
                spdlog::info("[FONT] merged extended Unicode glyphs from embedded DejaVu Sans");
            }
            else
            {
                // Only reachable if the embedded decompress failed — fall back to a system font.
                const char *kFontCandidates[] = {
                    "C:\\Windows\\Fonts\\segoeui.ttf",
                    "C:\\Windows\\Fonts\\arial.ttf",
                    "C:\\Windows\\Fonts\\tahoma.ttf",
                };
                bool merged = false;
                for (const char *p : kFontCandidates)
                {
                    if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) continue;
                    if (io.Fonts->AddFontFromFileTTF(p, 13.0f, &cfg, kExtRanges)) { merged = true;
                        spdlog::info("[FONT] embedded font failed; merged from {}", p); break; }
                }
                if (!merged)
                    spdlog::warn("[FONT] embedded + system font merge failed — non-Latin-1 chars will show '?'");
            }
        }
        // Software cursor: the game hides the OS cursor, so ImGui draws its own — but ONLY while the
        // F1 panel is up. Set per-frame to g_show in the render loop (NewFrame now runs every frame
        // for the overlay markers, so this can't be a one-time true). Default off.
        ImGui::GetIO().MouseDrawCursor = false;
        ImGui::StyleColorsDark();
        ImGui_ImplWin32_Init(g_hwnd);
        ImGui_ImplDX12_Init(g_device, g_buffer_count, DXGI_FORMAT_R8G8B8A8_UNORM, g_srv_heap,
                            g_srv_heap->GetCPUDescriptorHandleForHeapStart(),
                            g_srv_heap->GetGPUDescriptorHandleForHeapStart());

        // Install the input hook now that we have the window.
        goblin::input::install_wndproc_hook(g_hwnd);

        spdlog::info("[OVERLAY] ImGui initialised: {} back buffers, hwnd={:p}",
                     g_buffer_count, static_cast<void *>(g_hwnd));
        return true;
    }


    // ── Present hook (renders the overlay) ────────────────────────────────
    HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain3 *swapchain, UINT sync, UINT flags)
    {
        // Real frame time = wall delta between consecutive Present calls (the WHOLE frame: game
        // engine + our overlay + GPU present). Lets the [BENCH] report answer "are we even the
        // bottleneck": compare present.frame_wall (≈33ms at 30fps) against present.overlay_total
        // (our share) — if overlay_total ≪ frame_wall, the engine/GPU is the cap, not our markers.
        // Skip the first sample and absurd gaps (alt-tab / pause / loading) so the avg isn't polluted.
        {
            using clk = std::chrono::steady_clock;
            static auto s_last = clk::now();
            const auto now = clk::now();
            const double ms = std::chrono::duration<double, std::milli>(now - s_last).count();
            s_last = now;
            if (ms > 0.0 && ms < 1000.0)
                goblin::bench::Registry::instance().record("present.frame_wall", ms);
        }

        if (!g_imgui_init)
        {
            if (!init_imgui(swapchain))
            {
                g_failed = true;
                cleanup_imgui_device();
                spdlog::error("[OVERLAY] init failed → overlay disabled, mod continues");
                return o_present(swapchain, sync, flags);
            }
            g_imgui_init = true;
        }

        // Configurable open/close key (default F1; INI overlay_toggle_key). Works
        // anywhere, incl. over the 2D map. Edge-detected. (The CSMenuMan+0xCD
        // auto-show signal proved dead on this build — reads 0 even with the map
        // open — so this key is the sole trigger; the intended keybind-driven UX.)
        // GetAsyncKeyState is GLOBAL (focus-independent): without this guard, pressing F1 while the
        // game is alt-tabbed / on another monitor would still toggle the overlay AND activate the
        // cursor hooks (free/freeze the cursor, blank input) off-screen — bug report "F1 offscreen
        // active le hook cursor". Gate BOTH the toggle and the active state on the game window being
        // foreground: the hooks only ever run while you're actually in the game.
        // Message-driven (g_has_focus, set from WM_SETFOCUS/WM_KILLFOCUS in hk_wndproc), NOT a
        // per-frame GetForegroundWindow() poll — see g_has_focus's declaration comment for why
        // the poll was flapping during Alt+Tab under Wine and breaking input on refocus.
        const bool fg = g_hwnd && g_has_focus.load(std::memory_order_relaxed);
        bool down = fg && (GetAsyncKeyState(static_cast<int>(goblin::config::overlayToggleKey)) & 0x8000) != 0;
        if (down)
        {
            if (++g_toggle_kb_streak >= kToggleKbDebounceFrames && g_toggle_kb_armed)
            {
                g_user_show = !g_user_show;
                g_toggle_kb_armed = false;
                spdlog::info("[TOGGLEDIAG] KEYBOARD toggle fired, g_user_show now {}", g_user_show);
            }
        }
        else
        {
            g_toggle_kb_streak = 0;
            g_toggle_kb_armed = true;
        }

        // Gamepad support (dx-bugs-backlog PR C, items 2/3/6): combo toggle + cursor recenter.
        // XInput has no window messages, so — like the keyboard key above — it must be polled
        // here every frame rather than driven off hk_wndproc. Loaded + hooked once via
        // goblin::input::install_xinput_hook() (src/input/input_gamepad.cpp); always read via
        // xinput_get_state_real() (the trampoline) here so our own poll sees real data even
        // while F1 is open (the hook only swallows callers OUTSIDE our module).

        // Shared cursor-recenter (items 2 + 6): center of the game window client rect, via the
        // hooked SetCursorPos so it round-trips the same path as everything else touching the cursor.
        auto recenter_cursor_to_window = [&]()
        {
            if (!g_hwnd) return;
            RECT rc;
            if (!GetClientRect(g_hwnd, &rc)) return;
            POINT c{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
            ClientToScreen(g_hwnd, &c);
            // SetCursorPos generates a real WM_MOUSEMOVE — without this guard, hk_wndproc's
            // "real mouse move clears g_last_input_was_gamepad" logic would clear the flag WE
            // just set, immediately re-arming the "just switched to gamepad" edge on the very
            // next frame if the pad is still active at all (e.g. a hand resting on the stick) —
            // an infinite recenter-every-frame loop that pins the cursor and locks out the mouse
            // entirely. Consumed once in hk_wndproc, not just skipped here.
            g_ignore_next_mousemove_for_gamepad_flag = true;
            goblin::input::set_cursor_pos_real(c.x, c.y);
        };

        if (goblin::input::xinput_available())
        {
            WORD combined = 0;
            bool active = false;
            for (DWORD i = 0; i < 4; ++i)
            {
                XINPUT_STATE state{};
                if (goblin::input::xinput_get_state_real(i, &state) != ERROR_SUCCESS) continue;
                const auto &pad = state.Gamepad;
                combined |= pad.wButtons;
                if (pad.wButtons != 0
                    || std::abs(pad.sThumbLX) > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE
                    || std::abs(pad.sThumbLY) > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE
                    || std::abs(pad.sThumbRX) > XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE
                    || std::abs(pad.sThumbRY) > XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE
                    || pad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD
                    || pad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
                    active = true;
            }

            // Combo toggle — same edge-detected, foreground-gated path the keyboard key drives.
            // Skipped while recording a new combo: otherwise pressing the CURRENT combo (to record
            // its replacement) also flips g_user_show and closes the very panel you're recording in.
            if (goblin::config::overlayToggleGamepad != 0 && !g_gamepad_combo_recording)
            {
                const bool combo_down = fg && (combined & goblin::config::overlayToggleGamepad) == goblin::config::overlayToggleGamepad;
                // Debounced: only commit the toggle once `combo_down` has been true for
                // kToggleGamepadDebounceFrames CONSECUTIVE frames (rejects a single-frame/short
                // glitch burst — see g_toggle_gamepad_streak's declaration comment), and only
                // once per press (`g_toggle_gamepad_armed`, re-armed on release) so holding the
                // combo past the debounce window doesn't fire it again every subsequent frame.
                if (combo_down)
                {
                    if (++g_toggle_gamepad_streak >= kToggleGamepadDebounceFrames && g_toggle_gamepad_armed)
                    {
                        g_user_show = !g_user_show;
                        g_toggle_gamepad_armed = false;
                        spdlog::info("[TOGGLEDIAG] GAMEPAD toggle fired, g_user_show now {}", g_user_show);
                    }
                }
                else
                {
                    g_toggle_gamepad_streak = 0;
                    g_toggle_gamepad_armed = true;
                }
            }

            // Gamepad combo recorder: settings button arms this. Buttons pressed one after
            // another while held (e.g. Y then R3 while still holding Y) all count — capture the
            // UNION of everything held during the press, and finalize on release (not on the
            // first single button), so a multi-button combo has time to actually form.
            // Gate: with gamepad nav (PR C-2), the SAME button press that activates the
            // "Record gamepad combo" button via ImGui nav (A) is still physically held the
            // instant we arm — without waiting for a full release first, that click was itself
            // getting captured as the combo ("A"), before the user could press anything else.
            static WORD s_record_union = 0;
            if (g_gamepad_combo_recording)
            {
                if (!g_gamepad_combo_ready)
                {
                    if (combined == 0) g_gamepad_combo_ready = true;  // activating press fully released
                }
                else if (combined != 0)
                    s_record_union |= combined;
                else if (s_record_union != 0)
                {
                    // Reject a combo that's a SINGLE ImGui-nav-reserved button (A/B/X/Y or a
                    // D-pad direction): those alone drive normal widget interaction/navigation
                    // (A = Activate, etc.), so using just one of them as the toggle means every
                    // ordinary button click ALSO closes F1 — precisely the "pressed A to click
                    // Record, closed the menu instead" bug. Multi-button combos are fine even if
                    // they include one of these; only a lone nav button is rejected.
                    constexpr WORD NAV_RESERVED = 0x1000 /*A*/ | 0x2000 /*B*/ | 0x4000 /*X*/ | 0x8000 /*Y*/
                                                  | 0x0001 /*Up*/ | 0x0002 /*Down*/ | 0x0004 /*Left*/ | 0x0008 /*Right*/;
                    const bool is_single_button = (s_record_union & (s_record_union - 1)) == 0;
                    if (is_single_button && (s_record_union & NAV_RESERVED))
                    {
                        g_gamepad_combo_recording = false;
                        g_gamepad_combo_reject_reason = "Can't use " + goblin::mask_to_combo_string(s_record_union)
                                                         + " alone — it's needed for menu navigation. "
                                                           "Add a second button (e.g. +LB/RB/L3/R3/Back/Start).";
                        spdlog::warn("[OVERLAY] Gamepad combo rejected (single nav button): {}",
                                     goblin::mask_to_combo_string(s_record_union));
                    }
                    else
                    {
                        goblin::config::overlayToggleGamepad = s_record_union;
                        g_gamepad_combo_recording = false;
                        goblin::save_all_bool_settings(goblin::config_ini_path());
                        spdlog::info("[OVERLAY] Gamepad combo recorded: {}", goblin::mask_to_combo_string(s_record_union));
                    }
                }
            }
            else
            {
                s_record_union = 0;          // reset so a freshly-armed recording starts clean
                g_gamepad_combo_ready = false;
            }

            // Item 2: recenter the cursor once input has switched from mouse/kb to pad — ER
            // itself doesn't recenter, so going pad-only otherwise leaves the cursor wherever the
            // mouse last was. `fg`-gated: background pad noise (alt-tabbed away) must never
            // drive this or the flag below (dx-bugs 2026-07-01 "alt-tab back" followup).
            // Debounced (dx-bugs 2026-07-01 "search panel unusable" followup): only treat it as
            // a real switch after kGamepadSwitchDebounceFrames CONSECUTIVE active frames, so a
            // single-frame blip (stick drift, idle hand on the pad) can't immediately re-arm the
            // edge right after a genuine mouse move cleared it — that fight was pinning the
            // cursor to window centre on almost every mouse interaction.
            if (active && fg)
            {
                if (++g_gamepad_active_streak >= kGamepadSwitchDebounceFrames && !g_last_input_was_gamepad)
                {
                    recenter_cursor_to_window();
                    g_last_input_was_gamepad = true;
                }
            }
            else
            {
                g_gamepad_active_streak = 0;
            }
        }

        {
            // [FOCUSDIAG] log the rising edge (menu reappearing after alt-tab back) with the
            // exact io state at the frame it happens, to correlate against the WM_SETFOCUS log
            // in hk_wndproc — same followup as above, diagnostic-only, fires rarely.
            static bool s_prev_show_diag = false;
            const bool new_show = g_user_show && fg;
            if (new_show && !s_prev_show_diag)
            {
                ImGuiIO &io = ImGui::GetIO();
                spdlog::info("[FOCUSDIAG] g_show rising edge (present) fg={} WantCaptureMouse={} "
                             "WantCaptureKeyboard={} MousePos=({:.0f},{:.0f})",
                             fg, io.WantCaptureMouse, io.WantCaptureKeyboard, io.MousePos.x, io.MousePos.y);
            }
            s_prev_show_diag = new_show;
        }
        // <user> 2026-07-01: dropped the `&& fg` gate. This whole session's Alt+Tab bugs (cursor
        // stuck at centre, MousePos permanently invalid, WantCaptureMouse never recovering) all
        // stemmed from state that got reset/invalidated on the focus-loss/regain transition —
        // removing the transition itself (g_show no longer tracks OS focus at all) removes the
        // whole bug class instead of patching each edge case it produces. Tradeoff: F1 now stays
        // fully active (drawing + input capture) even while the game window is in the
        // background — fine for the common "always maximized/borderless" case, but if the user
        // ever alt-tabs to interact with a DIFFERENT window while F1 is open, our input-swallow
        // hooks (hk_wndproc, hk_set_cursor_pos, hk_clip_cursor — all gated on g_show) will still
        // be active and could interfere with that other window. Close F1 before alt-tabbing to
        // avoid that.
        g_show = g_user_show;
        // Count down the item-search nav window (set on a result click) — keeps the map "awake" for the
        // switch+pan, then lets the cursor re-freeze so the map stops drifting.
        if (int nf = g_nav_frames.load(std::memory_order_relaxed))
            g_nav_frames.store(nf - 1, std::memory_order_relaxed);

        // Falling edge (menu JUST closed): restore state the open-state hooks left
        // dangling. While open, hk_clip_cursor forced the cursor UNclipped on every
        // game ClipCursor call; the game only re-clips on a focus change, so after a
        // close the cursor can stay free — leaving mouse-look / map-drag input in a
        // bad state until the next refocus. Re-confine to the window client rect (what
        // the game does during play) so input is never left dangling. Idempotent and
        // safe whether the map is open or in gameplay (ER is window-confined in both).
        // Only re-clip on a real CLOSE (still foreground) — NOT when g_show dropped because focus was
        // lost (the cursor then belongs to the other window; clipping it into the background game
        // window would trap it there).
        static bool s_prev_show = false;
        if (s_prev_show && !g_show && fg && goblin::input::clip_cursor_hooked() && g_hwnd)
        {
            RECT rc;
            if (GetClientRect(g_hwnd, &rc))
            {
                POINT tl{rc.left, rc.top}, br{rc.right, rc.bottom};
                ClientToScreen(g_hwnd, &tl);
                ClientToScreen(g_hwnd, &br);
                RECT screen{tl.x, tl.y, br.x, br.y};
                goblin::input::clip_cursor_real(&screen); // ORIGINAL ClipCursor (g_show already false here)
            }
        }
        s_prev_show = g_show;

        // Item 6: recenter the cursor on the world map's (re)open transition, so the ImGui cursor
        // and ER's own native cursor agree instead of ImGui's carrying over wherever it last was.
        static bool s_prev_map_open = false;
        const bool map_open_now = goblin::world_map_open();
        if (map_open_now && !s_prev_map_open && fg)
            recenter_cursor_to_window();
        s_prev_map_open = map_open_now;

        // Mid-session resolution fix + diagnostic. Run every frame (the fix self-skips
        // when the dims already match) because NOT all resolution changes fire
        // ResizeBuffers — a per-frame enforcer catches the paths the resize hook misses.
        if (goblin::config::fixMidsessionResolution || goblin::config::debugRenderDims)
        {
            DXGI_SWAP_CHAIN_DESC d{};
            swapchain->GetDesc(&d);
            const int bbW = static_cast<int>(d.BufferDesc.Width);
            const int bbH = static_cast<int>(d.BufferDesc.Height);
            if (goblin::config::fixMidsessionResolution)
                goblin::worldmap_probe::reapply_render_res(bbW, bbH); // edge-triggered, idempotent
            if (goblin::config::debugRenderDims)
            {
                static int s_rd_tick = 0;
                if ((s_rd_tick++ % 120) == 0)
                    goblin::worldmap_probe::dump_render_dims(static_cast<float>(bbW),
                                                             static_cast<float>(bbH));
            }
        }

        // The overlay IS the map (native injection removed) → always draw overlay markers over
        // the open map, even with the F1 menu closed (get_live_view() no-ops when the map is shut).
        bool proto = true;
        bool minimap = goblin::config::showMinimap;
        if ((g_show || proto || minimap) && g_command_queue)
        {
            // Our TOTAL per-frame overlay CPU cost (NewFrame + markers + minimap + ImGui render +
            // command-list submit), excluding the game's own Present (o_present, called after this
            // block). Parent of render.worldmap / render.minimap in the [BENCH] report — the gap
            // present.overlay_total − Σ(those children) is UNLABELLED our-code (a benchmarking hole).
            GOBLIN_BENCH_QUIET("present.overlay_total");
            try_upload_atlas();   // one-time; needs the captured command queue
            goblin::input::set_imgui_reading_cursor(true);   // let ImGui's NewFrame see the real cursor
            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            goblin::input::set_imgui_reading_cursor(false);

            // Mouse BUTTONS via polling, not window messages. ER reads input through Raw Input
            // (RIDEV_NOLEGACY under newer wine/Proton), so WM_LBUTTONDOWN/UP are never posted to our
            // WndProc → ImGui's message path sees no clicks, even though hover works (NewFrame polls
            // GetCursorPos for the position). Poll the async button state — exactly how the F1 toggle
            // key is already read (GetAsyncKeyState, focus/message-independent) — and feed ImGui. Only
            // while the panel is up, foreground-guarded so a background click can't leak in.
            if (g_show)
            {
                // Message-driven, same as the top-level `fg` (see g_has_focus) — was an
                // independent GetForegroundWindow() poll, same flapping-under-Wine risk.
                const bool fgw = g_hwnd && g_has_focus.load(std::memory_order_relaxed);
                ImGuiIO &io = ImGui::GetIO();
                // ROOT CAUSE (confirmed via [KBDIAG], <user> 2026-07-01 "after Alt+Tab, can't
                // click in F1"): ImGui_ImplWin32's own NewFrame mouse-position update only feeds
                // io.MousePos via WM_MOUSEMOVE, which this game suppresses during normal gameplay
                // (raw input) — same reason the left-button click below is polled instead of read
                // from WM_LBUTTONDOWN. After a real Alt+Tab, WM_KILLFOCUS invalidates io.MousePos
                // and nothing ever refreshes it again, so WantCaptureMouse stayed false forever
                // even though the button poll correctly saw real clicks.
                //
                // TRUE ROOT CAUSE (<user> pointed at it directly by asking "why can't we use
                // GetCursorPos" — should have re-checked hk_get_cursor_pos's own body sooner):
                // hk_get_cursor_pos DELIBERATELY fakes screen-centre for ANY caller while g_show
                // is true, to freeze the game's own 2D map-panning camera — EXCEPT a caller that
                // sets g_imgui_reading_cursor first (existing mechanism, already used to exempt
                // ImGui_ImplWin32_NewFrame's own internal read, below). Every earlier "GetCursorPos
                // is frozen/stale" diagnosis was this exact self-inflicted fake-centre trap — none
                // of my own polling code (nor the [DIAG] cyan crosshair) was ever setting that
                // exemption flag, so it always got the SAME faked centre value regardless of Wine,
                // Alt+Tab, or anything else. The raw-input-delta virtual cursor (still computed
                // below, kept for the [DIAG] readout) was solving a problem that didn't really
                // exist while missing the real, trivial one — and is inherently less precise than
                // the real value (mickeys-to-pixels scaling is an approximation; GetCursorPos,
                // once unfaked, is exact). Use the exemption directly here instead.
                if (fgw && g_hwnd)
                {
                    POINT pt;
                    goblin::input::set_imgui_reading_cursor(true);
                    const BOOL gotPos = ::GetCursorPos(&pt);
                    goblin::input::set_imgui_reading_cursor(false);
                    if (gotPos && ::ScreenToClient(g_hwnd, &pt))
                    {
                        io.AddMousePosEvent(static_cast<float>(pt.x), static_cast<float>(pt.y));
                        g_diag_raw_cursor_client = pt;
                        g_diag_raw_cursor_valid = true;
                    }
                }
                else
                {
                    g_diag_raw_cursor_valid = false;
                }
                const bool lb = fgw && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
                io.AddMouseButtonEvent(0, lb);
                io.AddMouseButtonEvent(1, fgw && (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
                io.AddMouseButtonEvent(2, fgw && (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);
                // dx-bugs F3: keyboard TEXT input via GetAsyncKeyState poll, same reasoning as
                // the mouse-button poll above but foreground-guarded (fgw) so a background
                // keypress can't leak in. See input_keyboard_poll.cpp's header comment — this is
                // now the SOLE keyboard-text source while the menu is open; input_wndproc.cpp no
                // longer forwards WM_CHAR/WM_KEYDOWN/WM_KEYUP to ImGui at all.
                if (fgw)
                    goblin::input::poll_keyboard_text_input();
                static bool s_logged_click = false;
                if (lb && !s_logged_click)
                {
                    s_logged_click = true;
                    spdlog::info("[CLICKDIAG] L-button delivered to ImGui via GetAsyncKeyState poll "
                                 "(WndProc WM_LBUTTONDOWN seen while open: {})",
                                 goblin::input::diag_wndproc_lbdown_while_open_load());
                }
                // docs/re/proton11_cursor_lock_re_prompt.md step 1: once/sec while the panel is open,
                // log how many times each cursor/raw-input detour fired since last logged. If these
                // stay at 0 while the OS-level mouse is clearly captured (the Proton-11-frozen-cursor
                // symptom), ER's capture is bypassing the user32 exports we hook entirely (H1 native
                // Wayland pointer-lock, or H2 win32u-only path) -- decides which fix branch applies.
                static auto s_diag_last = std::chrono::steady_clock::now();
                auto now = std::chrono::steady_clock::now();
                if (now - s_diag_last >= std::chrono::seconds(1))
                {
                    s_diag_last = now;
                    spdlog::info("[CURSORDIAG] hooks/sec: set_cursor_pos={} clip_cursor={} get_cursor_pos={} "
                                 "raw_input_data={} raw_input_buffer={}",
                                 goblin::input::diag_set_cursor_pos_exchange(),
                                 goblin::input::diag_clip_cursor_exchange(),
                                 goblin::input::diag_get_cursor_pos_exchange(),
                                 goblin::input::diag_get_raw_input_data_exchange(),
                                 goblin::input::diag_get_raw_input_buffer_exchange());
                    // [KBDIAG] dx-bugs 2026-07-01 followup: <user> — keyboard loses the "hook" while
                    // typing in the search bar, WITHOUT any Alt+Tab. `wm_char`/`wm_keydown` = raw
                    // wndproc arrival (0 here while the bug is happening = the OS/our hook truly isn't
                    // delivering keys, look at WM_SETFOCUS/message-pump territory); nonzero arrival but
                    // WantCaptureKeyboard=false or WantTextInput=false = ImGui itself isn't claiming the
                    // keys (look at nav/gamepad-flag territory instead — the ImGuiIO snapshot here is
                    // last-frame's, since this runs just before ImGui::NewFrame()).
                    {
                        // Reuses the outer `io`/`lb` already in scope (the click-poll block just
                        // above, ~L3593) — extended with [KBDIAG] following <user>'s "after
                        // Alt+Tab, can't click in F1" report: `lb` = current polled left-button
                        // state (the Proton/Wine click workaround), `WantCaptureMouse` = whether
                        // ImGui is actually claiming it. lb=true + WantCaptureMouse=false at the
                        // same sample = a real click IS being read but ImGui isn't capturing it
                        // (mouse pos / hover territory); lb never true while clicking = the poll
                        // itself isn't seeing the button (fgw/GetAsyncKeyState territory).
                        spdlog::info("[KBDIAG] wm_char/sec={} wm_keydown/sec={} WantCaptureKeyboard={} "
                                     "WantTextInput={} WantCaptureMouse={} lb={} MousePos=({:.0f},{:.0f}) "
                                     "NavActive={} last_input_was_gamepad={} gamepad_active_streak={}",
                                     goblin::input::diag_wm_char_exchange(), goblin::input::diag_wm_keydown_exchange(),
                                     io.WantCaptureKeyboard, io.WantTextInput, io.WantCaptureMouse, lb,
                                     io.MousePos.x, io.MousePos.y, io.NavActive,
                                     g_last_input_was_gamepad, g_gamepad_active_streak);
                    }
                }
            }

            ImGui::NewFrame();
            // [DIAG] dx-bugs 2026-07-01 Alt+Tab followup, config::debugCursorDiagnostic: two live
            // crosshairs so a recurrence is visible in real time instead of needing another log
            // round-trip. Cyan = raw polled OS cursor (g_diag_raw_cursor_client, captured in the
            // mouse-poll block above regardless of whether it passed the baseline gate). Magenta
            // = io.MousePos, what ImGui itself currently thinks the position is (reflects this
            // frame's AddMousePosEvent, since NewFrame() just processed the input queue). If
            // magenta stops tracking cyan (freezes while cyan keeps moving), THAT is the stale
            // cursor. A text readout also dumps the raw numbers + the relevant gate states.
            if (g_show && goblin::config::debugCursorDiagnostic)
            {
                ImDrawList *diagDl = ImGui::GetBackgroundDrawList();
                const ImGuiIO &diagIo = ImGui::GetIO();
                auto crosshair = [diagDl](ImVec2 p, ImU32 col) {
                    diagDl->AddLine(ImVec2(p.x - 12, p.y), ImVec2(p.x + 12, p.y), col, 2.0f);
                    diagDl->AddLine(ImVec2(p.x, p.y - 12), ImVec2(p.x, p.y + 12), col, 2.0f);
                    diagDl->AddCircle(p, 14.0f, col, 0, 2.0f);
                };
                if (g_diag_raw_cursor_valid)
                    crosshair(ImVec2(static_cast<float>(g_diag_raw_cursor_client.x),
                                      static_cast<float>(g_diag_raw_cursor_client.y)),
                              IM_COL32(0, 255, 255, 255));
                crosshair(diagIo.MousePos, IM_COL32(255, 0, 255, 255));
                char diagBuf[448];
                std::snprintf(diagBuf, sizeof(diagBuf),
                              "[DIAG] GetCursorPos(cyan)=(%ld,%ld) valid=%d  ImGui(magenta)=(%.0f,%.0f)\n"
                              "virtual_cursor(raw-input-driven)=(%.0f,%.0f)\n"
                              "g_has_focus=%d WantCaptureMouse=%d\n"
                              "SetCursorPos/frame=%u last_call=(%d,%d) swallowed=%d",
                              g_diag_raw_cursor_valid ? static_cast<long>(g_diag_raw_cursor_client.x) : -1,
                              g_diag_raw_cursor_valid ? static_cast<long>(g_diag_raw_cursor_client.y) : -1,
                              g_diag_raw_cursor_valid, diagIo.MousePos.x, diagIo.MousePos.y,
                              goblin::input::virtual_cursor_x(),
                              goblin::input::virtual_cursor_y(),
                              g_has_focus.load(std::memory_order_relaxed), diagIo.WantCaptureMouse,
                              goblin::input::diag_set_cursor_pos_live_exchange(),
                              goblin::input::diag_last_set_cursor_pos_x(),
                              goblin::input::diag_last_set_cursor_pos_y(),
                              goblin::input::diag_set_cursor_pos_swallowed());
                diagDl->AddText(ImVec2(10, 10), IM_COL32(255, 255, 255, 255), diagBuf);
            }
            // Draw ImGui's software cursor ONLY while the F1 panel is up AND the world map is CLOSED.
            // (NewFrame now runs every frame for the overlay markers/minimap, so the old init-time
            // MouseDrawCursor=true leaked it into gameplay; and with the world map open ER already
            // draws its own cursor → don't double it.)
            ImGui::GetIO().MouseDrawCursor = g_show && !goblin::world_map_open();
            const OverlayFrameCtx frame_ctx{
                g_atlas_ready ? reinterpret_cast<void *>(g_atlas_gpu.ptr) : nullptr,
                g_hwnd,
                &g_nav_frames,
                &g_gamepad_combo_recording,
                &g_gamepad_combo_ready,
                &g_gamepad_combo_reject_reason,
                &g_item_icon_srvs,
            };
            if (g_show)
                goblin::overlay::draw_panel(frame_ctx);
            if (proto)
                goblin::overlay::draw_worldmap_markers(g_show, frame_ctx);
            if (minimap)
                goblin::overlay::draw_minimap_hud(frame_ctx);   // gameplay HUD (map closed) — self-gates overworld-only
            // Finalize any item-icon GPU copies requested this frame as ONE batch (one execute +
            // one fence wait) before g_command_list is reused for the ImGui render below.
            flush_item_icon_batch();
            ImGui::Render();

            UINT idx = swapchain->GetCurrentBackBufferIndex();
            FrameContext &frame = g_frames[idx];

            frame.allocator->Reset();
            g_command_list->Reset(frame.allocator, nullptr);

            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition.pResource = frame.render_target;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            g_command_list->ResourceBarrier(1, &barrier);

            g_command_list->OMSetRenderTargets(1, &frame.rtv_handle, FALSE, nullptr);
            g_command_list->SetDescriptorHeaps(1, &g_srv_heap);
            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_command_list);

            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            g_command_list->ResourceBarrier(1, &barrier);

            g_command_list->Close();
            ID3D12CommandList *lists[] = {g_command_list};
            g_command_queue->ExecuteCommandLists(1, lists);
        }

        return o_present(swapchain, sync, flags);
    }

    // ── ResizeBuffers hook (resolution / fullscreen change) ───────────────
    HRESULT STDMETHODCALLTYPE hk_resize_buffers(IDXGISwapChain3 *swapchain, UINT count, UINT w,
                                                UINT h, DXGI_FORMAT fmt, UINT flags)
    {
        // Drop our references to the old back buffers before the swapchain
        // recreates them, then rebuild RTVs after.
        if (g_imgui_init)
            release_render_targets();

        HRESULT hr = o_resize_buffers(swapchain, count, w, h, fmt, flags);

        if (g_imgui_init && SUCCEEDED(hr))
        {
            DXGI_SWAP_CHAIN_DESC desc{};
            swapchain->GetDesc(&desc);
            UINT n = (count != 0) ? count : g_buffer_count;
            for (UINT i = 0; i < n && i < g_frames.size(); i++)
            {
                ID3D12Resource *back = nullptr;
                swapchain->GetBuffer(i, IID_PPV_ARGS(&back));
                g_device->CreateRenderTargetView(back, nullptr, g_frames[i].rtv_handle);
                g_frames[i].render_target = back;
            }
        }

        // Flag the resize so the hk_present enforcer fires the re-apply even when the dims
        // read consistent (fullscreen doubling = stale GPU resources, unchanged dims). We
        // only NOTE it here — the re-apply itself (reapply_render_res → FUN_1419ed440)
        // calls ResizeBuffers internally and must run from hk_present, not re-enter here.
        if (SUCCEEDED(hr) && goblin::config::fixMidsessionResolution)
            goblin::worldmap_probe::note_resize_event();
        return hr;
    }

    // ── ExecuteCommandLists hook (captures the game's DIRECT queue) ────────
    void STDMETHODCALLTYPE hk_execute_command_lists(ID3D12CommandQueue *queue, UINT count,
                                                    ID3D12CommandList *const *lists)
    {
        if (!g_command_queue && queue)
        {
            // The swapchain Present uses a DIRECT queue; capture the first one.
            D3D12_COMMAND_QUEUE_DESC qd = queue->GetDesc();
            if (qd.Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
            {
                g_command_queue = queue;
                spdlog::info("[OVERLAY] captured game command queue {:p}",
                             static_cast<void *>(queue));
            }
        }
        o_execute_command_lists(queue, count, lists);
    }

    // ── Vtable resolution via a throwaway device + swapchain ──────────────
    bool resolve_vtables(void **present_addr, void **resize_addr, void **eclist_addr)
    {
        // A hidden message-only-ish window for the dummy swapchain.
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"MfgOverlayDummy";
        RegisterClassExW(&wc);
        HWND dummy = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                                   nullptr, nullptr, wc.hInstance, nullptr);
        if (!dummy)
        {
            UnregisterClassW(wc.lpszClassName, wc.hInstance);
            return false;
        }

        ID3D12Device *device = nullptr;
        ID3D12CommandQueue *queue = nullptr;
        IDXGIFactory4 *factory = nullptr;
        IDXGISwapChain1 *swapchain1 = nullptr;
        bool ok = false;

        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
            goto done;

        {
            D3D12_COMMAND_QUEUE_DESC qd{};
            qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))))
                goto done;
        }

        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            goto done;

        {
            DXGI_SWAP_CHAIN_DESC1 scd{};
            scd.BufferCount = 2;
            scd.Width = 64;
            scd.Height = 64;
            scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            scd.SampleDesc.Count = 1;
            if (FAILED(factory->CreateSwapChainForHwnd(queue, dummy, &scd, nullptr, nullptr,
                                                       &swapchain1)))
                goto done;
        }

        // Present = vtable[8], ResizeBuffers = vtable[13] on IDXGISwapChain;
        // ExecuteCommandLists = vtable[10] on ID3D12CommandQueue.
        *present_addr = vtable_entry(swapchain1, 8);
        *resize_addr = vtable_entry(swapchain1, 13);
        *eclist_addr = vtable_entry(queue, 10);
        ok = true;

    done:
        if (swapchain1) swapchain1->Release();
        if (factory) factory->Release();
        if (queue) queue->Release();
        if (device) device->Release();
        DestroyWindow(dummy);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return ok;
    }
}

bool goblin::input::menu_open() { return g_show; }
int goblin::input::nav_frames_active() { return g_nav_frames.load(std::memory_order_relaxed); }
bool goblin::input::user_show() { return g_user_show; }
void goblin::input::set_has_focus(bool v) { g_has_focus.store(v, std::memory_order_relaxed); }
bool goblin::input::last_input_was_gamepad() { return g_last_input_was_gamepad; }
void goblin::input::set_last_input_was_gamepad(bool v) { g_last_input_was_gamepad = v; }
int goblin::input::gamepad_active_streak() { return g_gamepad_active_streak; }
void goblin::input::set_gamepad_active_streak(int v) { g_gamepad_active_streak = v; }
bool goblin::input::ignore_next_mousemove_for_gamepad_flag() { return g_ignore_next_mousemove_for_gamepad_flag; }
void goblin::input::set_ignore_next_mousemove_for_gamepad_flag(bool v) { g_ignore_next_mousemove_for_gamepad_flag = v; }

void goblin::overlay::initialize()
{
    GOBLIN_BENCH("overlay.init.hooks");
    void *present_addr = nullptr, *resize_addr = nullptr, *eclist_addr = nullptr;
    if (!resolve_vtables(&present_addr, &resize_addr, &eclist_addr))
    {
        spdlog::error("[OVERLAY] vtable resolution failed (dummy swapchain) → overlay disabled");
        g_failed = true;
        return;
    }

    bool ok = true;
    ok &= (MH_CreateHook(present_addr, reinterpret_cast<void *>(&hk_present),
                         reinterpret_cast<void **>(&o_present)) == MH_OK);
    ok &= (MH_CreateHook(resize_addr, reinterpret_cast<void *>(&hk_resize_buffers),
                         reinterpret_cast<void **>(&o_resize_buffers)) == MH_OK);
    ok &= (MH_CreateHook(eclist_addr, reinterpret_cast<void *>(&hk_execute_command_lists),
                         reinterpret_cast<void **>(&o_execute_command_lists)) == MH_OK);
    if (!ok)
    {
        spdlog::error("[OVERLAY] MH_CreateHook failed → overlay disabled");
        g_failed = true;
        return;
    }

    MH_EnableHook(present_addr);
    MH_EnableHook(resize_addr);
    MH_EnableHook(eclist_addr);

    // Cursor hooks (SetCursorPos/ClipCursor/GetCursorPos) — extracted to
    // src/input/input_cursor.cpp (docs/plans/input_module_refactor_plan.md).
    goblin::input::install_cursor_hooks();

    // Raw input hooks (GetRawInputData/GetRawInputBuffer) — extracted to
    // src/input/input_rawinput.cpp (docs/plans/input_module_refactor_plan.md).
    goblin::input::install_rawinput_hooks();

    // XInputGetState (dx-bugs-backlog PR C / PR C-2) — extracted to
    // src/input/input_gamepad.cpp (docs/plans/input_module_refactor_plan.md).
    goblin::input::install_xinput_hook();

    // DirectInput8 mouse/keyboard hook (ER's primary input path) — extracted to
    // src/input/input_directinput.cpp (docs/plans/input_module_refactor_plan.md).
    goblin::input::install_directinput_hooks();

    spdlog::info("[OVERLAY] hooks installed (Present/ResizeBuffers/ExecuteCommandLists"
                 "/SetCursorPos/ClipCursor). F1 toggles.");
}

void goblin::overlay::shutdown()
{
    if (g_failed) return;
    goblin::input::uninstall_wndproc_hook(g_hwnd);
    if (g_imgui_init)
    {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        cleanup_imgui_device();
    }
}

// Phase 2 Slice B (docs/plans/overlay_hot_reload_playwright_plan.md) wrapper surface: the draw
// layer (now src/goblin_overlay_render.cpp) calls these. Mechanical forwards / getters, NOT moved
// because they operate on host-owned g_device/g_command_queue/g_srv_heap/g_frames/g_command_list —
// the same per-frame D3D12 state hk_present resets every frame. `::` disambiguates from this very
// wrapper (same short name, different namespace) so these don't self-recurse.
UINT64 goblin::overlay::ensure_item_icon_srv(int iconId) { return ::ensure_item_icon_srv(iconId); }
bool goblin::overlay::ensure_grace_srv() { return ::ensure_grace_srv(); }
bool goblin::overlay::ensure_grace_dungeon_srv() { return ::ensure_grace_dungeon_srv(); }
void goblin::overlay::force_rebuild_grace() { ::force_rebuild_grace(); }
void goblin::overlay::ensure_grace_debug() { ::ensure_grace_debug(); }
UINT64 goblin::overlay::copy_er_sheet_direct(int &outW, int &outH) { return ::copy_er_sheet_direct(outW, outH); }
UINT64 goblin::overlay::create_tex_from_dds_mem(const uint8_t *data, size_t len, int &outW, int &outH, DXGI_FORMAT &outFmt)
{
    return ::create_tex_from_dds_mem(data, len, outW, outH, outFmt);
}

int goblin::overlay::grace_state() { return g_grace_state; }
int goblin::overlay::grace_dbg_fmt_used() { return g_grace_dbg_fmt_used; }
int *goblin::overlay::grace_dbg_srgb_ptr() { return &g_grace_dbg_srgb; }
int *goblin::overlay::grace_dbg_swiz_ptr() { return &g_grace_dbg_swiz; }

void goblin::overlay::grace_srv_info(void *&gpu, ImVec2 &uv0, ImVec2 &uv1, int &native_w, int &native_h)
{
    gpu = reinterpret_cast<void *>(g_grace_gpu.ptr);
    uv0 = g_grace_uv0; uv1 = g_grace_uv1;
    native_w = g_grace_native_w; native_h = g_grace_native_h;
}

void goblin::overlay::grace_dungeon_srv_info(void *&gpu, ImVec2 &uv0, ImVec2 &uv1)
{
    gpu = reinterpret_cast<void *>(g_grace_dgn_gpu.ptr);
    uv0 = g_grace_dgn_uv0; uv1 = g_grace_dgn_uv1;
}

const std::vector<goblin::overlay::GraceDbg> &goblin::overlay::grace_debug_candidates() { return g_grace_dbg; }

bool goblin::overlay::is_ready() { return g_imgui_init; }

bool goblin::overlay::native_item_icon(int iconId, void *&tex, float &u0, float &v0, float &u1,
                                       float &v1)
{
    if (iconId < 0)
        return false;
    // 1) Preferred: the icon's sheet is already RESIDENT in-game (equipment icons stream their
    //    SB_Icon sheet). Sheet-as-atlas: copy the whole harvested sheet once (cached) + return the
    //    icon's UV sub-rect.
    goblin::ItemSprite sp;
    if (goblin::harvested_icon(iconId, sp) && sp.sheet)
    {
        const SheetTex *st = copy_sheet_cached(reinterpret_cast<ID3D12Resource *>(sp.sheet));
        if (st && st->gpu && st->w > 0 && st->h > 0)
        {
            tex = reinterpret_cast<void *>(st->gpu);
            u0 = (float)sp.x0 / st->w; v0 = (float)sp.y0 / st->h;
            u1 = (float)sp.x1 / st->w; v1 = (float)sp.y1 / st->h;
            return true;
        }
    }
    // 2) No-bake fallback: the sheet ISN'T resident (the common non-equipment case → CreateImage
    //    returns 0x0). Draw the icon from the sheet DDS we read out of the menu texture pack
    //    ourselves: layout iconId → sheet name + pixel rect, upload that sheet's DDS once, crop the
    //    rect as UV. Returns false until the DDS is read off-thread + uploaded (baked atlas meanwhile).
    int x = 0, y = 0, w = 0, h = 0;
    std::string sheet;
    if (!goblin::item_icon_layout_rect(iconId, x, y, w, h, sheet) || w <= 0 || h <= 0 || sheet.empty())
        return false;  // empty sheet = unparsed atlas → baked fallback (parser now drops these upstream)
    if (size_t dot = sheet.rfind('.'); dot != std::string::npos)  // "SB_Icon_00.png" → "SB_Icon_00"
        sheet.resize(dot);
    DiskSheet ds;
    if (!ensure_disk_sheet(sheet, ds) || ds.w <= 0 || ds.h <= 0)
        return false;
    tex = reinterpret_cast<void *>(ds.gpu);
    u0 = (float)x / ds.w; v0 = (float)y / ds.h;
    u1 = (float)(x + w) / ds.w; v1 = (float)(y + h) / ds.h;
    return true;
}

bool goblin::overlay::native_map_point_icon(int iconId, void *&tex, float &u0, float &v0, float &u1,
                                            float &v1)
{
    if (iconId < 0)
        return false;
    // map_icon_rect resolves the MENU_MAP_<NN> symbol's rect + backing sheet from the FD4 image
    // repo (RE: windows_map_point_icon_layout_re_findings.md). Copy the whole sheet once (cached)
    // and return the UV sub-rect — sheet-as-atlas (whole sheet copied once, icons share one SRV).
    // No-op (false) until the world map opened and the symbol is resident.
    int x = 0, y = 0, w = 0, h = 0;
    void *sheet = nullptr;
    if (!goblin::map_icon_rect(iconId, x, y, w, h, sheet) || !sheet || w <= 0 || h <= 0)
        return false;
    const SheetTex *st = copy_sheet_cached(reinterpret_cast<ID3D12Resource *>(sheet));
    if (!st || !st->gpu || st->w <= 0 || st->h <= 0)
        return false;
    tex = reinterpret_cast<void *>(st->gpu);
    u0 = (float)x / st->w; v0 = (float)y / st->h;
    u1 = (float)(x + w) / st->w; v1 = (float)(y + h) / st->h;
    return true;
}

bool goblin::overlay::native_map_point_icon_by_name(const char *name, void *&tex, float &u0,
                                                     float &v0, float &u1, float &v1)
{
    if (!name)
        return false;
    // Name-keyed map symbols (ERR custom: MENU_MAP_ERR_Boss/Camp/…, plus MENU_MAP_Church etc) live
    // in the same image repo, resolved by name. Same sheet-as-atlas copy as the numeric path.
    // Route through the grace-identical pipeline (record_sprite_copy sub-rect + write_inline_srv),
    // per-name cached. The old whole-sheet copy_sheet_cached path drew the boss invisible.
    MapSymSrv *s = nullptr;
    if (!ensure_map_sym_srv(name, s) || !s || !s->gpu.ptr)
        return false;
    tex = reinterpret_cast<void *>(s->gpu.ptr);
    u0 = s->uv0.x; v0 = s->uv0.y; u1 = s->uv1.x; v1 = s->uv1.y;
    return true;
}

// Mod-agnostic DISK map-point glyph: resolve a SB_MapCursor[_02] rect (by name, else numeric iconId)
// from the active install's parsed layout, upload that sheet's DDS once off the live 01_common.tpf, and
// return tex + UV sub-rect. Mirrors native_item_icon's GAP#2 disk branch. Returns false until the DDS is
// read+uploaded (caller falls back to its previous icon). No baked dependency → correct on any mod.
bool goblin::overlay::map_point_glyph_uv(const char *name, int iconId, void *&tex,
                                         float &u0, float &v0, float &u1, float &v1,
                                         int *outW, int *outH)
{
    int x = 0, y = 0, w = 0, h = 0;
    std::string sheet;
    bool got = (name && name[0] && goblin::map_point_rect_by_name(name, x, y, w, h, sheet));
    if (!got && iconId >= 0)
        got = goblin::map_point_rect(iconId, x, y, w, h, sheet);
    if (!got || w <= 0 || h <= 0 || sheet.empty())
        return false;
    if (size_t dot = sheet.rfind('.'); dot != std::string::npos)  // "SB_MapCursor.png" → "SB_MapCursor"
        sheet.resize(dot);
    DiskSheet ds;
    if (!ensure_disk_sheet(sheet, ds) || ds.w <= 0 || ds.h <= 0)
        return false;
    tex = reinterpret_cast<void *>(ds.gpu);
    u0 = (float)x / ds.w; v0 = (float)y / ds.h;
    u1 = (float)(x + w) / ds.w; v1 = (float)(y + h) / ds.h;
    if (outW) *outW = w;   // raw pixel rect dims, before UV normalization (dx-bugs 2026-07-01
    if (outH) *outH = h;   // auto-scale-ratio followup) -- see header comment
    return true;
}
