#include "goblin_overlay.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <MinHook.h>
#include <spdlog/spdlog.h>

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include "goblin_inject.hpp"   // goblin::world_map_open()

#include <vector>

// ImGui's Win32 backend message handler (defined in imgui_impl_win32.cpp).
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
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

    // Raw input — ER reads gameplay keyboard/mouse here (not via window
    // messages), so we neutralise it while the menu is open to fully disable
    // game commands. ImGui still gets keyboard/mouse via the WndProc + cursor.
    using GetRawInputDataFn =
        UINT(WINAPI *)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
    GetRawInputDataFn o_get_raw_input_data = nullptr;

    using GetRawInputBufferFn = UINT(WINAPI *)(PRAWINPUT, PUINT, UINT);
    GetRawInputBufferFn o_get_raw_input_buffer = nullptr;

    // ── D3D12 state captured from the live game ───────────────────────────
    ID3D12Device *g_device = nullptr;
    ID3D12CommandQueue *g_command_queue = nullptr;   // captured from ExecuteCommandLists
    ID3D12DescriptorHeap *g_rtv_heap = nullptr;
    ID3D12DescriptorHeap *g_srv_heap = nullptr;       // ImGui font SRV (1 descriptor)
    ID3D12GraphicsCommandList *g_command_list = nullptr;

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
        if (g_rtv_heap) { g_rtv_heap->Release(); g_rtv_heap = nullptr; }
        if (g_srv_heap) { g_srv_heap->Release(); g_srv_heap = nullptr; }
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
            case WM_INPUT:
            case WM_MOUSEMOVE:
            case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
            case WM_XBUTTONDOWN: case WM_XBUTTONUP:
            case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
            case WM_SYSKEYDOWN: case WM_SYSKEYUP:
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

        // SRV heap: ImGui font (shader-visible, 1 descriptor).
        D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
        srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv_desc.NumDescriptors = 1;
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
        // The game hides the OS cursor, so draw ImGui's own software cursor.
        // Only rendered while the panel is up (we skip NewFrame otherwise).
        ImGui::GetIO().MouseDrawCursor = true;
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
            ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(320, 380), ImGuiCond_FirstUseEver);
            ImGui::Begin("Map for Goblins##large");
            if (ImGui::Button("[-] collapse"))
                g_large = false;
            ImGui::SameLine();
            ImGui::TextDisabled("F1 close | %.0f fps", io.Framerate);
            ImGui::Separator();

            // Master on/off.
            bool icons_on = goblin::ui::icons_enabled();
            if (ImGui::Checkbox("Show icons (master)", &icons_on))
                goblin::ui::set_icons_enabled(icons_on);

            // Sections (coarse) + their categories (fine). A row shows only if
            // both its section and its category are enabled.
            ImGui::SeparatorText("Sections & categories");
            for (int s = 0; s < goblin::ui::section_count(); s++)
            {
                if (!ImGui::TreeNode(goblin::ui::section_label(s)))
                    continue;

                bool sv = goblin::ui::section_visible(s);
                if (ImGui::Checkbox("(whole section)", &sv))
                    goblin::ui::set_section_visible(s, sv);

                ImGui::PushID(s);
                if (ImGui::SmallButton("All"))
                    for (int c = 0; c < goblin::ui::category_count(); c++)
                        if (goblin::ui::category_section(c) == s)
                            goblin::ui::set_category_visible(c, true);
                ImGui::SameLine();
                if (ImGui::SmallButton("None"))
                    for (int c = 0; c < goblin::ui::category_count(); c++)
                        if (goblin::ui::category_section(c) == s)
                            goblin::ui::set_category_visible(c, false);
                ImGui::PopID();
                ImGui::Separator();

                for (int c = 0; c < goblin::ui::category_count(); c++)
                {
                    if (goblin::ui::category_section(c) != s) continue;
                    bool cv = goblin::ui::category_visible(c);
                    if (ImGui::Checkbox(goblin::ui::category_label(c), &cv))
                        goblin::ui::set_category_visible(c, cv);
                }
                ImGui::TreePop();
            }

            // Clustering (only on builds that ship cluster data).
            if (goblin::ui::clustering_available())
            {
                ImGui::SeparatorText("Clustering");
                bool expanded = goblin::ui::clusters_expanded();
                if (ImGui::Checkbox("Expand clusters (show members)", &expanded))
                    goblin::ui::set_clusters_expanded(expanded);
                bool dbg = goblin::ui::cluster_debug();
                if (ImGui::Checkbox("Cluster labels show counts", &dbg))
                    goblin::ui::set_cluster_debug(dbg);
            }
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

        // F1 = open/close the menu (the keybind; works anywhere, incl. over the
        // 2D map). Edge-detected. (The CSMenuMan+0xCD auto-show signal proved
        // dead on this build — reads 0 even with the map open — so F1 is the
        // sole trigger; that is also the intended keybind-driven UX.)
        bool down = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
        if (down && !g_prev_toggle_down) g_user_show = !g_user_show;
        g_prev_toggle_down = down;
        g_show = g_user_show;

        if (g_show && g_command_queue)
        {
            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            draw_panel();
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
