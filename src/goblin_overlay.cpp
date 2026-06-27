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
#include "worldmap/category_meta.hpp"    // baked→GPU icon migration counters (F1 panel)
#include "worldmap/loot_disk.hpp"        // disk_loot_state — F1 "maps not found" error
#include "re_signatures.hpp"             // sig_health — F1 "signatures unresolved" error
#include "goblin_messages.hpp"           // lookup_text_utf8 (item-search name resolution)
#include "generated_shared/goblin_overlay_icons.hpp" // ATLAS_PNG category-icon atlas
#include "generated_shared/dejavu_sans_ttf.h"         // embedded DejaVu Sans (extended-Latin glyphs)
#include "stb_image.h"                                // stbi_load_from_memory (PNG decode)
#include "goblin_bench.hpp"                           // GOBLIN_BENCH scoped timers

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

// ImGui's Win32 backend message handler (defined in imgui_impl_win32.cpp).
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
    // Append codepoint `cp`, folded to lowercase ASCII, to `out`. Maps Latin-1 and a
    // few Latin-Extended accented letters to their base letter so a query typed without
    // accents (or in another locale) still matches the localized name. Non-Latin code
    // points that have no ASCII base are dropped (harmless for substring search).
    void append_folded(std::string &out, uint32_t cp)
    {
        if (cp < 0x80) { out += (char)tolower((int)cp); return; }
        switch (cp)
        {
        case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5:
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: out += 'a'; break;
        case 0xC6: case 0xE6: out += "ae"; break;
        case 0xC7: case 0xE7: out += 'c'; break;
        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        case 0xE8: case 0xE9: case 0xEA: case 0xEB: out += 'e'; break;
        case 0xCC: case 0xCD: case 0xCE: case 0xCF:
        case 0xEC: case 0xED: case 0xEE: case 0xEF: out += 'i'; break;
        case 0xD0: case 0xF0: out += 'd'; break;
        case 0xD1: case 0xF1: out += 'n'; break;
        case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD8:
        case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF8: out += 'o'; break;
        case 0xD9: case 0xDA: case 0xDB: case 0xDC:
        case 0xF9: case 0xFA: case 0xFB: case 0xFC: out += 'u'; break;
        case 0xDD: case 0xFD: case 0xFF: out += 'y'; break;
        case 0xDE: case 0xFE: out += "th"; break;
        case 0xDF: out += "ss"; break;
        case 0x152: case 0x153: out += "oe"; break;  // Œ / œ
        default: break;  // no ASCII base — drop
        }
    }

    // Decode UTF-8 `s` into a lowercase, accent-folded ASCII string (see append_folded).
    std::string fold_ci(const char *s)
    {
        std::string out;
        const unsigned char *p = (const unsigned char *)s;
        while (p && *p)
        {
            unsigned char c = *p;
            uint32_t cp;
            int len;
            if (c < 0x80)            { cp = c;        len = 1; }
            else if ((c >> 5) == 0x6)  { cp = c & 0x1F; len = 2; }
            else if ((c >> 4) == 0xE)  { cp = c & 0x0F; len = 3; }
            else if ((c >> 3) == 0x1E) { cp = c & 0x07; len = 4; }
            else { ++p; continue; }  // stray continuation / invalid lead — skip
            int i = 1;
            for (; i < len; ++i)
            {
                if ((p[i] & 0xC0) != 0x80) break;  // truncated sequence
                cp = (cp << 6) | (p[i] & 0x3F);
            }
            p += i;  // advance by bytes actually consumed
            append_folded(out, cp);
        }
        return out;
    }

    // Case-insensitive, accent-insensitive substring match (empty needle = match all).
    // Used by the Sections & categories search box (Quest Browser has its own copy).
    bool contains_ci(const char *hay, const char *need)
    {
        if (!need || !need[0]) return true;
        return fold_ci(hay).find(fold_ci(need)) != std::string::npos;
    }

    // Word-order-independent match for the item search: every whitespace-separated
    // token of `query` must appear (case/accent-insensitive substring) somewhere in
    // `hay`. So "Claw Talisman", "Talisman Claw" and "griffe claw" all match the same
    // marker when `hay` is its combined game-language + English text. Empty query (no
    // tokens) returns false — callers guard the empty box separately.
    bool matches_all_tokens(const std::string &hay, const char *query)
    {
        const std::string fh = fold_ci(hay.c_str());
        bool any = false;
        for (const char *p = query; *p;)
        {
            while (*p == ' ' || *p == '\t') ++p;
            const char *start = p;
            while (*p && *p != ' ' && *p != '\t') ++p;
            if (p == start) break;
            any = true;
            if (fh.find(fold_ci(std::string(start, p - start).c_str())) == std::string::npos)
                return false;
        }
        return any;
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
    // Item-search nav window: while > 0, a locate/page-switch is in flight and the input hooks inject a
    // tiny net-zero mouse jitter so the game keeps processing its world-map (otherwise, with the panel
    // open, input is blanked -> the map's view/page step doesn't run -> our switch+pan only apply once
    // F1 closes). Set on a result click, counted down each Present frame.
    std::atomic<int> g_nav_frames{0};
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
    struct ItemIconSrv { ID3D12Resource *tex = nullptr; D3D12_GPU_DESCRIPTOR_HANDLE gpu{}; bool ok = false; ImVec2 uv0{0, 0}, uv1{1, 1}; };
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

        begin_icon_batch(); // reuse the shared list (reset once/frame)
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

    // ── Grace-sprite GPU debug viewer (dev) ──────────────────────────────────────────
    // Draws EVERY harvested SB_ERR_Grace_* frame as a GPU image so we can visually pick the correct
    // grace rect (the captured 'Morning_Color' frame didn't look like a grace). Uses a FULL-SHEET SRV
    // over the engine resource + per-frame UV (NO copy → shows the exact rect content, isolates a
    // wrong-rect bug from a copy/UV bug). Engine-owned sheet (don't Release); dev/map-open only.
    struct GraceDbg { ImTextureID tex; ImVec2 uv0, uv1; std::string name; int w, h; };
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

    // Dev UX guard: the F1 candidate viewers only fill up once run_force_grace force-creates the
    // mod's MENU_MAP_* grace sprites — and it does that ONLY while grace_overlay + grace_gpu_sprite
    // are BOTH on. Open the viewer with either off (incl. off in the INI) and the list silently
    // stays at the single naturally-drawn SB_ERR_Grace_* frame: the "I don't see my candidates"
    // trap. Show an amber warning pointing at both the checkboxes and the INI keys when the gate
    // is closed. No-op once both are on.
    void grace_candidate_gate_warning()
    {
        if (goblin::config::graceOverlay && goblin::config::graceGpuSprite)
            return;
        ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.15f, 1.0f),
            "(!) Few/no candidates listed?\n"
            "    The forced MENU_MAP_* grace sprites are only created while BOTH\n"
            "    'grace_overlay' AND 'grace_gpu_sprite' are ON. Enable them via the\n"
            "    checkboxes below (Overlay graces / live engine sprite), or set both\n"
            "    = true in MapForGoblins.ini. Otherwise only the live SB_ERR_Grace_*\n"
            "    frame the game happens to draw will appear.");
    }

    // A scale/offset row with BOTH a coarse slider AND a precise input on the same line: drag the
    // slider for a rough value, or type an exact one / click the +/- step arrows of the InputFloat
    // for fine control. Keyboard text entry already works (the WndProc feeds WM_CHAR to ImGui), so
    // no ImGui-init change is needed for typing. The value is clamped to [lo,hi] (the slider range).
    // Returns true if the value changed this frame. (Tip: Ctrl+Click the slider itself also types.)
    bool scale_control(const char *label, float *v, float lo, float hi,
                       float step, float step_fast, const char *fmt)
    {
        bool changed = false;
        ImGui::PushID(label);
        ImGui::SetNextItemWidth(150.0f);
        changed |= ImGui::SliderFloat("##slider", v, lo, hi, fmt);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        changed |= ImGui::InputFloat("##input", v, step, step_fast, fmt);
        if (*v < lo) *v = lo;
        if (*v > hi) *v = hi;
        ImGui::SameLine();
        ImGui::TextUnformatted(label);
        ImGui::PopID();
        return changed;
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
            // ER drives the 2D map camera off GetCursorPos. While a search-locate / page-switch is in
            // flight, jitter the reported cursor ±1px (net zero) so the game keeps STEPPING its map
            // (the per-frame c32f0 step, where our locate reticle-write + page switch apply) with the F1
            // panel still open. 1px = no drift. (Centring itself is driven on the game thread via the
            // c32f0 reticle-target write, not from this cursor position.)
            if (g_nav_frames.load(std::memory_order_relaxed) > 0)
            {
                static int s_cjit = 0;
                s_cjit ^= 1;
                p->x += s_cjit ? 1 : -1;
            }
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
                // During an item-search nav, feed a 1px net-zero (±1 alternating) delta so the game
                // keeps stepping/rendering its world map (which is otherwise frozen by the input blank)
                // — lets our page/layer switch + pan actually take effect with the F1 panel open. The
                // map cursor jitters 1px (negligible, nets to zero); clicks/keys stay suppressed.
                if (g_nav_frames.load(std::memory_order_relaxed) > 0)
                {
                    static int s_jit = 0;
                    s_jit ^= 1;
                    ri->data.mouse.lLastX = s_jit ? 1 : -1;
                    ri->data.mouse.lLastY = 0;
                }
                else
                {
                    ri->data.mouse.lLastX = 0;
                    ri->data.mouse.lLastY = 0;
                }
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
        // F1 panel CLOSED but the world map is open: feed ImGui the mouse so in-world chips
        // (region toggles) stay clickable, and consume the L-button PRESS for the game ONLY
        // when the cursor is over a chip (map pan/select elsewhere is untouched). Releases
        // always pass through to the game (never swallow an UP → no "held forever" bug).
        else if (goblin::world_map_open())
        {
            switch (msg)
            {
            case WM_MOUSEMOVE:
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
                ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
                if (msg == WM_LBUTTONDOWN && goblin::worldmap::inworld_hovered())
                    return 1; // chip ate the click; the game must not pan/select
                break;
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
        namespace wm = goblin::worldmap;
        if (!goblin::ui::icons_enabled() && !wm::item_search_active())
            return; // master off (and no active search) → draw no overlay markers
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
        if (ensure_grace_dungeon_srv())
            wm::set_grace_dungeon_sprite(reinterpret_cast<void *>(g_grace_dgn_gpu.ptr),
                                         g_grace_dgn_uv0.x, g_grace_dgn_uv0.y, g_grace_dgn_uv1.x, g_grace_dgn_uv1.y);
        wm::render_markers(s_layers, atlas, mx, my);
        // Item-search "locate": centre the live map on the located marker. The engine re-derives the
        // pan from the cursor reticle every frame (a Present-thread pan write is reverted), so the actual
        // centring is driven on the GAME thread inside the c32f0 step hook: set_locate_target() hands it
        // the marker, and the hook writes the reticle target each frame just before the engine's easer
        // pans toward it. HELD over a window of frames (the freeze re-asserts the static view), and the
        // nav jitter is kept alive so the c32f0 step keeps running with F1 open. set_view_center writes
        // the ZOOM-in + the debug snapshot.
        static float s_hold_u = 0.f, s_hold_v = 0.f;
        static int s_hold_frames = 0;
        static int s_settle_hits = 0;
        float lu = 0.f, lvv = 0.f;
        if (wm::take_locate_pos(&lu, &lvv)) { s_hold_u = lu; s_hold_v = lvv; s_hold_frames = 90; s_settle_hits = 0; }
        if (s_hold_frames > 0)
        {
            // Zoom in if the view is too far-out (else the item is a speck). kLocateZoom is calibration
            // (map zoom ~0.05..1.0; kGraceZoomRef in map_renderer is 0.25 "mid") — bump if still too wide.
            constexpr float kLocateZoom = 0.5f;
            goblin::worldmap_probe::set_view_center(s_hold_u, s_hold_v, kLocateZoom);
            goblin::worldmap_probe::set_locate_target(s_hold_u, s_hold_v); // game-thread c32f0 centres on it
            // Keep the map STEPPING (c32f0 runs) with F1 open: the per-frame step is gated on perceived
            // input, so keep the nav jitter alive for the whole hold.
            if (g_nav_frames.load(std::memory_order_relaxed) < s_hold_frames)
                g_nav_frames.store(s_hold_frames, std::memory_order_relaxed);
            --s_hold_frames;
            // EARLY RELEASE (perf): each hold-frame forces ER to step + re-composite its WHOLE Scaleform
            // world map — that engine cost (~tens of ms, NOT our ~0.1ms render) is the FPS drop after a
            // locate click. The 90-frame cap is only a fallback for a slow/far pan; the moment the live
            // view CONVERGES on the target (centre within a few map-units, 2 frames running to skip a
            // mid-ease false hit) we cut the hold to a short settle so the stepping stops early.
            constexpr int kSettle = 3;
            constexpr float kConvergeEps2 = 1024.f;  // ~32 marker-units off the screen centre
            goblin::worldmap_probe::LiveView lv2{};
            if (s_hold_frames > kSettle && goblin::worldmap_probe::get_live_view(lv2) && lv2.zoom > 0.f)
            {
                const float du = (lv2.panX + lv2.snapMidX) / lv2.zoom - s_hold_u;
                const float dv = (lv2.panZ + lv2.snapMidZ) / lv2.zoom - s_hold_v;
                if (du * du + dv * dv < kConvergeEps2)
                {
                    if (++s_settle_hits >= 2) s_hold_frames = kSettle;
                }
                else
                    s_settle_hits = 0;
            }
            if (s_hold_frames == 0)
                goblin::worldmap_probe::clear_locate_target(); // release the map (mouse pan resumes)
        }
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
        if (ensure_grace_dungeon_srv())
            goblin::worldmap::set_grace_dungeon_sprite(reinterpret_cast<void *>(g_grace_dgn_gpu.ptr),
                                                       g_grace_dgn_uv0.x, g_grace_dgn_uv0.y, g_grace_dgn_uv1.x, g_grace_dgn_uv1.y);
        goblin::worldmap::draw_minimap(overlay_layers(), atlas, io.DisplaySize.x,
                                       io.DisplaySize.y);
    }

    void draw_panel()
    {
        ImGuiIO &io = ImGui::GetIO();

        // Disk-loot maps definitively not found (ancestor-walk AND the CreateFileW
        // observer came up empty within the timeout) → the disk source is REQUIRED
        // when loot_from_disk_msb is on, so replace the whole panel with a red error
        // instead of drawing an empty/misleading map.
        if (goblin::worldmap::disk_loot_state() == goblin::worldmap::DiskLootState::Failed)
        {
            ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
            ImGui::Begin("Map for Goblins##error", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.25f, 0.25f, 1.0f));
            ImGui::TextUnformatted("Map for Goblins - ERROR");
            ImGui::Separator();
            ImGui::TextWrapped("The mod's map folder (map\\MapStudio\\*.msb.dcx) could not be "
                               "found. The mod cannot load its markers.");
            ImGui::PopStyleColor();
            ImGui::Spacing();
            std::string sd = goblin::worldmap::disk_loot_dir().string();
            if (!sd.empty())
                ImGui::TextDisabled("Last path searched: %s", sd.c_str());
            ImGui::TextDisabled("Set 'loot_msb_dir' in MapForGoblins.ini to your mod's");
            ImGui::TextDisabled("map\\MapStudio folder (the markers are read from it live).");
            ImGui::End();
            return;
        }

        // One or more RE signatures (AOB) failed to resolve uniquely at init → the mod
        // hooked the wrong function or nothing at all, so markers/graces/loot will be
        // wrong or absent. Surface it instead of silently rendering a broken map (the
        // [SIG] log is invisible mid-game). MULTI = ambiguous (likely-wrong function),
        // FAIL = gone (needs re-find after a game update).
        {
            const goblin::sig::SigHealth &sh = goblin::sig::sig_health();
            if (sh.ran && (sh.fail > 0 || sh.multi > 0))
            {
                ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
                ImGui::Begin("Map for Goblins##sigerror", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.15f, 1.0f));
                ImGui::TextUnformatted("Map for Goblins - WARNING");
                ImGui::Separator();
                ImGui::TextWrapped("The RE signatures were not resolved correctly. The mod "
                                   "will probably not work correctly (markers, graces or "
                                   "loot missing or wrong).");
                ImGui::PopStyleColor();
                ImGui::Spacing();
                ImGui::TextDisabled("%d missing, %d ambiguous out of %d signatures.",
                                    sh.fail, sh.multi, sh.total);
                ImGui::TextDisabled("Likely broken by a game update — see the [SIG] log");
                ImGui::TextDisabled("for details (the AOBs need to be re-found).");
                ImGui::End();
                return;
            }
        }

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
                    // The copied tex is the 4-block-snapped region; crop to the exact icon via UVs.
                    auto sit = g_item_icon_srvs.find(id);
                    ImVec2 uv0 = sit != g_item_icon_srvs.end() ? sit->second.uv0 : ImVec2(0, 0);
                    ImVec2 uv1 = sit != g_item_icon_srvs.end() ? sit->second.uv1 : ImVec2(1, 1);
                    ImGui::Image(reinterpret_cast<ImTextureID>(h), ImVec2(48, 48), uv0, uv1);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("iconId %d", id);
                }
                if (!drawn)
                    ImGui::TextDisabled(ids.empty() ? "no icons harvested yet — open inventory first"
                                                    : "harvested icons not ready yet (1-frame batch)...");

                // DEV force-load test (findings §5b): stream a TPF resident by FD4 path via CSFile.
                // Type a path + click → watch [FORCELOAD] in the log (handle != 0 = loaded) and whether
                // the sheet becomes resident. e.g. "menu:/00_Solo.tpf", "menutpfbnd:/00_Solo/<name>.tpf".
                ImGui::Separator();
                static char s_fl_path[192] = "menu:/00_Solo.tpf";
                ImGui::SetNextItemWidth(260);
                ImGui::InputText("##flpath", s_fl_path, sizeof(s_fl_path));
                ImGui::SameLine();
                if (ImGui::Button("Force-load (CSFile)"))
                    goblin::force_load_file(s_fl_path);
                // One-click sweep of the menu resource-GROUP BNDs (re_v110): logs a [FORCELOAD] line per
                // path. Try with the map/inventory CLOSED so a non-resident group's load is visible
                // (handle != 0 + harvested grows after you then open the inventory).
                if (ImGui::Button("Test: force-load common groups"))
                {
                    static const char *kGroups[] = {
                        "menu:/01_Common.tpfbhd", "menu:/01_Common.tpf",
                        "menu:/00_Solo.tpfbhd",   "menu:/03_ChrMake.tpfbhd",
                        "menu:/71_MapTile.tpfbhd", "menu:/02_Title.tpfbhd",
                    };
                    for (const char *p : kGroups) goblin::force_load_file(p);
                }
                // Map-point icon rects (MENU_MAP_* → iconId→rect) are harvested from the resident image
                // repo by the repo walk when the world map opens — no force-load needed.
                ImGui::Text("map-icon layout entries: %zu", goblin::map_icon_layout_count());

                // ── Bind-flip test (findings §5e) — does +0x7c flip trigger the per-icon bind? ──
                // Recommended sequence (watch [BINDTEST] in the log, then "harvested:" above):
                //   0) open inventory/map ONCE so the ticker captures the manager.
                //   1) "Dump groups" — logs each loaded group's apply-vmethod RVA + flags.
                //   2) "Load files (gid)" — streams the group's TPF+sblytbnd resident via the by-id loaders.
                //   3) "Flip-bind all" — sets +0x7c on every loaded group → engine re-applies this tick.
                //   4) re-check "harvested:" — if it JUMPS for un-browsed items, the flag IS the bind lever.
                ImGui::Separator();
                ImGui::TextDisabled("Bind-flip test (§5e): open inventory once, then ->");
                static int s_bt_gid = 1;   // 1 = 01_Common (item-icon group)
                ImGui::SetNextItemWidth(120);
                ImGui::InputInt("group id", &s_bt_gid);
                if (s_bt_gid < 0) s_bt_gid = 0; if (s_bt_gid > 8) s_bt_gid = 8;
                if (ImGui::Button("1) Dump groups"))       goblin::bind_test(1, s_bt_gid);
                ImGui::SameLine();
                if (ImGui::Button("2) Load files (gid)"))  goblin::bind_test(2, s_bt_gid);
                if (ImGui::Button("3) Flip-bind all"))      goblin::bind_test(3, s_bt_gid);
                ImGui::SameLine();
                if (ImGui::Button("4) Load + flip (gid)")) goblin::bind_test(4, s_bt_gid);

                // Force-CreateImage (§5g): replay the GFx per-image bind callback for one item icon.
                // Watch [CREATEIMG] in the log: live names while browsing, then the forced result +
                // whether "harvested:" grows. Open inventory once first so the context is captured.
                ImGui::Separator();
                ImGui::TextDisabled("Force-bind one icon via CreateImage (§5g):");
                static int s_ci_icon = 0;
                ImGui::SetNextItemWidth(120);
                ImGui::InputInt("iconId##ci", &s_ci_icon);
                ImGui::SameLine();
                if (ImGui::Button("Force CreateImage")) goblin::force_create_icon(s_ci_icon);
                if (ImGui::Button("Replay last live symbol (control)")) goblin::force_create_last();
                if (ImGui::Button("Force graces now")) goblin::force_graces();
                ImGui::SameLine();
                ImGui::TextDisabled("(re-harvest MENU_MAP_*/SB_ERR_Grace_* on demand)");

                // LOAD DIRECT: take ER's OWN already-loaded sheet resource (the harvested grace
                // sheet's ID3D12Resource) and copy the WHOLE sheet straight into our own texture.
                // No file, no DCX — the game's resident GPU image, grabbed directly.
                ImGui::Separator();
                ImGui::TextDisabled("Copy ER's loaded sheet DIRECT into our texture:");
                static UINT64 s_dir_tex = 0; static int s_dir_w = 0, s_dir_h = 0;
                if (ImGui::Button("Load ER image direct"))
                    s_dir_tex = copy_er_sheet_direct(s_dir_w, s_dir_h);
                if (s_dir_tex)
                {
                    ImGui::Text("%dx%d (our copy of ER's sheet)", s_dir_w, s_dir_h);
                    float dw = s_dir_w > 320 ? 320.f : (float)s_dir_w;
                    float dh = s_dir_h ? dw * s_dir_h / s_dir_w : dw;
                    ImGui::Image((ImTextureID)s_dir_tex, ImVec2(dw, dh));
                }

                // LOAD FROM RAM: upload the DDS the Oodle hook captured in the game's decompressed
                // RAM (FD4 cache) into our own texture — no game GPU bind, mod-agnostic. Proves the
                // whole chain: DCX→oodle→TPF(RAM)→our SRV→draw.
                // BROWSE every DDS we grabbed from ER's decompresses (RAM, no game bind). Flip through
                // them to find the icon sheet. Each "Show" uploads that DDS into our own texture.
                ImGui::Separator();
                size_t ddsN = goblin::tpf_dds_count();
                ImGui::TextDisabled("Captured ER DDS in RAM: %zu (browse to find the icon sheet)", ddsN);
                static int s_idx = 0;
                static UINT64 s_ram_tex = 0; static int s_ram_w = 0, s_ram_h = 0, s_ram_fmt = 0;
                if (ddsN)
                {
                    if (s_idx >= (int)ddsN) s_idx = (int)ddsN - 1;
                    ImGui::SetNextItemWidth(180);
                    ImGui::SliderInt("##ddsidx", &s_idx, 0, (int)ddsN - 1);
                    ImGui::SameLine(); if (ImGui::SmallButton("<") && s_idx > 0) --s_idx;
                    ImGui::SameLine(); if (ImGui::SmallButton(">") && s_idx < (int)ddsN - 1) ++s_idx;
                    ImGui::SameLine();
                    if (ImGui::Button("Show"))
                    {
                        static std::vector<uint8_t> dds;
                        if (goblin::tpf_dds_at((size_t)s_idx, dds))
                        {
                            DXGI_FORMAT f;
                            s_ram_tex = create_tex_from_dds_mem(dds.data(), dds.size(), s_ram_w, s_ram_h, f);
                            s_ram_fmt = (int)f;
                        }
                    }
                }
                if (s_ram_tex)
                {
                    ImGui::Text("#%d  %dx%d fmt=%d (ER RAM, no bind)", s_idx, s_ram_w, s_ram_h, s_ram_fmt);
                    float dw = s_ram_w > 320 ? 320.f : (float)s_ram_w;
                    float dh = s_ram_h ? dw * s_ram_h / s_ram_w : dw;
                    ImGui::Image((ImTextureID)s_ram_tex, ImVec2(dw, dh));
                }
            }

            // Icon migration (Baked → GPU): a LIVE three-way census of what each category ACTUALLY
            // draws RIGHT NOW — GPU icon (a real native engine sprite), Atlas (the baked PNG cell), or
            // Circle (no cell → coloured circle). This mirrors map_renderer's IconSet::resolve order,
            // including live residency (the GPU path only "wins" once its sprite is harvested), so the
            // panel shows the truth — not just what's WIRED. Lets us verify the per-category item icon
            // (00_Solo atlas) is correct before committing to the group-load that makes it resident.
            if (ImGui::CollapsingHeader("Icon migration (Baked \xE2\x86\x92 GPU)"))
            {
                namespace wm = goblin::worldmap;
                const bool native_on = goblin::config::nativeItemIcons;
                const float SZ = 22.f;
                const ImVec4 GREEN(0.40f, 0.85f, 0.45f, 1), YEL(0.90f, 0.80f, 0.35f, 1),
                             GRAY(0.62f, 0.62f, 0.62f, 1);
                // Runtime native sprite (tex+UV) for a category, using the SAME overlay backends the
                // renderer draws with → the panel shows the EXACT runtime icon. false = none right now.
                auto runtime_tex = [&](int c, ImTextureID &tex, ImVec2 &uv0, ImVec2 &uv1,
                                       const char *&via) -> bool {
                    via = ""; if (!native_on) return false;
                    void *t = nullptr; float u0, v0, u1, v1; bool ok = false;
                    if (const char *sym = wm::category_gpu_icon_name(c))
                        ok = goblin::overlay::native_map_point_icon_by_name(sym, t, u0, v0, u1, v1), via = "name symbol";
                    if (!ok) if (int iid = wm::category_gpu_iconId(c))
                        ok = goblin::overlay::native_map_point_icon(iid, t, u0, v0, u1, v1), via = "map-point id";
                    if (!ok) if (int rep = wm::category_rep_icon(c))
                        ok = goblin::overlay::native_item_icon(rep, t, u0, v0, u1, v1), via = "item icon";
                    if (!ok) { via = ""; return false; }
                    tex = reinterpret_cast<ImTextureID>(t); uv0 = ImVec2(u0, v0); uv1 = ImVec2(u1, v1);
                    return true;
                };
                // Baked atlas UV for a category's cell (mirror map_renderer's icon_uv). false = no cell.
                auto baked_uv = [&](int c, ImVec2 &uv0, ImVec2 &uv1) -> bool {
                    using namespace goblin::overlay_icons;
                    const char *key = wm::category_icon_key(c);
                    if (!key) return false;
                    for (int i = 0; i < ICON_CELL_COUNT; ++i)
                        if (std::strcmp(ICON_CELLS[i].key, key) == 0)
                        {
                            const IconCell &cell = ICON_CELLS[i];
                            uv0 = ImVec2((cell.col * CELL) / (float)ATLAS_W, (cell.row * CELL) / (float)ATLAS_H);
                            uv1 = ImVec2(((cell.col + 1) * CELL) / (float)ATLAS_W, ((cell.row + 1) * CELL) / (float)ATLAS_H);
                            return true;
                        }
                    return false;
                };
                const bool grace_gpu = native_on && goblin::config::graceOverlay && goblin::config::graceGpuSprite;
                const int total = wm::category_count();
                struct Row { int s; const char *via; bool hasRt; ImTextureID rt; ImVec2 ra, rb; };
                std::vector<Row> rows(total);
                int gpu = 0, atlas = 0, circle = 0;
                for (int c = 0; c < total; ++c)
                {
                    Row &r = rows[c];
                    r.hasRt = runtime_tex(c, r.rt, r.ra, r.rb, r.via);
                    if (r.hasRt) r.s = 2;
                    else if (grace_gpu && c == static_cast<int>(goblin::generated::Category::WorldGraces))
                    { r.s = 2; r.via = "grace sprite"; }
                    else r.s = wm::category_has_baked_icon(c) ? 1 : 0;
                    switch (r.s) { case 2: ++gpu; break; case 1: ++atlas; break; default: ++circle; }
                }
                ImGui::Text("Live render source (of %d categories)%s:", total,
                            native_on ? "" : "  [native_item_icons OFF]");
                ImGui::TextColored(GREEN, "  GPU icon : %d", gpu);
                ImGui::SameLine(); ImGui::TextColored(YEL, "   Atlas : %d", atlas);
                ImGui::SameLine(); ImGui::TextColored(GRAY, "   Circle : %d", circle);
                ImGui::ProgressBar(total ? (float)gpu / total : 0.f, ImVec2(-1, 0));
                ImGui::TextDisabled("A/B: [baked]  [runtime]   category  (source)");
                ImGui::Separator();
                if (ImGui::BeginChild("iconmig_census", ImVec2(0, 320), true))
                {
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    for (int c = 0; c < total; ++c)
                    {
                        const Row &r = rows[c];
                        const char *name = goblin::ui::category_label(c);
                        // [baked] thumbnail (atlas cell, or a coloured circle for the no-cell categories).
                        ImVec2 bu0, bu1, p = ImGui::GetCursorScreenPos();
                        if (g_atlas_ready && baked_uv(c, bu0, bu1))
                            ImGui::Image(reinterpret_cast<ImTextureID>(g_atlas_gpu.ptr), ImVec2(SZ, SZ), bu0, bu1);
                        else
                        {
                            ImGui::Dummy(ImVec2(SZ, SZ));
                            dl->AddCircleFilled(ImVec2(p.x + SZ / 2, p.y + SZ / 2), SZ * 0.34f,
                                                goblin::worldmap::category_color(c));
                        }
                        ImGui::SameLine();
                        // [runtime] thumbnail (the real native sprite) or an empty slot if not resident.
                        ImVec2 q = ImGui::GetCursorScreenPos();
                        if (r.hasRt)
                            ImGui::Image(r.rt, ImVec2(SZ, SZ), r.ra, r.rb);
                        else
                        {
                            ImGui::Dummy(ImVec2(SZ, SZ));
                            dl->AddRect(q, ImVec2(q.x + SZ, q.y + SZ), IM_COL32(110, 110, 110, 90));
                        }
                        ImGui::SameLine();
                        ImGui::AlignTextToFramePadding();
                        const ImVec4 col = r.s == 2 ? GREEN : r.s == 1 ? YEL : GRAY;
                        const bool wired = wm::category_is_gpu_native(c);
                        if (r.s == 2)
                            ImGui::TextColored(col, "%s  (%s)", name ? name : "?", r.via);
                        else
                            ImGui::TextColored(col, "%s%s", name ? name : "?",
                                               (native_on && wired) ? "  (wired, not resident)" : "");
                    }
                }
                ImGui::EndChild();
            }

            // Grace-sprite GPU debug: draw every harvested SB_ERR_Grace_* frame (full-sheet SRV +
            // UV, no copy) so we can visually pick the correct grace rect. Open the world map (with a
            // discovered grace) to populate. The frame that looks like a Site of Grace = the one to lock.
            if (goblin::config::dumpIconTextures && ImGui::CollapsingHeader("ERR map sprites (GPU debug)"))
            {
                ensure_grace_debug();
                grace_candidate_gate_warning();
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

            // Grace texture DEBUG (live format/swizzle/source) — verify the active grace mapped the
            // right NAME and the right COLORING. Tweak below + "Re-apply" re-copies the SRV live.
            if (goblin::config::dumpIconTextures && ImGui::CollapsingHeader("Grace texture debug (live)"))
            {
                // The active grace, drawn big (this IS what the map markers use).
                if (g_grace_state == 1)
                {
                    ImGui::Text("active grace  fmt=%d (98=BC7_UNORM 99=BC7_SRGB)", g_grace_dbg_fmt_used);
                    ImGui::Image(reinterpret_cast<ImTextureID>(g_grace_gpu.ptr), ImVec2(128, 128),
                                 g_grace_uv0, g_grace_uv1);
                    ImGui::SameLine();
                    // Full copied texture (no UV crop) so a wrong-rect vs wrong-color bug is separable.
                    ImGui::Image(reinterpret_cast<ImTextureID>(g_grace_gpu.ptr), ImVec2(128, 128));
                    ImGui::TextDisabled("left = UV-cropped (marker) | right = full copied block");
                }
                else ImGui::TextDisabled("grace not ready (open map near a grace)");

                bool dirty = false;
                ImGui::Text("sRGB:");
                ImGui::SameLine(); dirty |= ImGui::RadioButton("auto##gs", &g_grace_dbg_srgb, 0);
                ImGui::SameLine(); dirty |= ImGui::RadioButton("UNORM(98)##gs", &g_grace_dbg_srgb, 1);
                ImGui::SameLine(); dirty |= ImGui::RadioButton("SRGB(99)##gs", &g_grace_dbg_srgb, 2);
                ImGui::Text("channels:");
                ImGui::SameLine(); dirty |= ImGui::RadioButton("RGBA##gz", &g_grace_dbg_swiz, 0);
                ImGui::SameLine(); dirty |= ImGui::RadioButton("R<->B##gz", &g_grace_dbg_swiz, 1);
                ImGui::SameLine(); dirty |= ImGui::RadioButton("R<->G##gz", &g_grace_dbg_swiz, 2);
                ImGui::SameLine(); dirty |= ImGui::RadioButton("A=1##gz", &g_grace_dbg_swiz, 3);
                if (ImGui::Button("Re-apply (rebuild grace SRV)") || dirty)
                    force_rebuild_grace();

                // Source picker: which captured candidate is the active grace (test "the right name").
                ImGui::Separator();
                ImGui::TextDisabled("source candidate (click to use as grace):");
                grace_candidate_gate_warning();
                std::vector<goblin::GraceCandidate> cands = goblin::grace_candidates();
                for (size_t i = 0; i < cands.size(); ++i)
                {
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::SmallButton("use")) { goblin::set_grace_from_candidate(i); force_rebuild_grace(); }
                    ImGui::SameLine();
                    ImGui::Text("%s  (%d,%d)-(%d,%d)", cands[i].name.c_str(), cands[i].spr.x0,
                                cands[i].spr.y0, cands[i].spr.x1, cands[i].spr.y1);
                    ImGui::PopID();
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

            // Map-fragment gate (live; persists via "Save to INI"). When on, a marker stays hidden
            // until the player has acquired that area's map-fragment ITEM (fragment event flag).
            ImGui::Checkbox("Require map fragments (hide an area's icons until its fragment is found)",
                            &goblin::config::requireMapFragments);

            // DIAG: draw ONLY the no-bake residual (Baked-source markers; disk/live twins already
            // deduped away). Fly the world + eyeball each spot — real loot the live pass misses
            // (coverage gap) vs a phantom the bake invented (bake bug). See nobake_scoreboard.md.
            ImGui::Checkbox("Baked-only (diag: show just the no-bake residual)",
                            &goblin::config::bakedOnly);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Hides every marker the live disk/memory passes already cover,\n"
                                  "leaving only the markers still coming from the static bake.\n"
                                  "Use it to judge each leftover: real loot we fail to source live\n"
                                  "(coverage miss) vs a stale/invented spot (bake bug).");

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
                scale_control("Master", &goblin::config::overlayMasterScale, 0.3f, 3.0f, 0.05f, 0.25f, "%.2f");
                scale_control("Category icons", &goblin::config::overlayIconScale, 0.3f, 10.0f, 0.05f, 0.25f, "%.2f");
                scale_control("Grace icons (calib)", &goblin::config::graceIconScale, 0.2f, 10.0f, 0.05f, 0.25f, "%.2f");
                scale_control("Grace offset X (native vs imgui)", &goblin::config::graceOffsetX, -200.0f, 200.0f, 1.0f, 10.0f, "%.0f");
                scale_control("Grace offset Y (native vs imgui)", &goblin::config::graceOffsetY, -200.0f, 200.0f, 1.0f, 10.0f, "%.0f");
                scale_control("Cluster piles", &goblin::config::overlayClusterScale, 0.3f, 3.0f, 0.05f, 0.25f, "%.2f");
                if (ImGui::SmallButton("Reset##scale"))
                {
                    goblin::config::overlayMasterScale = 1.0f;
                    goblin::config::overlayIconScale = 1.2f;     // match the schema defaults
                    goblin::config::overlayClusterScale = 1.0f;
                    goblin::config::graceIconScale = 1.2f;
                    goblin::config::graceOffsetX = 0.0f;
                    goblin::config::graceOffsetY = 0.0f;
                }
                ImGui::TextDisabled("Slider = coarse; type in the box or use its +/- arrows for an exact\n"
                                    "value (Ctrl+Click the slider also types). × a resolution-relative\n"
                                    "base. Save to INI to persist.");
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

            // ── Find item / object ────────────────────────────────────────────────────────────
            // Search markers by NAME: type a fragment, get a results list; the matching markers are
            // ringed on the map (and pulled out of cluster piles). Click a result to point the map
            // cursor at it (cursor = the 2D map camera). The match set is rebuilt only when the query
            // changes (resolving ~thousands of FMG names per frame would be too costly), so the hot
            // render loop just does an O(1) name_id set lookup.
            ImGui::SeparatorText("Find item / object");
            {
                // group bits (marker_layer): bit0 = underground, bit1 = DLC. Label the page so the
                // user knows where a hit is — locate only pans WITHIN the open page (cross-page needs
                // a manual page switch first; auto-switch would need page-transition RE).
                auto page_label = [](int g) -> const char * {
                    switch (g & 3) { case 1: return "Underground"; case 2: return "DLC";
                                     case 3: return "DLC Underground"; default: return "Overworld"; }
                };
                // One result row per (name, PAGE): an item on several pages (e.g. a Larval Tear on
                // Overworld + Underground + DLC) gets a separate row per page, so clicking a row locates
                // on THAT page. (Deduping by name_id alone collapsed them into one row carrying only the
                // first marker's group — the "shows only Underground" bug.)
                struct Hit { std::string label; int32_t name_id; int count; int group; };
                static char item_q[64] = "";
                static std::string s_last_q;
                static std::unordered_set<int32_t> s_match;   // name_ids whose name matches (rendered ring)
                static std::vector<Hit> s_hits;               // deduped results for the list
                static int32_t s_pending_locate = 0;
                static std::string s_locate_label;            // clicked item name (pending banner)
                static int s_locate_group = 0;                // clicked item page (pending banner)

                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                ImGui::InputTextWithHint("##itemsearch", "find item/object by name... (e.g. larval, bolt of)",
                                         item_q, sizeof(item_q));

                // ── Locate debug (dev) — gated behind Verbose logging (debug_logging). Diagnoses the
                // item-search centring: LIVE view vs the target the engine should ease the pan toward
                // ("live-vs-target dPan" → CENTERED OK once converged). Kept for future locate issues.
                if (goblin::config::debugLogging && ImGui::TreeNode("Locate debug (dev)"))
                {
                    goblin::worldmap_probe::LiveView dlv{};
                    const bool dbg_open = goblin::worldmap_probe::get_live_view(dlv);
                    const auto &d = goblin::worldmap_probe::last_locate_debug();
                    if (dbg_open && dlv.zoom != 0.f)
                    {
                        const float cu = (dlv.panX + dlv.snapMidX) / dlv.zoom;
                        const float cv = (dlv.panZ + dlv.snapMidZ) / dlv.zoom;
                        ImGui::Text("LIVE pan=(%.1f, %.1f) zoom=%.3f  centre(marker)=(%.1f, %.1f)",
                                    dlv.panX, dlv.panZ, dlv.zoom, cu, cv);
                        ImGui::Text("LIVE snapMid=(%.1f, %.1f)", dlv.snapMidX, dlv.snapMidZ);
                    }
                    else
                        ImGui::TextDisabled("map closed / no live view (open the world map)");
                    ImGui::Separator();
                    if (!d.ran)
                        ImGui::TextDisabled("no locate yet — click a result");
                    else
                    {
                        ImGui::Text("cursorOk=%d  wrote=%d  rectOk=%d", d.cursorOk, d.wrote, d.rectOk);
                        ImGui::Text("req centre   = (%.1f, %.1f)", d.reqU, d.reqV);
                        ImGui::TextColored(d.clamped ? ImVec4(1, 0.7f, 0.2f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1),
                                           "clamp centre = (%.1f, %.1f)  %s", d.clampU, d.clampV,
                                           d.clamped ? "[CLAMPED -> near edge]" : "[no clamp]");
                        ImGui::Text("zoom %.3f -> %.3f   world=[%.0f,%.0f .. %.0f,%.0f]",
                                    d.zoomBefore, d.zoomUsed, d.wMinX, d.wMinZ, d.wMaxX, d.wMaxZ);
                        ImGui::Text("pan TARGET   = (%.1f, %.1f)  (driven via cursor, not written)",
                                    d.panWroteX, d.panWroteZ);
                        if (dbg_open)
                        {
                            const float dx = dlv.panX - d.panWroteX, dz = dlv.panZ - d.panWroteZ;
                            const bool off = (dx > 8.f || dx < -8.f || dz > 8.f || dz < -8.f);
                            ImGui::TextColored(off ? ImVec4(1, 0.7f, 0.2f, 1) : ImVec4(0.3f, 1, 0.3f, 1),
                                               "live-vs-target dPan=(%.1f, %.1f)  %s", dx, dz,
                                               off ? "<- converging / not there yet" : "<- CENTERED OK");
                        }
                    }
                    ImGui::TreePop();
                }

                if (s_last_q != item_q)
                {
                    s_last_q = item_q;
                    s_match.clear();
                    s_hits.clear();
                    if (item_q[0] != '\0')
                    {
                        // Resolve each distinct name_id once (cache), substring-match the query
                        // against BOTH the live (game-language) label AND the bundled English
                        // alias — so a player on a French/other game can type the English/wiki
                        // name and still find it. The displayed label stays the game-language
                        // name, with the English in parens when it differs (e.g. on a non-EN
                        // game) to confirm the match.
                        struct Names { std::string loc, en, label; };
                        std::unordered_map<int32_t, Names> name_cache;
                        // Dedup result rows by (name_id, page) via a composite key (name_id<<2 | group),
                        // so each page an item is on becomes its own row. s_match stays keyed by name_id
                        // so EVERY instance still highlights/rings on the map.
                        std::map<int64_t, int> hit_count;  // (name_id<<2 | group) -> marker count
                        for (auto *L : overlay_layers())
                        {
                            if (!L) continue;
                            for (const auto &m : L->markers())
                            {
                                if (m.name_id < 0) continue;
                                auto it = name_cache.find(m.name_id);
                                if (it == name_cache.end())
                                {
                                    Names n;
                                    n.loc = goblin::lookup_text_utf8(m.name_id);
                                    n.en = goblin::lookup_name_alias_en_utf8(m.name_id);
                                    // Label = game-language name; fall back to English if the
                                    // live FMG had no entry. Append "(English)" only when it adds
                                    // information (present and different from the shown name).
                                    n.label = n.loc.empty() ? n.en : n.loc;
                                    if (!n.en.empty() && n.en != n.label)
                                        n.label += " (" + n.en + ")";
                                    it = name_cache.emplace(m.name_id, std::move(n)).first;
                                }
                                const Names &nm = it->second;
                                if (nm.label.empty()) continue;
                                // Word-order-independent: each query token must appear in the
                                // combined game-language + English text (so "Claw Talisman" and
                                // "Talisman Claw" both match, and FR/EN words can be mixed).
                                if (!matches_all_tokens(nm.loc + " " + nm.en, item_q)) continue;
                                const int g = m.group & 3;
                                s_match.insert(m.name_id);                       // ring every instance
                                const int64_t k = ((int64_t)m.name_id << 2) | g; // per (name, page)
                                if (++hit_count[k] == 1)
                                    s_hits.push_back({nm.label, m.name_id, 0, g});
                            }
                        }
                        for (auto &h : s_hits)
                            h.count = hit_count[((int64_t)h.name_id << 2) | h.group];
                        // Force-load each matched item's OWN native icon (the 00_Solo item-icon atlas)
                        // so a revealed/located hit draws its real game icon instead of the baked
                        // fallback. Drained on the engine tick; bounded+deduped by queue_force_item_icon.
                        // Only fires on query change (this block), not per frame.
                        if (goblin::config::nativeItemIcons)
                            for (int32_t nid : s_match)
                                goblin::queue_force_item_icon(goblin::item_real_icon_id(nid));
                        // Group same-name rows together, pages in order (Overworld, Underground, DLC).
                        std::sort(s_hits.begin(), s_hits.end(), [](const Hit &a, const Hit &b) {
                            int c = a.label.compare(b.label);
                            return c != 0 ? c < 0 : a.group < b.group;
                        });
                    }
                }

                if (item_q[0] != '\0')
                {
                    // Locate (pan / page-switch) needs the world map OPEN — the live view + the
                    // game-thread switch step only exist then. When closed, the list stays browsable
                    // (what exists + which region) but the rows are disabled with a hint, so a click
                    // can't leave a locate dangling forever.
                    goblin::worldmap_probe::LiveView lv{};
                    const bool map_open = goblin::worldmap_probe::get_live_view(lv);
                    const int open_grp = map_open ? ((lv.openDlc ? 2 : 0) | (lv.underground ? 1 : 0)) : 0;

                    if (map_open)
                        ImGui::TextDisabled("%zu match%s (ringed on map; click = pan map onto it)",
                                            s_hits.size(), s_hits.size() == 1 ? "" : "es");
                    else
                        ImGui::TextColored(ImVec4(1.f, 0.85f, 0.2f, 1.f),
                                           "%zu match%s - open the world map to locate them",
                                           s_hits.size(), s_hits.size() == 1 ? "" : "es");

                    // Region-visited gate from GRACE DISCOVERY (robust, save-backed — supersedes the
                    // fragile dialog availability byte): if NO grace in a region has been rested at, the
                    // player has never been there, so its map is undiscovered — grey those results +
                    // don't teleport in. Covers DLC (bit1) AND underground (bit0). bit1=DLC, bit0=UG.
                    // Recomputed LIVE every throttled tick (NOT latched) so DISCOVERING a new grace mid-
                    // session unlocks its page within ~0.5s, and a save/character switch re-locks it.
                    // There is NO reliable native O(1) "page discovered" flag (the dialog DLC byte
                    // read unreliably; the UG flag was never found — see goblin_worldmap_probe.hpp
                    // TODO(page_og_underground_available)). read_event_flag() IS O(1), though: harvest
                    // the grace discover-flags per page group ONCE (the flag IDs are save-independent —
                    // every grace ROW exists in BonfireWarpParam regardless of discovery), then each
                    // throttled tick read only that small list.
                    // DO NOT LATCH seen=true: these statics outlive the DLL, so a save/character switch
                    // within one game session (graces un-discover) would keep a latched page wrongly
                    // unlocked — the "I rested at no grace yet nothing blocks" bug. We recompute seen
                    // LIVE every tick instead (the list is tiny, so it's free).
                    static bool s_dlc_seen = false, s_ug_seen = false;
                    static std::vector<uint32_t> s_dlc_grace_flags, s_ug_grace_flags;
                    static bool s_grace_flags_built = false;
                    static int s_visit_tick = 0;
                    if (map_open && (++s_visit_tick % 30 == 0))
                    {
                        // Only fires during an active search (map open, 1 tick in 30). The first run
                        // also builds the flag lists (one ~8477-marker pass); steady-state is just a
                        // few-dozen flag reads — the bench line shows both.
                        GOBLIN_BENCH("overlay.item_search.gate_scan");
                        if (!s_grace_flags_built)
                        {
                            for (auto *L : overlay_layers())
                            {
                                if (!L) continue;
                                for (const auto &m : L->markers())
                                {
                                    if (!m.discover_flag) continue;  // graces only carry a discover flag
                                    if (m.group & 2) s_dlc_grace_flags.push_back((uint32_t)m.discover_flag);
                                    if (m.group & 1) s_ug_grace_flags.push_back((uint32_t)m.discover_flag);
                                }
                            }
                            s_grace_flags_built = true;
                        }
                        s_dlc_seen = false;
                        for (uint32_t f : s_dlc_grace_flags)
                            if (goblin::ui::read_event_flag(f)) { s_dlc_seen = true; break; }
                        s_ug_seen = false;
                        for (uint32_t f : s_ug_grace_flags)
                            if (goblin::ui::read_event_flag(f)) { s_ug_seen = true; break; }
                    }
                    if (ImGui::BeginChild("##itemhits", ImVec2(0, 150), true))
                    {
                        if (!map_open) ImGui::BeginDisabled();
                        for (size_t i = 0; i < s_hits.size(); i++)
                        {
                            const Hit &h = s_hits[i];
                            const bool off_page = (h.group & 3) != (open_grp & 3);
                            // Locked = this row's page is a region the player hasn't visited (no grace).
                            const bool locked = map_open && (((h.group & 2) && !s_dlc_seen) ||
                                                             ((h.group & 1) && !s_ug_seen));
                            char row[200];
                            std::snprintf(row, sizeof(row), "%s  (x%d) - %s%s##h%zu", h.label.c_str(),
                                          h.count, page_label(h.group),
                                          locked ? " [undiscovered]" : "", i);
                            if (locked) ImGui::BeginDisabled();
                            if (ImGui::Selectable(row) && map_open)
                            {
                                s_pending_locate = h.name_id;  // click → pan the map onto it
                                s_locate_label = h.label;      // remembered for the pending banner
                                s_locate_group = h.group;      // this row's page
                                g_nav_frames.store(90, std::memory_order_relaxed);  // wake the map so
                                                  // the switch+pan apply with the F1 panel still open
                                // Cross-page: switch to this row's page+layer (overworld<->DLC +
                                // surface<->UG), marshalled onto the game thread, then the locate pans.
                                if (off_page)
                                    goblin::worldmap_probe::request_switch_to_page(h.group);
                            }
                            if (locked) ImGui::EndDisabled();
                            if (map_open && !locked && off_page && ImGui::IsItemHovered())
                                ImGui::SetTooltip("On the %s map — click to switch there + centre on it.",
                                                  page_label(h.group));
                            if (locked && ImGui::IsItemHovered())
                                ImGui::SetTooltip("On the %s map — you haven't discovered it yet.",
                                                  page_label(h.group));
                        }
                        if (!map_open) ImGui::EndDisabled();
                        if (s_hits.empty())
                            ImGui::TextDisabled("no marker matches");
                    }
                    ImGui::EndChild();

                    // Cross-page locate: the switch is marshalled to the game thread + the locate pans
                    // the instant that page opens. The banner shows until it lands.
                    if (map_open && goblin::worldmap::locate_pending())
                        ImGui::TextColored(ImVec4(1.f, 0.85f, 0.2f, 1.f),
                                           "> Locating \"%s\" on the %s map...", s_locate_label.c_str(),
                                           page_label(s_locate_group));
                }
                // Hand the renderer the live match set + any pending locate (consumed once).
                goblin::worldmap::set_item_search(item_q[0] ? &s_match : nullptr, s_pending_locate);
                s_pending_locate = 0;
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
        // GetAsyncKeyState is GLOBAL (focus-independent): without this guard, pressing F1 while the
        // game is alt-tabbed / on another monitor would still toggle the overlay AND activate the
        // cursor hooks (free/freeze the cursor, blank input) off-screen — bug report "F1 offscreen
        // active le hook cursor". Gate BOTH the toggle and the active state on the game window being
        // foreground: the hooks only ever run while you're actually in the game.
        const bool fg = (g_hwnd && GetForegroundWindow() == g_hwnd);
        bool down = fg && (GetAsyncKeyState(static_cast<int>(goblin::config::overlayToggleKey)) & 0x8000) != 0;
        if (down && !g_prev_toggle_down) g_user_show = !g_user_show;
        g_prev_toggle_down = down;
        g_show = g_user_show && fg;   // lose focus → hooks deactivate; regain → panel restores
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
        if (s_prev_show && !g_show && fg && o_clip_cursor && g_hwnd)
        {
            RECT rc;
            if (GetClientRect(g_hwnd, &rc))
            {
                POINT tl{rc.left, rc.top}, br{rc.right, rc.bottom};
                ClientToScreen(g_hwnd, &tl);
                ClientToScreen(g_hwnd, &br);
                RECT screen{tl.x, tl.y, br.x, br.y};
                o_clip_cursor(&screen); // ORIGINAL ClipCursor (g_show already false here)
            }
        }
        s_prev_show = g_show;

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
        // Install + LOG each user32 hook. A silent failure here is the suspected cause of the
        // "click the search box → it never focuses" bug on some launches: if SetCursorPos or
        // GetCursorPos fails to hook (e.g. another mod / the Steam overlay trampolined user32
        // first, or a MinHook race), the game's per-frame recenter-to-middle is NOT swallowed →
        // ImGui_ImplWin32_NewFrame reads the cursor at screen centre → every click lands at the
        // centre, never on the InputText. The GetCursorPos + SetCursorPos lines are the ones to
        // watch in the log on a "bad" launch.
        auto hook_u32 = [&](const char *name, void *detour, void **orig) {
            void *tgt = reinterpret_cast<void *>(GetProcAddress(u32, name));
            if (!tgt) { spdlog::error("[OVERLAY] user32!{} not found — hook skipped", name); return; }
            MH_STATUS cs = MH_CreateHook(tgt, detour, orig);
            MH_STATUS es = (cs == MH_OK) ? MH_EnableHook(tgt) : cs;
            if (cs != MH_OK || es != MH_OK)
                spdlog::error("[OVERLAY] user32!{} HOOK FAILED (create={}, enable={}) — cursor/input "
                              "may misbehave (search box may not take focus)",
                              name, MH_StatusToString(cs), MH_StatusToString(es));
            else
                spdlog::info("[OVERLAY] user32!{} hook installed", name);
        };
        hook_u32("SetCursorPos", reinterpret_cast<void *>(&hk_set_cursor_pos),
                 reinterpret_cast<void **>(&o_set_cursor_pos));
        hook_u32("ClipCursor", reinterpret_cast<void *>(&hk_clip_cursor),
                 reinterpret_cast<void **>(&o_clip_cursor));
        hook_u32("GetRawInputData", reinterpret_cast<void *>(&hk_get_raw_input_data),
                 reinterpret_cast<void **>(&o_get_raw_input_data));
        hook_u32("GetRawInputBuffer", reinterpret_cast<void *>(&hk_get_raw_input_buffer),
                 reinterpret_cast<void **>(&o_get_raw_input_buffer));
        hook_u32("GetCursorPos", reinterpret_cast<void *>(&hk_get_cursor_pos),
                 reinterpret_cast<void **>(&o_get_cursor_pos));
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

bool goblin::overlay::native_item_icon(int iconId, void *&tex, float &u0, float &v0, float &u1,
                                       float &v1)
{
    if (iconId < 0)
        return false;
    // Sheet-as-atlas: resolve the icon's harvested sheet + rect (the MENU_ItemIcon_<id> inventory
    // atlas — the 00_Solo TPFs), copy the WHOLE sheet once (cached), and return that shared texture
    // + the icon's UV sub-rect. Falls back (false) until the sheet is harvested + GPU-bound, so the
    // caller keeps the baked atlas icon meanwhile. Drives the per-CATEGORY representative item icon.
    goblin::ItemSprite sp;
    if (!goblin::harvested_icon(iconId, sp) || !sp.sheet)
        return false;
    const SheetTex *st = copy_sheet_cached(reinterpret_cast<ID3D12Resource *>(sp.sheet));
    if (!st || !st->gpu || st->w <= 0 || st->h <= 0)
        return false;
    tex = reinterpret_cast<void *>(st->gpu);
    u0 = (float)sp.x0 / st->w; v0 = (float)sp.y0 / st->h;
    u1 = (float)sp.x1 / st->w; v1 = (float)sp.y1 / st->h;
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
    int x = 0, y = 0, w = 0, h = 0;
    void *sheet = nullptr;
    if (!goblin::map_icon_rect_by_name(name, x, y, w, h, sheet) || !sheet || w <= 0 || h <= 0)
        return false;
    const SheetTex *st = copy_sheet_cached(reinterpret_cast<ID3D12Resource *>(sheet));
    if (!st || !st->gpu || st->w <= 0 || st->h <= 0)
        return false;
    tex = reinterpret_cast<void *>(st->gpu);
    u0 = (float)x / st->w; v0 = (float)y / st->h;
    u1 = (float)(x + w) / st->w; v1 = (float)(y + h) / st->h;
    return true;
}
