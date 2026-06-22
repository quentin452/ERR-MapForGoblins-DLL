#include "goblin_overlay.hpp"
#include "goblin_config.hpp"
#include "goblin_quest_steps.hpp"
#include "goblin_debug_events.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <MinHook.h>
#include <spdlog/spdlog.h>

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include "goblin_inject.hpp"   // goblin::world_map_open()
#include "goblin_worldmap_probe.hpp"   // get_live_view() for the marker prototype
#include "goblin_map_data.hpp"         // generated::MAP_ENTRIES (graces for Phase 1)
#include "worldmap/grace_layer.hpp"      // goblin::worldmap::GraceLayer
#include "worldmap/map_entry_layer.hpp"  // goblin::worldmap::MapEntryLayer
#include "worldmap/map_renderer.hpp"     // goblin::worldmap::render_markers
#include "generated_shared/goblin_overlay_icons.hpp" // ATLAS_PNG category-icon atlas
#include "stb_image.h"                                // stbi_load_from_memory (PNG decode)
#include "goblin_bench.hpp"                           // GOBLIN_BENCH scoped timers

#include <vector>
#include <map>
#include <string>
#include <cmath>
#include <cstring>
#include <algorithm>

// ImGui's Win32 backend message handler (defined in imgui_impl_win32.cpp).
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
    // Case-insensitive substring match (empty needle = match all). Used by the
    // Sections & categories search box (the Quest Browser has its own local copy).
    bool contains_ci(const char *hay, const char *need)
    {
        if (!need || !need[0]) return true;
        std::string h, n;
        for (const char *p = hay; p && *p; ++p) h += (char)tolower((unsigned char)*p);
        for (const char *p = need; *p; ++p)     n += (char)tolower((unsigned char)*p);
        return h.find(n) != std::string::npos;
    }

    // ── Hooked function typedefs ──────────────────────────────────────────
    using PresentFn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain3 *, UINT, UINT);
    using ResizeBuffersFn =
        HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain3 *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    using ExecuteCommandListsFn =
        void(STDMETHODCALLTYPE *)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);

    PresentFn o_present = nullptr;
    ResizeBuffersFn o_resize_buffers = nullptr;
    ExecuteCommandListsFn o_execute_command_lists = nullptr;

    // user32 cursor calls — the game recenters/clips the cursor every frame for
    // raw-input mouse-look. We neutralise them while the menu is open so the OS
    // cursor can move freely over the panel.
    using SetCursorPosFn = BOOL(WINAPI *)(int, int);
    using ClipCursorFn = BOOL(WINAPI *)(const RECT *);
    SetCursorPosFn o_set_cursor_pos = nullptr;
    ClipCursorFn o_clip_cursor = nullptr;

    // The 2D world map cursor follows the absolute OS cursor via GetCursorPos
    // (a different path from the 3D camera's DirectInput). We can't freeze it
    // globally — ImGui needs the real position. So return a frozen position to
    // the GAME, the real one to ImGui (flag set only around ImGui's NewFrame).
    using GetCursorPosFn = BOOL(WINAPI *)(LPPOINT);
    GetCursorPosFn o_get_cursor_pos = nullptr;
    bool g_imgui_reading_cursor = false;

    // Raw input — ER reads gameplay keyboard/mouse here (not via window
    // messages), so we neutralise it while the menu is open to fully disable
    // game commands. ImGui still gets keyboard/mouse via the WndProc + cursor.
    using GetRawInputDataFn =
        UINT(WINAPI *)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
    GetRawInputDataFn o_get_raw_input_data = nullptr;

    using GetRawInputBufferFn = UINT(WINAPI *)(PRAWINPUT, PUINT, UINT);
    GetRawInputBufferFn o_get_raw_input_buffer = nullptr;

    // DirectInput8 — ER's primary input path (imports DINPUT8.dll); the world-map
    // cursor/pan follows the mouse through here even after raw input + window
    // messages are blocked. Zero the device state while the menu is open.
    using DIGetDeviceStateFn = HRESULT(STDMETHODCALLTYPE *)(IDirectInputDevice8 *, DWORD, LPVOID);
    using DIGetDeviceDataFn = HRESULT(STDMETHODCALLTYPE *)(IDirectInputDevice8 *, DWORD,
                                                           LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
    DIGetDeviceStateFn o_di_get_device_state = nullptr;
    DIGetDeviceDataFn o_di_get_device_data = nullptr;

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
    WNDPROC g_orig_wndproc = nullptr;

    bool g_imgui_init = false;   // ImGui + D3D resources built against live swapchain
    bool g_failed = false;       // gave up (no overlay), mod continues
    bool g_show = false;         // panel visible this frame
    bool g_user_show = false;    // F1 master open/close (works anywhere = the menu keybind)
    bool g_large = true;         // false = compact widget, true = full panel
    bool g_prev_toggle_down = false;

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
    struct ItemIconSrv { ID3D12Resource *tex = nullptr; D3D12_GPU_DESCRIPTOR_HANDLE gpu{}; bool ok = false; };
    std::map<int, ItemIconSrv> g_item_icon_srvs;
    UINT g_next_item_srv = 2;   // 0 = ImGui font, 1 = category atlas

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
            int w = sp.x1 - sp.x0, h = sp.y1 - sp.y0;
            auto *src_res = reinterpret_cast<ID3D12Resource *>(sp.sheet);
            // Authoritative format from D3D12 (render thread, sheet bound) — NOT the RPM-read
            // sp.format, which is 0 when the sheet was harvested before its texture bound (lazy,
            // findings §2). If the sheet isn't a ready TEXTURE2D yet, return WITHOUT storing a
            // permanent miss so a later frame retries.
            D3D12_RESOURCE_DESC rd = src_res->GetDesc();
            DXGI_FORMAT fmt = rd.Format;
            // DIAG (capped): why does an harvested icon NOT produce an SRV? ready = sheet is a bound
            // TEXTURE2D (GetDesc ok); the rect must be BC-block aligned (4) in BOTH size AND offset.
            static int s_srv_dbg = 0;
            bool ready = (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && fmt != DXGI_FORMAT_UNKNOWN);
            bool size_ok = (w > 0 && h > 0 && w <= 1024 && h <= 1024 && (w % 4) == 0 && (h % 4) == 0);
            bool off_ok = ((sp.x0 % 4) == 0 && (sp.y0 % 4) == 0);
            if (s_srv_dbg < 80 && !(ready && size_ok && off_ok))
            {
                ++s_srv_dbg;
                spdlog::info("[ICONSRV-DBG] id={} rect=({},{})-({},{}) w={} h={} ready={} dim={} fmt={} size_ok={} off_ok={}",
                             iconId, sp.x0, sp.y0, sp.x1, sp.y1, w, h, ready, (int)rd.Dimension,
                             (int)fmt, size_ok, off_ok);
            }
            if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || fmt == DXGI_FORMAT_UNKNOWN)
                return 0;   // sheet not bound yet → retry next frame (no permanent-miss store)
            if (w > 0 && h > 0 && w <= 1024 && h <= 1024 && (w % 4) == 0 && (h % 4) == 0)
            {
                D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC td{};
                td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                td.Width = static_cast<UINT64>(w); td.Height = static_cast<UINT>(h);
                td.DepthOrArraySize = 1; td.MipLevels = 1; td.Format = fmt;
                td.SampleDesc.Count = 1; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                if (SUCCEEDED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&slot.tex))))
                {
                    begin_icon_batch();
                    // src sheet: assume PIXEL_SHADER_RESOURCE → COPY_SOURCE (sampled engine texture).
                    // Per-icon barriers are fine inside one list even when icons share a sheet (each
                    // pair restores it to PSR); the win is collapsing N submits+waits into one.
                    D3D12_RESOURCE_BARRIER bs{};
                    bs.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    bs.Transition.pResource = src_res; bs.Transition.Subresource = 0;
                    bs.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                    bs.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                    g_command_list->ResourceBarrier(1, &bs);

                    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = slot.tex;
                    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
                    D3D12_TEXTURE_COPY_LOCATION csrc{}; csrc.pResource = src_res;
                    csrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; csrc.SubresourceIndex = 0;
                    D3D12_BOX box{};
                    box.left = sp.x0; box.top = sp.y0; box.front = 0;
                    box.right = sp.x1; box.bottom = sp.y1; box.back = 1;
                    g_command_list->CopyTextureRegion(&dst, 0, 0, 0, &csrc, &box);

                    D3D12_RESOURCE_BARRIER after[2]{};
                    after[0] = bs;  // restore src: COPY_SOURCE → PIXEL_SHADER_RESOURCE
                    after[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                    after[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                    after[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    after[1].Transition.pResource = slot.tex; after[1].Transition.Subresource = 0;
                    after[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                    after[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                    g_command_list->ResourceBarrier(2, after);

                    // Reserve the SRV slot now; the descriptor itself is written at flush (after the
                    // copy completes). Park the slot as not-ok so it isn't re-enqueued; flush flips it.
                    UINT idx = g_next_item_srv++;
                    g_pending_icons.push_back({iconId, slot.tex, fmt, idx, w, h});
                    g_item_icon_srvs[iconId] = slot;  // ok=false, tex set → pending
                    return 0;                          // drawn next frame, after flush
                }
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
    int g_grace_state = 0;   // 0 = not ready (retry), 1 = ok, 2 = failed (give up)

    bool ensure_grace_srv()
    {
        if (g_grace_state == 1) return true;
        if (g_grace_state == 2) return false;
        goblin::ItemSprite sp;
        if (!(g_device && g_command_queue && g_srv_heap && g_next_item_srv < 256 &&
              goblin::harvested_grace(sp) && sp.sheet))
            return false;   // not harvested yet → retry next frame (do NOT mark failed)

        auto *src_res = reinterpret_cast<ID3D12Resource *>(sp.sheet);
        DXGI_FORMAT fmt = static_cast<DXGI_FORMAT>(sp.format);
        int x0 = sp.x0 & ~3, y0 = sp.y0 & ~3;            // snap to 4-aligned blocks (BC7)
        int x1 = (sp.x1 + 3) & ~3, y1 = (sp.y1 + 3) & ~3;
        int w = x1 - x0, h = y1 - y0;
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024) { g_grace_state = 2; return false; }

        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = static_cast<UINT64>(w); td.Height = static_cast<UINT>(h);
        td.DepthOrArraySize = 1; td.MipLevels = 1; td.Format = fmt;
        td.SampleDesc.Count = 1; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_grace_tex))))
        { g_grace_state = 2; return false; }

        g_frames[0].allocator->Reset();
        g_command_list->Reset(g_frames[0].allocator, nullptr);
        D3D12_RESOURCE_BARRIER bs{};
        bs.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bs.Transition.pResource = src_res; bs.Transition.Subresource = 0;
        bs.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        bs.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        g_command_list->ResourceBarrier(1, &bs);
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = g_grace_tex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION csrc{}; csrc.pResource = src_res;
        csrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; csrc.SubresourceIndex = 0;
        D3D12_BOX box{}; box.left = x0; box.top = y0; box.front = 0;
        box.right = x1; box.bottom = y1; box.back = 1;
        g_command_list->CopyTextureRegion(&dst, 0, 0, 0, &csrc, &box);
        D3D12_RESOURCE_BARRIER after[2]{};
        after[0] = bs;
        after[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        after[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        after[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        after[1].Transition.pResource = g_grace_tex; after[1].Transition.Subresource = 0;
        after[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        after[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g_command_list->ResourceBarrier(2, after);
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

        const UINT inc = g_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        UINT idx = g_next_item_srv++;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_srv_heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(inc) * idx;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = fmt; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        g_device->CreateShaderResourceView(g_grace_tex, &sd, cpu);
        g_grace_gpu = g_srv_heap->GetGPUDescriptorHandleForHeapStart();
        g_grace_gpu.ptr += static_cast<SIZE_T>(inc) * idx;
        // UV onto the exact grace rect within the snapped copy.
        g_grace_uv0 = ImVec2(static_cast<float>(sp.x0 - x0) / w, static_cast<float>(sp.y0 - y0) / h);
        g_grace_uv1 = ImVec2(static_cast<float>(sp.x1 - x0) / w, static_cast<float>(sp.y1 - y0) / h);
        g_grace_state = 1;
        spdlog::info("[GRACE-SRV] copied {}x{} (snapped from {},{}-{},{}) fmt={} -> slot {} gpu={:#x}",
                     w, h, sp.x0, sp.y0, sp.x1, sp.y1, static_cast<int>(fmt), idx, g_grace_gpu.ptr);
        return true;
    }

    // ── Grace-sprite GPU debug viewer (dev) ──────────────────────────────────────────
    // Draws EVERY harvested SB_ERR_Grace_* frame as a GPU image so we can visually pick the correct
    // grace rect (the captured 'Morning_Color' frame didn't look like a grace). Uses a FULL-SHEET SRV
    // over the engine resource + per-frame UV (NO copy → shows the exact rect content, isolates a
    // wrong-rect bug from a copy/UV bug). Engine-owned sheet (don't Release); dev/map-open only.
    struct GraceDbg { ImTextureID tex; ImVec2 uv0, uv1; std::string name; int w, h; };
    std::vector<GraceDbg> g_grace_dbg;
    size_t g_grace_dbg_built = 0;   // # candidates already turned into SRVs (the list grows live)

    void ensure_grace_debug()
    {
        if (!g_device || !g_srv_heap) return;
        std::vector<goblin::GraceCandidate> cands = goblin::grace_candidates();
        if (cands.size() <= g_grace_dbg_built) return;   // only build SRVs for NEW candidates
        const UINT inc = g_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (size_t i = g_grace_dbg_built; i < cands.size(); ++i)
        {
            const goblin::GraceCandidate &c = cands[i];
            if (!c.spr.sheet || c.spr.sheetW == 0 || c.spr.sheetH == 0 || g_next_item_srv >= 256)
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
        g_grace_dbg_built = cands.size();
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

    // ── Cursor hooks (free the OS cursor while the menu is open) ──────────
    BOOL WINAPI hk_set_cursor_pos(int x, int y)
    {
        if (g_show) return TRUE;            // swallow the game's recenter-to-middle
        return o_set_cursor_pos(x, y);
    }
    BOOL WINAPI hk_clip_cursor(const RECT *rc)
    {
        if (g_show) return o_clip_cursor(nullptr);   // unclip while menu is up
        return o_clip_cursor(rc);
    }

    BOOL WINAPI hk_get_cursor_pos(LPPOINT p)
    {
        BOOL r = o_get_cursor_pos(p);
        // Freeze the cursor the GAME sees while the menu is open (so the 2D map
        // stops following it). ImGui's own NewFrame read gets the real position.
        // Report the SCREEN CENTRE (not the open-time point): the map pans on
        // (cursor - centre) delta, so a static off-centre point = a constant
        // non-zero delta = the map drifts forever (softlock). Centre = zero
        // delta = no pan.
        if (g_show && !g_imgui_reading_cursor && p)
        {
            p->x = GetSystemMetrics(SM_CXSCREEN) / 2;
            p->y = GetSystemMetrics(SM_CYSCREEN) / 2;
        }
        return r;
    }

    UINT WINAPI hk_get_raw_input_data(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size,
                                      UINT hdr)
    {
        UINT ret = o_get_raw_input_data(h, cmd, data, size, hdr);
        // While the menu is open, blank the raw event so the game sees no mouse
        // movement / clicks / key presses. (ImGui's input comes from the
        // WndProc, not from here, so the panel stays fully usable.)
        if (g_show && data && cmd == RID_INPUT)
        {
            auto *ri = reinterpret_cast<RAWINPUT *>(data);
            if (ri->header.dwType == RIM_TYPEMOUSE)
            {
                ri->data.mouse.lLastX = 0;
                ri->data.mouse.lLastY = 0;
                ri->data.mouse.usButtonFlags = 0;
                ri->data.mouse.usButtonData = 0;
            }
            else if (ri->header.dwType == RIM_TYPEKEYBOARD)
            {
                ri->data.keyboard.VKey = 0xFF;          // no valid key
                ri->data.keyboard.Message = WM_NULL;
                ri->data.keyboard.MakeCode = 0;
                ri->data.keyboard.Flags = RI_KEY_BREAK; // treat as key-up
            }
        }
        return ret;
    }

    UINT WINAPI hk_get_raw_input_buffer(PRAWINPUT data, PUINT size, UINT hdr)
    {
        // Batched raw input. While the menu is open, report zero buffered events
        // for actual reads (data != null); pass size-queries through so the
        // game's buffer sizing stays correct.
        if (g_show && data != nullptr) return 0;
        return o_get_raw_input_buffer(data, size, hdr);
    }

    // DirectInput8 device hooks. The vtable is shared by all devices (mouse +
    // keyboard), so zeroing on g_show blocks both — which is exactly what we
    // want while the menu owns input.
    HRESULT STDMETHODCALLTYPE hk_di_get_device_state(IDirectInputDevice8 *dev, DWORD cb,
                                                     LPVOID data)
    {
        HRESULT hr = o_di_get_device_state(dev, cb, data);
        if (g_show && data && SUCCEEDED(hr))
            memset(data, 0, cb);   // no axes / no buttons / no keys
        return hr;
    }
    HRESULT STDMETHODCALLTYPE hk_di_get_device_data(IDirectInputDevice8 *dev, DWORD cb,
                                                    LPDIDEVICEOBJECTDATA rg, LPDWORD inout,
                                                    DWORD flags)
    {
        HRESULT hr = o_di_get_device_data(dev, cb, rg, inout, flags);
        if (g_show && inout)
            *inout = 0;            // report zero buffered events
        return hr;
    }

    // ── WndProc hook (input capture) ──────────────────────────────────────
    LRESULT CALLBACK hk_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (g_show)
        {
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
            // While the menu is open, swallow ALL mouse/keyboard input so the
            // game gets none of it — regardless of where the cursor is (over the
            // map, the panel, anywhere). ImGui was already fed above, so the
            // panel stays fully usable. This stops the world-map panning when
            // the cursor is outside the panel (WantCaptureMouse would be false
            // there and let the move reach the game).
            switch (msg)
            {
            // RELEASES always pass through to the game (fall out of the switch). If we
            // swallowed them, a key/button held BEFORE the overlay opened (or held when the
            // map is quit abnormally) would never get its KEYUP → the game thinks it is held
            // forever → camera/movement stuck "à vie". ImGui was already fed above.
            case WM_KEYUP: case WM_SYSKEYUP:
            case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
                break;
            // PRESSES / moves / wheel / char are consumed so the game gets none while open.
            case WM_INPUT:
            case WM_MOUSEMOVE:
            case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
            case WM_XBUTTONDOWN:
            case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            case WM_KEYDOWN: case WM_CHAR:
            case WM_SYSKEYDOWN:
                return (msg == WM_INPUT) ? 0 : 1;  // consume; game never sees it
            default:
                break;
            }
        }
        return CallWindowProcW(g_orig_wndproc, hwnd, msg, wp, lp);
    }

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
        g_orig_wndproc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hk_wndproc)));

        spdlog::info("[OVERLAY] ImGui initialised: {} back buffers, hwnd={:p}",
                     g_buffer_count, static_cast<void *>(g_hwnd));
        return true;
    }

    // ── Overlay-rendered markers ──────────────────────────────────────────
    // The world map is now drawn by the goblin::worldmap module (src/worldmap/):
    // map_renderer owns projection + motion-sync + group gating + draw; each marker
    // type is a MarkerLayer plugin (graces = 1st impl). This is the NEW overlay-
    // rendered map, distinct from the legacy native WorldMapPointParam injection.
    // Shared overlay marker layers: one GraceLayer (live BonfireWarp graces) + one
    // MapEntryLayer per other category (baked MAP_ENTRIES). Built once; layer order is
    // stable. Used by both the worldmap markers and the minimap HUD.
    std::vector<goblin::worldmap::MarkerLayer *> &overlay_layers()
    {
        namespace wm = goblin::worldmap;
        namespace gen = goblin::generated;
        static wm::GraceLayer s_graces;
        static std::vector<wm::MapEntryLayer> s_cat;     // stable storage
        static std::vector<wm::MarkerLayer *> s_layers;  // pointers into the above
        if (s_layers.empty())
        {
            const int N = static_cast<int>(gen::Category::WorldInteractables) + 1;
            const int graces = static_cast<int>(gen::Category::WorldGraces);
            s_cat.reserve(N); // reserve → no realloc, so the pointers below stay valid
            for (int c = 0; c < N; ++c)
                if (c != graces) // graces come from the live GraceLayer, not MAP_ENTRIES
                    s_cat.emplace_back(c);
            s_layers.push_back(&s_graces);
            for (auto &L : s_cat)
                s_layers.push_back(&L);
        }
        return s_layers;
    }

    void draw_worldmap_markers(bool /*menu_open*/)
    {
        if (!goblin::ui::icons_enabled())
            return; // master off → draw no overlay markers
        namespace wm = goblin::worldmap;
        std::vector<wm::MarkerLayer *> &s_layers = overlay_layers();
        void *atlas = g_atlas_ready ? reinterpret_cast<void *>(g_atlas_gpu.ptr) : nullptr;
        // OS cursor in client/backbuffer px for marker tooltips (the map cursor tracks it).
        float mx = -1.f, my = -1.f;
        POINT pt{};
        BOOL ok = o_get_cursor_pos ? o_get_cursor_pos(&pt) : GetCursorPos(&pt);
        if (ok && g_hwnd && ScreenToClient(g_hwnd, &pt)) { mx = (float)pt.x; my = (float)pt.y; }
        // Hand the renderer the harvested grace sprite (once ready) so it draws graces itself.
        if (ensure_grace_srv())
            wm::set_grace_sprite(reinterpret_cast<void *>(g_grace_gpu.ptr),
                                 g_grace_uv0.x, g_grace_uv0.y, g_grace_uv1.x, g_grace_uv1.y);
        wm::render_markers(s_layers, atlas, mx, my);
    }

    // In-game minimap HUD (corner, north-up, overworld). Drawn during gameplay (map
    // closed) on the foreground draw list. No-ops internally when show_minimap is off,
    // the icons master is off, or the player is underground (pos not yet reliable).
    void draw_minimap_hud()
    {
        void *atlas = g_atlas_ready ? reinterpret_cast<void *>(g_atlas_gpu.ptr) : nullptr;
        ImGuiIO &io = ImGui::GetIO();
        if (ensure_grace_srv())
            goblin::worldmap::set_grace_sprite(reinterpret_cast<void *>(g_grace_gpu.ptr),
                                               g_grace_uv0.x, g_grace_uv0.y, g_grace_uv1.x, g_grace_uv1.y);
        goblin::worldmap::draw_minimap(overlay_layers(), atlas, io.DisplaySize.x,
                                       io.DisplaySize.y);
    }

    void draw_panel()
    {
        ImGuiIO &io = ImGui::GetIO();
        if (!g_large)
        {
            // Compact widget: a small corner pill that expands to the full panel.
            ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(0, 0), ImGuiCond_Always);  // auto-fit
            ImGui::Begin("Map for Goblins##small", nullptr,
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar);
            if (ImGui::Button("Map for Goblins  [+]"))  // expand
                g_large = true;
            ImGui::SameLine();
            ImGui::TextDisabled("F1");
            ImGui::End();
        }
        else
        {
            // Full panel — live controls (post intents to the watcher thread).
            // Auto-fit to content (so it stops being clipped too small), clamped
            // to a sane min/max so it neither shrinks to nothing nor overflows the
            // screen; if content exceeds the max it scrolls.
            ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSizeConstraints(
                ImVec2(360.0f, 240.0f),
                ImVec2(720.0f, io.DisplaySize.y * 0.92f));
            ImGui::Begin("Map for Goblins##large", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
            // Keep the per-category census warm: the watcher only runs the flag
            // sweep while the panel is on-screen (stamped here each frame).
            goblin::ui::note_menu_visible();
            if (ImGui::Button("[-] collapse"))
                g_large = false;
            ImGui::SameLine();
            ImGui::TextDisabled("F1 close | %.0f fps", io.Framerate);
            ImGui::Separator();

            // P2b vertical slice: draw live-harvested item icons (copied GPU→GPU from the engine's
            // menu sheets). Open the inventory first to harvest, then check here. Dev-gated.
            if (goblin::config::dumpIconTextures && ImGui::CollapsingHeader("Item icons (P2b test)"))
            {
                // Draw the ACTUAL harvested icons (a hardcoded id list may not match what the
                // player browsed → empty grid even with harvested>0). Exercises the batch path.
                ImGui::Text("harvested: %zu  (open inventory to fill)", goblin::harvested_count());
                std::vector<int> ids = goblin::harvested_ids(48);
                int drawn = 0;
                for (int id : ids)
                {
                    UINT64 h = ensure_item_icon_srv(id);
                    if (!h) continue;
                    if (drawn++ % 8 != 0) ImGui::SameLine();
                    ImGui::Image(reinterpret_cast<ImTextureID>(h), ImVec2(48, 48));
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("iconId %d", id);
                }
                if (!drawn)
                    ImGui::TextDisabled(ids.empty() ? "no icons harvested yet — open inventory first"
                                                    : "harvested icons not ready yet (1-frame batch)...");
            }

            // Grace-sprite GPU debug: draw every harvested SB_ERR_Grace_* frame (full-sheet SRV +
            // UV, no copy) so we can visually pick the correct grace rect. Open the world map (with a
            // discovered grace) to populate. The frame that looks like a Site of Grace = the one to lock.
            if (goblin::config::dumpIconTextures && ImGui::CollapsingHeader("ERR map sprites (GPU debug)"))
            {
                ensure_grace_debug();
                if (g_grace_dbg.empty())
                    ImGui::TextDisabled("open the world map to harvest SB_ERR_* map sprites");
                for (const GraceDbg &d : g_grace_dbg)
                {
                    ImGui::Image(d.tex, ImVec2(64, 64), d.uv0, d.uv1);
                    ImGui::SameLine();
                    ImGui::Text("%s  %dx%d  uv(%.3f,%.3f)-(%.3f,%.3f)", d.name.c_str(), d.w, d.h,
                                d.uv0.x, d.uv0.y, d.uv1.x, d.uv1.y);
                }
            }

            // Master on/off + Save.
            bool icons_on = goblin::ui::icons_enabled();
            if (ImGui::Checkbox("Show icons (master)", &icons_on))
                goblin::ui::set_icons_enabled(icons_on);
            ImGui::SameLine();
            static double saved_at = -10.0;
            if (ImGui::Button("Save to INI"))
            {
                goblin::ui::request_save();
                saved_at = ImGui::GetTime();
            }
            // Brief confirmation so the button isn't a silent no-op (the file I/O
            // happens on the watcher thread; this just acknowledges the click).
            double since = ImGui::GetTime() - saved_at;
            if (since < 2.0)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Saved to INI");
            }

            // Map-fragment gate (live; persists via "Save to INI"). When on, a marker
            // stays hidden until the player has discovered that area's map fragment.
            ImGui::Checkbox("Require map fragments (hide an area's icons until its fragment is found)",
                            &goblin::config::requireMapFragments);

            // Collected/cleared graying (overlay map; live, persists via "Save to INI").
            // On = dim looted items / killed bosses (cleared bosses get a checkmark);
            // hide_collected switches dim → remove (legacy native-map behaviour).
            ImGui::Checkbox("Gray collected/cleared markers (dim looted items & killed bosses)",
                            &goblin::config::collectedGraying);
            if (goblin::config::collectedGraying)
            {
                ImGui::SameLine();
                ImGui::Checkbox("hide instead", &goblin::config::hideCollected);
            }

            // Major-region name labels (overlay map; live, persists via "Save to INI").
            ImGui::Checkbox("Show region labels (major-region names on the map)",
                            &goblin::config::showRegionLabels);

            // Redify boss markers (overlay port of the legacy red-skull iconId; live,
            // persists via "Save to INI"). Tints WorldBosses markers red (overworld +
            // dungeon bosses); collected/cleared graying still takes precedence.
            ImGui::Checkbox("Red boss markers (tint boss icons red)",
                            &goblin::config::redifyBossIcons);

            // Grace rendering: overlay draws all graces (discovered=colour, undiscovered=grey).
            ImGui::Checkbox("Overlay graces (draw all graces ourselves)",
                            &goblin::config::graceOverlay);
            if (goblin::config::graceOverlay)
            {
                ImGui::SameLine();
                ImGui::Checkbox("GPU sprite (engine, time-tinted) vs CPU (baked atlas)",
                                &goblin::config::graceGpuSprite);
            }

            // Spoiler-free loot (overlay port of anonymous_loot; live, persists via
            // "Save to INI"). Lot-backed loot markers draw as a gray "?" with a generic
            // label instead of the real item icon/name.
            ImGui::Checkbox("Spoiler-free loot (gray \"?\" instead of the item)",
                            &goblin::config::anonymousLoot);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Every loot marker shows a gray \"?\" and only its location,\n"
                                  "hiding the real item (useful with randomizers). Markers still\n"
                                  "gray out when collected; category show/hide is unaffected.");

            // Overlay marker scale (live preview; persists via "Save to INI"). Final
            // size = resolution-relative base × master × per-type scale.
            if (ImGui::CollapsingHeader("Marker scale (overlay map)"))
            {
                ImGui::SliderFloat("Master", &goblin::config::overlayMasterScale, 0.3f, 3.0f, "%.2f");
                ImGui::SliderFloat("Category icons", &goblin::config::overlayIconScale, 0.3f, 3.0f, "%.2f");
                ImGui::SliderFloat("Cluster piles", &goblin::config::overlayClusterScale, 0.3f, 3.0f, "%.2f");
                ImGui::SameLine();
                if (ImGui::SmallButton("Reset##scale"))
                {
                    goblin::config::overlayMasterScale = 1.0f;
                    goblin::config::overlayIconScale = 1.0f;
                    goblin::config::overlayClusterScale = 1.0f;
                }
                ImGui::TextDisabled("Live. × a resolution-relative base. Save to INI to persist.");
            }

            // In-game minimap HUD (foundation; overworld-only, north-up). Live; persists.
            if (ImGui::CollapsingHeader("Minimap (in-game HUD)"))
            {
                ImGui::Checkbox("Show minimap (corner HUD during gameplay)",
                                &goblin::config::showMinimap);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("A small north-up minimap in the screen corner showing nearby\n"
                                      "markers around you during play. OVERWORLD only for now\n"
                                      "(underground player position isn't reliable yet).");
                ImGui::SliderFloat("Zoom (px/world)", &goblin::config::minimapZoom, 0.02f, 0.30f, "%.3f");
                ImGui::SliderFloat("Radius (px)", &goblin::config::minimapSize, 60.0f, 300.0f, "%.0f");
                ImGui::SliderFloat("Opacity", &goblin::config::minimapOpacity, 0.0f, 1.0f, "%.2f");
                ImGui::Checkbox("Anchor right", &goblin::config::minimapAnchorRight);
                ImGui::SameLine();
                ImGui::Checkbox("Anchor bottom", &goblin::config::minimapAnchorBottom);
                ImGui::SliderFloat("Offset X", &goblin::config::minimapOffsetX, 0.0f, 600.0f, "%.0f");
                ImGui::SliderFloat("Offset Y", &goblin::config::minimapOffsetY, 0.0f, 600.0f, "%.0f");
                ImGui::TextDisabled("North-up. Hidden while the world map is open. Save to INI to persist.");
            }

            // Sections (coarse) + their categories (fine). A row shows only if
            // both its section and its category are enabled.
            ImGui::SeparatorText("Sections & categories");
            // Search box: filter the category list by name. Matching a SECTION name
            // shows that whole section; otherwise only matching category rows show,
            // and sections with no match are hidden. Sections auto-expand while
            // filtering so matches are visible without manual clicking.
            static char cat_filter[64] = "";
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            ImGui::InputTextWithHint("##catfilter", "search categories... (e.g. sorcer, ash, smith)",
                                     cat_filter, sizeof(cat_filter));
            const bool cat_filtering = cat_filter[0] != '\0';
            for (int s = 0; s < goblin::ui::section_count(); s++)
            {
                bool sec_name_match = contains_ci(goblin::ui::section_label(s), cat_filter);
                if (cat_filtering && !sec_name_match)
                {
                    // Skip a section entirely if neither it nor any of its categories match.
                    bool any = false;
                    for (int c = 0; c < goblin::ui::category_count() && !any; c++)
                        if (goblin::ui::category_section(c) == s &&
                            contains_ci(goblin::ui::category_label(c), cat_filter))
                            any = true;
                    if (!any) continue;
                }
                if (cat_filtering)
                    ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                if (!ImGui::TreeNode(goblin::ui::section_label(s)))
                    continue;
                // While filtering, a category row shows only if it matches (unless the
                // section name itself matched → show the whole section).
                const bool show_all_cats = !cat_filtering || sec_name_match;

                bool sv = goblin::ui::section_visible(s);
                if (ImGui::Checkbox("(whole section)", &sv))
                    goblin::ui::set_section_visible(s, sv);

                ImGui::PushID(s);
                if (ImGui::SmallButton("Show all"))
                    for (int c = 0; c < goblin::ui::category_count(); c++)
                        if (goblin::ui::category_section(c) == s)
                            goblin::ui::set_category_visible(c, true);
                ImGui::SameLine();
                if (ImGui::SmallButton("Show none"))
                    for (int c = 0; c < goblin::ui::category_count(); c++)
                        if (goblin::ui::category_section(c) == s)
                            goblin::ui::set_category_visible(c, false);
                ImGui::SameLine();
                if (ImGui::SmallButton("Cluster all"))
                    for (int c = 0; c < goblin::ui::category_count(); c++)
                        if (goblin::ui::category_section(c) == s)
                            goblin::ui::set_category_clustered(c, true);
                ImGui::SameLine();
                if (ImGui::SmallButton("Cluster none"))
                    for (int c = 0; c < goblin::ui::category_count(); c++)
                        if (goblin::ui::category_section(c) == s)
                            goblin::ui::set_category_clustered(c, false);
                ImGui::PopID();
                ImGui::TextDisabled("left = show on map   |   right [cluster] = join location pile (live) / unchecked = shown normally");
                ImGui::Separator();

                for (int c = 0; c < goblin::ui::category_count(); c++)
                {
                    if (goblin::ui::category_section(c) != s) continue;
                    if (!show_all_cats && !contains_ci(goblin::ui::category_label(c), cat_filter))
                        continue;   // search box: hide non-matching category rows
                    ImGui::PushID(c);
                    // The raw Quest-NPC map pins are legacy/unfinished — the Quest
                    // Browser below is the supported quest-navigation path. Tag the
                    // label and explain on hover. Off by default.
                    const char *clabel = goblin::ui::category_label(c);
                    bool legacy_quest = std::string(clabel) == "World - Quest NPC";
                    char clbuf[96];
                    if (legacy_quest)
                    {
                        snprintf(clbuf, sizeof(clbuf), "%s  (legacy)", clabel);
                        clabel = clbuf;
                    }
                    bool cv = goblin::ui::category_visible(c);
                    if (ImGui::Checkbox(clabel, &cv))
                        goblin::ui::set_category_visible(c, cv);
                    // Capture row width once, before any SameLine, so the badge and
                    // the cluster checkbox both position from a stable origin.
                    float row_avail = ImGui::GetContentRegionAvail().x;
                    if (legacy_quest && ImGui::IsItemHovered())
                        ImGui::SetTooltip("Legacy / unfinished: raw quest-NPC map pins.\n"
                                          "Use the Quest Browser (below) for quest navigation.\n"
                                          "Off by default.");
                    // Uncollected badge: "<remaining>/<total>" of collectible items in
                    // this category. Skipped for categories with no collectible rows
                    // (graces/NPCs/regions → total 0). Green once fully looted.
                    int rem = goblin::ui::category_remaining(c);
                    int tot = goblin::ui::category_total(c);
                    if (tot > 0)
                    {
                        char cntbuf[24];
                        snprintf(cntbuf, sizeof(cntbuf), "%d/%d", rem < 0 ? 0 : rem, tot);
                        ImGui::SameLine(row_avail - 150.0f);
                        ImVec4 col = (rem == 0) ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f)   // all collected
                                                : ImVec4(0.85f, 0.82f, 0.45f, 1.0f);  // some left
                        ImGui::TextColored(col, "%s", cntbuf);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Items not yet collected / total collectible in this category.");
                    }
                    // Right-aligned cluster opt-in: checked = this category's markers
                    // join the location pile; unchecked = shown normally on the map.
                    // Live (re-plans on next map open).
                    ImGui::SameLine(row_avail - 70.0f);
                    bool clu = goblin::ui::category_clustered(c);
                    if (ImGui::Checkbox("cluster", &clu))
                        goblin::ui::set_category_clustered(c, clu);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Checked = this category's markers fold into the\n"
                                          "one-per-location cluster pile. Unchecked = shown\n"
                                          "normally on the map. Live — reopen the map to apply.");
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }

            ImGui::SeparatorText("ERR integration");
            {
                bool hide_bosses = goblin::ui::err_hide_bosses();
                if (ImGui::Checkbox("Hide boss markers (ERR already marks bosses)", &hide_bosses))
                    goblin::ui::set_err_hide_bosses(hide_bosses);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("ELDEN RING Reforged natively marks bosses (and enemy camps,\n"
                                      "plus completion markers) on the world map. Enable this to hide\n"
                                      "MapForGoblins' own boss markers and avoid the duplicate.\n"
                                      "Same as unchecking 'World - Bosses' above; persists on Save.");
                ImGui::TextDisabled("ERR also marks enemy camps & completion (cleared dungeons/ruins).");
            }

            ImGui::SeparatorText("Quest navigation");
            {
                ImGui::TextDisabled("Use the Quest Browser below. The map-pin options here are");
                ImGui::TextDisabled("legacy / unfinished and off by default.");
                bool qa = goblin::ui::quest_aware();
                if (ImGui::Checkbox("Quest-aware NPCs (legacy / unfinished)", &qa))
                    goblin::ui::set_quest_aware(qa);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("LEGACY / UNFINISHED — superseded by the Quest Browser below.\n"
                                      "Gates the legacy quest-NPC map pins on their questline flag\n"
                                      "(show a pin only while its quest is active). Needs the Quest\n"
                                      "NPC (legacy) category enabled. Off by default.");

                // Quest Browser: ordered steps per NPC (hand-authored, original
                // text). Each step names its location/zone for manual navigation.
                // Grouped into base game + Shadow of the Erdtree via NpcQuest::dlc.
                size_t total = goblin::generated::QUEST_BROWSER_COUNT;
                size_t nbase = 0, ndlc = 0;
                for (size_t i = 0; i < total; i++)
                    (goblin::generated::QUEST_BROWSER[i].dlc ? ndlc : nbase)++;
                char hdr[64];
                snprintf(hdr, sizeof(hdr), "Quest Browser (%zu questlines)", total);
                if (ImGui::TreeNode(hdr))
                {
                    ImGui::TextDisabled("Steps in order; location named per line.");
                    ImGui::TextDisabled("Based on vanilla quests; modded profiles (ERR/Convergence/...) may differ.");
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1.0f));
                    ImGui::TextWrapped("(!) = order-sensitive / missable -- read its note before doing other quests.");
                    ImGui::PopStyleColor();
                    static char filter[64] = "";
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::InputTextWithHint("##questfilter", "filter by NPC name...",
                                             filter, sizeof(filter));
                    // Experimental: grey out questlines whose NPC death flag is set.
                    // Live (read each frame) + persisted to the ini on toggle.
                    if (ImGui::Checkbox("Grey out dead-NPC questlines (experimental)",
                                        &goblin::config::questGreyOnDeath))
                        goblin::ui::request_save();  // watcher-thread sync + persist to ini
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "POTENTIALLY BUGGY. Death flags are reverse-engineered per NPC;\n"
                            "a line may grey incorrectly, or stay normal when its NPC is gone.\n"
                            "Some flags are also shared with normal completion ([concluded]).\n"
                            "Turn this off to always show every questline. Saved to the ini.");
                    auto contains_ci = [](const char *hay, const char *need) {
                        if (!need[0]) return true;
                        std::string h, n;
                        for (const char *p = hay; *p; ++p) h += (char)tolower(*p);
                        for (const char *p = need; *p; ++p) n += (char)tolower(*p);
                        return h.find(n) != std::string::npos;
                    };
                    // Per-step progress is keyed by NPC NAME (stable), not array
                    // position: blob format "<name>=<bits>;<name>=<bits>;". So
                    // reordering or inserting NPCs no longer shifts saved ticks
                    // (the old positional bitstring from commit 40691d5 drifted).
                    // Names carry no '=' or ';'; bits are one '0'/'1' per step.
                    std::string &qp = goblin::config::questProgress;
                    // One-time migration: a non-empty blob with no '=' is the old
                    // positional format. Walk author order and re-key by name. Base
                    // NPCs precede all DLC, so their global indices are unchanged by
                    // the now-authored DLC steps; legacy bits map cleanly.
                    if (!qp.empty() && qp.find('=') == std::string::npos)
                    {
                        std::string out;
                        size_t g = 0;
                        for (size_t i = 0; i < total; i++)
                        {
                            const auto &q = goblin::generated::QUEST_BROWSER[i];
                            std::string bits;
                            for (size_t s = 0; s < q.step_count; s++)
                                bits += (g + s < qp.size() && qp[g + s] == '1') ? '1' : '0';
                            if (bits.find('1') != std::string::npos)
                                out += std::string(q.name) + "=" + bits + ";";
                            g += q.step_count;
                        }
                        qp = out;
                    }
                    // Parse keyed blob -> name -> bits.
                    std::map<std::string, std::string> prog;
                    for (size_t p = 0; p < qp.size();)
                    {
                        size_t semi = qp.find(';', p);
                        if (semi == std::string::npos) semi = qp.size();
                        size_t eq = qp.rfind('=', semi);
                        if (eq != std::string::npos && eq > p)
                            prog[qp.substr(p, eq - p)] = qp.substr(eq + 1, semi - eq - 1);
                        p = semi + 1;
                    }
                    auto reserialize = [&]() {
                        std::string out;
                        for (auto &kv : prog)
                            if (kv.second.find('1') != std::string::npos)
                                out += kv.first + "=" + kv.second + ";";
                        qp = out;
                    };
                    auto qp_get = [&](const char *name, size_t s) {
                        auto it = prog.find(name);
                        return it != prog.end() && s < it->second.size() && it->second[s] == '1';
                    };
                    auto qp_set = [&](const char *name, size_t s, bool v) {
                        std::string &bits = prog[name];
                        if (bits.size() <= s) bits.resize(s + 1, '0');
                        bits[s] = v ? '1' : '0';
                        reserialize();
                    };
                    // Render one NPC subtree. The tree ID is derived from the
                    // stable array index (ptr-id overload), NOT the label text —
                    // the label carries the live (done/total) count, and hashing
                    // that would change the node's ID every tick and silently
                    // collapse the subtree on each click.
                    auto draw_npc = [&](const goblin::generated::NpcQuest &q, int id) {
                        if (!contains_ci(q.name, filter)) return;
                        int done = 0;
                        for (size_t s = 0; s < q.step_count; s++)
                            if (qp_get(q.name, s)) done++;
                        // Visual state: grey "[unfinishable]" (NPC dead, fail_flag
                        // set) takes precedence over amber "(!)" (order-sensitive /
                        // missable). Both push a text tint over the whole subtree.
                        bool dead = goblin::config::questGreyOnDeath
                                    && goblin::ui::quest_unfinishable((size_t)id);
                        // `concl`: the fail_flag is the NPC's shared "concluded"
                        // flag (set on completion OR death) -- grey it, but label
                        // it [concluded] rather than asserting the NPC is dead.
                        bool concl = dead && q.fail_conclusion;
                        bool warn = q.warning && q.warning[0];
                        bool tint = dead || warn;
                        if (tint)
                            ImGui::PushStyleColor(ImGuiCol_Text,
                                dead ? ImVec4(0.55f, 0.55f, 0.55f, 1.0f)
                                     : ImVec4(1.0f, 0.78f, 0.35f, 1.0f));
                        bool open = ImGui::TreeNode((void *)(intptr_t)id, "%s  (%d/%zu)%s",
                                                    q.name, done, q.step_count,
                                                    dead ? (concl ? "  [concluded]"
                                                                  : "  [unfinishable]")
                                                         : warn ? "  (!)" : "");
                        if (tint)
                            ImGui::PopStyleColor();
                        if (open)
                        {
                            if (dead && concl)
                            {
                                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.70f, 0.70f, 1.0f));
                                ImGui::TextWrapped("[concluded] This questline is over -- the NPC has "
                                                   "either finished their story or is gone. (This flag "
                                                   "is set on completion as well as on death.)");
                                ImGui::PopStyleColor();
                            }
                            else if (dead)
                            {
                                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.45f, 0.45f, 1.0f));
                                ImGui::TextWrapped("[unfinishable] This questline's NPC is dead "
                                                   "-- it can no longer be completed.");
                                ImGui::PopStyleColor();
                            }
                            if (warn)
                            {
                                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.66f, 0.28f, 1.0f));
                                ImGui::TextWrapped("(!) %s", q.warning);
                                ImGui::PopStyleColor();
                            }
                            if (q.related)
                            {
                                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.8f, 1.0f, 1.0f));
                                ImGui::TextWrapped("Link: %s", q.related);
                                ImGui::PopStyleColor();
                            }
                            for (size_t s = 0; s < q.step_count; s++)
                            {
                                ImGui::PushID((int)s);
                                bool d = qp_get(q.name, s);
                                if (ImGui::Checkbox("##done", &d))
                                    qp_set(q.name, s, d);
                                ImGui::SameLine();
                                ImGui::TextWrapped("%zu. %s", s + 1, q.steps[s].title);
                                ImGui::Indent();
                                if (q.steps[s].desc && q.steps[s].desc[0])
                                {
                                    ImGui::PushStyleColor(ImGuiCol_Text,
                                        ImVec4(0.75f, 0.75f, 0.75f, 1.0f));
                                    ImGui::TextWrapped("%s", q.steps[s].desc);
                                    ImGui::PopStyleColor();
                                }
                                if (q.steps[s].zone && q.steps[s].zone[0])
                                    ImGui::TextDisabled("[%s]", q.steps[s].zone);
                                ImGui::Unindent();
                                ImGui::PopID();
                            }
                            ImGui::TreePop();
                        }
                    };

                    ImGui::BeginChild("questlist", ImVec2(0, 300), true);
                    char gh[48];
                    snprintf(gh, sizeof(gh), "Base game (%zu)", nbase);
                    if (ImGui::TreeNodeEx(gh, ImGuiTreeNodeFlags_DefaultOpen))
                    {
                        for (size_t i = 0; i < total; i++)
                            if (!goblin::generated::QUEST_BROWSER[i].dlc)
                                draw_npc(goblin::generated::QUEST_BROWSER[i], (int)i);
                        ImGui::TreePop();
                    }
                    snprintf(gh, sizeof(gh), "Shadow of the Erdtree (%zu)", ndlc);
                    if (ImGui::TreeNodeEx(gh, ImGuiTreeNodeFlags_DefaultOpen))
                    {
                        for (size_t i = 0; i < total; i++)
                            if (goblin::generated::QUEST_BROWSER[i].dlc)
                                draw_npc(goblin::generated::QUEST_BROWSER[i], (int)i);
                        ImGui::TreePop();
                    }
                    ImGui::EndChild();
                    ImGui::TextDisabled("Tick steps to track progress; Save to keep it. Original text.");
                    ImGui::TreePop();
                }
            }

            ImGui::SeparatorText("Clustering");
            {
                // Master enable — ALWAYS shown. (Bug: this was gated on
                // clustering_active(), so once a re-plan found no pile over the
                // threshold the whole block vanished and clustering could not be
                // re-enabled without a restart.) Drives config::enableClustering and
                // re-plans live; enabled ⇔ dense piles collapsed into one icon.
                bool en = goblin::ui::clustering_enabled();
                if (ImGui::Checkbox("Enable clustering (declutter dense areas)", &en))
                    goblin::ui::set_clustering_enabled(en);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Collapse dense marker piles into one icon to keep busy\n"
                                      "regions readable. NOT for performance — the overlay map\n"
                                      "has no freeze. Live (updates as you pan/zoom the map).");

                if (en)
                {
                    ImGui::TextDisabled("One mixed pile per location. Per-category opt-in above.");
                    // Cluster size = how many markers a LOCATION must hold to collapse
                    // into one pile (sparse locations stay normal).
                    int gt = goblin::ui::global_threshold();
                    ImGui::SetNextItemWidth(90.0f);
                    if (ImGui::InputInt("Cluster size — markers per location (live)", &gt))
                        goblin::ui::set_global_threshold(gt);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("A location collapses into one pile when it holds MORE\n"
                                          "than this many (clustered-category) markers. LOWER = MORE\n"
                                          "clustering. Min 1. Live — reopen the map to see it.\n"
                                          "When distance-adaptive is on, this is the FAR (clustered) size.");

                    // Distance-adaptive: scale the size up far from the player.
                    bool da = goblin::config::clusterDistanceAdaptive;
                    if (ImGui::Checkbox("Distance-adaptive (cluster by distance from player)", &da))
                    {
                        goblin::config::clusterDistanceAdaptive = da;
                        goblin::ui::request_cluster_replan();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Own mode: OVERRIDES the per-category opt-in above and clusters\n"
                                          "EVERY category by distance — full detail (individual items)\n"
                                          "near you, dense spots far away merged into piles. Ramps from\n"
                                          "the near size to the far size below. OVERWORLD ONLY (the\n"
                                          "underground player position isn't available, so underground\n"
                                          "uses normal threshold + per-category). Live.");
                    if (da)
                    {
                        int nr = goblin::config::clusterNearRadius;
                        ImGui::SetNextItemWidth(160.0f);
                        if (ImGui::SliderInt("Near radius (tiles)", &nr, 1, 40))
                        { goblin::config::clusterNearRadius = static_cast<uint8_t>(nr); goblin::ui::request_cluster_replan(); }
                        int nt = goblin::config::clusterNearThreshold;
                        ImGui::SetNextItemWidth(160.0f);
                        if (ImGui::SliderInt("Detail near you (cluster size)", &nt, 1, 60))
                        { goblin::config::clusterNearThreshold = static_cast<uint8_t>(nt); goblin::ui::request_cluster_replan(); }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Cluster size WITHIN the near radius (higher = more individual\n"
                                              "items shown near you). Ramps down to 'Cluster size' (above) far away.");
                        int fr = goblin::config::clusterFarRadius;
                        ImGui::SetNextItemWidth(160.0f);
                        if (ImGui::SliderInt("Far radius (tiles)", &fr, 2, 80))
                        { goblin::config::clusterFarRadius = static_cast<uint8_t>(fr); goblin::ui::request_cluster_replan(); }

                        // Debug viz: draw the player + near/far rings (overworld) and
                        // per-pile sub-page tab (underground) so you can SEE where the
                        // distance ramp engages and pin the bug.
                        ImGui::Checkbox("DEBUG: distance zones (player + near/far rings)",
                                        &goblin::config::clusterDebugRadius);
                        ImGui::Checkbox("DEBUG: cluster anchors (pile→member lines + name)",
                                        &goblin::config::debugClusterAnchors);
                        ImGui::Checkbox("DEBUG: region volumes (names; red = unresolved)",
                                        &goblin::config::debugRegionVolumes);
                    }

                    // Player-profile presets — one click sets every cluster knob.
                    ImGui::TextDisabled("Preset:");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Completionist"))
                    {
                        // Anchors aggregate ~40 markers each, so only a very high
                        // threshold leaves them as individual items everywhere.
                        goblin::ui::set_global_threshold(60);
                        goblin::config::clusterDistanceAdaptive = false;
                        goblin::ui::request_cluster_replan();
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Max detail: individual items everywhere; only the densest spots cluster. No distance scaling.");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Explorer"))
                    {
                        // Tight radius: only the area immediately around you is detailed,
                        // everything else clusters (covers just the useful part of the
                        // map, not the whole thing). Verified-good config.
                        goblin::ui::set_global_threshold(4);           // far = clustered
                        goblin::config::clusterDistanceAdaptive = true;
                        goblin::config::clusterNearThreshold = 60;     // near = individual items
                        goblin::config::clusterNearRadius = 1;
                        goblin::config::clusterFarRadius = 2;
                        goblin::ui::request_cluster_replan();
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Balanced: individual items in your immediate area, everything else merged.");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Performance"))
                    {
                        goblin::ui::set_global_threshold(2);           // far = very aggressive
                        goblin::config::clusterDistanceAdaptive = true;
                        goblin::config::clusterNearThreshold = 30;     // tighter detail bubble
                        goblin::config::clusterNearRadius = 1;
                        goblin::config::clusterFarRadius = 2;
                        goblin::ui::request_cluster_replan();
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Aggressive far-merge: fewest distant icons, most declutter.");
                }
            }

            // Dev/debug observers. Each installs a hook or starts a worker thread
            // ONCE at startup based on its config flag (dllmain setup), so these
            // are restart-required: flip + Save, then relaunch. save_all_bool_settings
            // persists every config bool, so no per-flag plumbing is needed.
            ImGui::SeparatorText("Debug");
            // Live toggle (no restart): project_marker reads this every frame.
            ImGui::Checkbox("Live projection (engine world→map fn; fixes dungeon/UG placement)",
                            &goblin::config::liveProjection);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Project markers with the game's OWN WorldMapViewModel instead of the baked\n"
                    "affine. Toggles live (open the map and flip it to compare). Underground /\n"
                    "dungeon markers snap to their real LegacyConv-folded positions. Falls back\n"
                    "to baked until the map is open (engine VM must resolve).");
            if (ImGui::TreeNode("Dev tools (Save + restart)"))
            {
                ImGui::Checkbox("Event-flag hook (coverage-gap detector)",
                                &goblin::config::debugEventFlags);
                ImGui::Checkbox("Item-grant hook (coverage-gap detector)",
                                &goblin::config::debugItemGrants);
                ImGui::Checkbox("World-map cursor probe (logs cursor coords)",
                                &goblin::config::debugWorldmapProbe);
                ImGui::Checkbox("Marker-dump hotkey (F9 → markers log)",
                                &goblin::config::enableMarkerDump);
                ImGui::Checkbox("Verbose logging (addresses, param internals)",
                                &goblin::config::debugLogging);
                ImGui::Checkbox("Flag-capture hook (NPC death-flag tool)",
                                &goblin::config::debugFlagCapture);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("These take effect after Save + a game restart — each\n"
                                      "installs its hook/worker once at startup.");

                // Flag-capture tool: arm naming an NPC, kill it, finalize -> the
                // persisted death flag(s) are logged as fail_flag candidates.
                ImGui::Separator();
                ImGui::TextDisabled("NPC death-flag capture (needs the hook above + restart)");
                static int cap_sel = 0;
                size_t qn = goblin::generated::QUEST_BROWSER_COUNT;
                if (cap_sel >= (int)qn) cap_sel = 0;
                const char *cap_name = goblin::generated::QUEST_BROWSER[cap_sel].name;
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::BeginCombo("##capnpc", cap_name))
                {
                    for (int i = 0; i < (int)qn; i++)
                        if (ImGui::Selectable(goblin::generated::QUEST_BROWSER[i].name,
                                              i == cap_sel))
                            cap_sel = i;
                    ImGui::EndCombo();
                }
                static int cap_last = -1;
                if (!goblin::debug_events::capture_armed())
                {
                    if (ImGui::Button("Arm capture"))
                    {
                        goblin::debug_events::arm_capture(cap_name);
                        cap_last = -1;
                    }
                    if (cap_last >= 0)
                    {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f),
                                           "logged %d -> flagcapture.txt", cap_last);
                    }
                }
                else
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.3f, 1.0f));
                    ImGui::TextWrapped("ARMED for '%s' — kill it, wait ~5s, then Finalize.",
                                       cap_name);
                    ImGui::PopStyleColor();
                    ImGui::Text("flags captured: %zu", goblin::debug_events::capture_count());
                    if (ImGui::Button("Finalize -> log"))
                        cap_last = goblin::debug_events::finalize_capture(
                            &goblin::ui::read_event_flag);
                }
                ImGui::TreePop();
            }

            // ── Danger zone: destructive resets behind a confirm popup ────────
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.13f, 0.13f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.78f, 0.18f, 0.18f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.90f, 0.22f, 0.22f, 1.0f));
            if (ImGui::TreeNode("Danger zone"))
            {
                if (ImGui::Button("Reset quest progression"))
                    ImGui::OpenPopup("##confirm_reset_quest");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Clear every quest-step checkmark. Save to INI to persist.");
                if (ImGui::Button("Reset parameters to default"))
                    ImGui::OpenPopup("##confirm_reset_params");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Restore all settings to defaults and write the ini.\n"
                                      "Restart the game to fully apply.");

                if (ImGui::BeginPopupModal("##confirm_reset_quest", nullptr,
                                           ImGuiWindowFlags_AlwaysAutoResize))
                {
                    ImGui::TextUnformatted("Clear ALL quest-step checkmarks?");
                    ImGui::TextDisabled("Cannot be undone. Save to INI afterwards to persist.");
                    if (ImGui::Button("Yes, clear"))
                    {
                        goblin::ui::reset_quest_progress();
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel"))
                        ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
                if (ImGui::BeginPopupModal("##confirm_reset_params", nullptr,
                                           ImGuiWindowFlags_AlwaysAutoResize))
                {
                    ImGui::TextUnformatted("Reset ALL settings to defaults?");
                    ImGui::TextDisabled("Writes defaults to MapForGoblins.ini. Restart to fully apply.");
                    if (ImGui::Button("Yes, reset"))
                    {
                        goblin::ui::reset_to_defaults();
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel"))
                        ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
                ImGui::TreePop();
            }
            ImGui::PopStyleColor(3);

            ImGui::End();
        }
    }

    // ── Present hook (renders the overlay) ────────────────────────────────
    HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain3 *swapchain, UINT sync, UINT flags)
    {
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
        bool down = (GetAsyncKeyState(static_cast<int>(goblin::config::overlayToggleKey)) & 0x8000) != 0;
        if (down && !g_prev_toggle_down) g_user_show = !g_user_show;
        g_prev_toggle_down = down;
        g_show = g_user_show;

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

        // The marker prototype draws over the open map even when the menu is closed,
        // so build a frame for it too (get_live_view() no-ops when the map is shut).
        // Draw overlay markers when the prototype flag is on OR native injection is
        // off (overlay is then the sole map).
        bool proto = goblin::config::overlayMarkersProto || !goblin::config::nativeMapInjection;
        bool minimap = goblin::config::showMinimap;
        if ((g_show || proto || minimap) && g_command_queue)
        {
            try_upload_atlas();   // one-time; needs the captured command queue
            g_imgui_reading_cursor = true;   // let ImGui's NewFrame see the real cursor
            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            g_imgui_reading_cursor = false;
            ImGui::NewFrame();
            // Draw ImGui's software cursor ONLY while the F1 panel is up AND the world map is CLOSED.
            // (NewFrame now runs every frame for the overlay markers/minimap, so the old init-time
            // MouseDrawCursor=true leaked it into gameplay; and with the world map open ER already
            // draws its own cursor → don't double it.)
            ImGui::GetIO().MouseDrawCursor = g_show && !goblin::world_map_open();
            if (g_show)
                draw_panel();
            if (proto)
                draw_worldmap_markers(g_show);
            if (minimap)
                draw_minimap_hud();   // gameplay HUD (map closed) — self-gates overworld-only
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

    // Cursor hooks (user32) — let the OS cursor move freely over the menu by
    // neutralising the game's per-frame recenter/clip while the panel is open.
    if (HMODULE u32 = GetModuleHandleW(L"user32.dll"))
    {
        void *scp = reinterpret_cast<void *>(GetProcAddress(u32, "SetCursorPos"));
        void *clp = reinterpret_cast<void *>(GetProcAddress(u32, "ClipCursor"));
        if (scp && MH_CreateHook(scp, reinterpret_cast<void *>(&hk_set_cursor_pos),
                                 reinterpret_cast<void **>(&o_set_cursor_pos)) == MH_OK)
            MH_EnableHook(scp);
        if (clp && MH_CreateHook(clp, reinterpret_cast<void *>(&hk_clip_cursor),
                                 reinterpret_cast<void **>(&o_clip_cursor)) == MH_OK)
            MH_EnableHook(clp);
        void *grid = reinterpret_cast<void *>(GetProcAddress(u32, "GetRawInputData"));
        if (grid && MH_CreateHook(grid, reinterpret_cast<void *>(&hk_get_raw_input_data),
                                  reinterpret_cast<void **>(&o_get_raw_input_data)) == MH_OK)
            MH_EnableHook(grid);
        void *grib = reinterpret_cast<void *>(GetProcAddress(u32, "GetRawInputBuffer"));
        if (grib && MH_CreateHook(grib, reinterpret_cast<void *>(&hk_get_raw_input_buffer),
                                  reinterpret_cast<void **>(&o_get_raw_input_buffer)) == MH_OK)
            MH_EnableHook(grib);
        void *gcp = reinterpret_cast<void *>(GetProcAddress(u32, "GetCursorPos"));
        if (gcp && MH_CreateHook(gcp, reinterpret_cast<void *>(&hk_get_cursor_pos),
                                 reinterpret_cast<void **>(&o_get_cursor_pos)) == MH_OK)
            MH_EnableHook(gcp);
    }

    // DirectInput8 mouse/keyboard hook (ER's primary input path). Resolve the
    // IDirectInputDevice8 vtable via a throwaway mouse device, then hook
    // GetDeviceState (vtable[9]) + GetDeviceData (vtable[10]).
    {
        IDirectInput8 *di8 = nullptr;
        IDirectInputDevice8 *dev = nullptr;
        if (SUCCEEDED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
                                         IID_IDirectInput8, reinterpret_cast<void **>(&di8),
                                         nullptr)) &&
            di8 && SUCCEEDED(di8->CreateDevice(GUID_SysMouse, &dev, nullptr)) && dev)
        {
            void **vt = *reinterpret_cast<void ***>(dev);
            void *gds = vt[9], *gdd = vt[10];
            if (MH_CreateHook(gds, reinterpret_cast<void *>(&hk_di_get_device_state),
                              reinterpret_cast<void **>(&o_di_get_device_state)) == MH_OK)
                MH_EnableHook(gds);
            if (MH_CreateHook(gdd, reinterpret_cast<void *>(&hk_di_get_device_data),
                              reinterpret_cast<void **>(&o_di_get_device_data)) == MH_OK)
                MH_EnableHook(gdd);
            spdlog::info("[OVERLAY] DirectInput8 device hooks installed");
        }
        else
        {
            spdlog::warn("[OVERLAY] DirectInput8 resolve failed — map may still follow mouse");
        }
        if (dev) dev->Release();
        if (di8) di8->Release();
    }

    spdlog::info("[OVERLAY] hooks installed (Present/ResizeBuffers/ExecuteCommandLists"
                 "/SetCursorPos/ClipCursor). F1 toggles.");
}

void goblin::overlay::shutdown()
{
    if (g_failed) return;
    if (g_hwnd && g_orig_wndproc)
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_orig_wndproc));
    if (g_imgui_init)
    {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        cleanup_imgui_device();
    }
}

bool goblin::overlay::is_ready() { return g_imgui_init; }
