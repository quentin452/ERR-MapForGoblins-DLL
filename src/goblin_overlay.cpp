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

#include <vector>
#include <map>
#include <string>

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

    // ── Overlay-rendered markers PROTOTYPE (config overlay_markers_proto) ──
    // Project the live map reticle (and later real markers) onto the open world
    // map via the WorldMapArea pan/zoom, to verify the world→screen affine. The
    // RE doc's formula direction is unconfirmed (evidence says zoom = px/marker-
    // unit → MULTIPLY, not the doc's /zoom), so both forms + a residual {scale,
    // bias} are exposed as live sliders. Tune until our dot sits on the game's
    // reticle through pan + zoom → the affine is solved.
    // SOLVED model (re-fitted from the cross-capture, 2026-06-19) — CENTER-RELATIVE,
    // so it's zoom-independent (the earlier pan-based form drifted with zoom):
    //   screenX = (markerU - centerU) * zoom * scaleX + realW/2 + biasX
    //   screenY = (markerV - centerV) * zoom * scaleY + realH/2 + biasY
    // centerU = +0xFC (raw[0]), centerV = +0x100 (raw[1]) = the VIEW CENTER in marker
    // space; markerU = +0x104 (raw[2]), markerV = +0x108 (raw[3]) = a point's coords;
    // screen-centre (realW/2, realH/2) is the pivot. scale ≈ 0.986 (fine residual);
    // bias ≈ 0 (centre handles it). Verified: (3390-4824)*0.196+960 = 679 ✓.
    struct MarkerCalib
    {
        float scaleX = 1.0f, scaleY = 1.0f, biasX = 0.0f, biasY = 0.0f;
    };
    MarkerCalib g_calib;

    // (markerU, markerV) marker coords → backbuffer px. centerU/V = live view centre.
    ImVec2 project_uv(const goblin::worldmap_probe::LiveView &v, float mU, float mV,
                      float realW, float realH)
    {
        float centerU = v.raw[0], centerV = v.raw[1];
        return ImVec2((mU - centerU) * v.zoom * g_calib.scaleX + realW * 0.5f + g_calib.biasX,
                      (mV - centerV) * v.zoom * g_calib.scaleY + realH * 0.5f + g_calib.biasY);
    }

    // OS cursor in client/backbuffer px, read DIRECTLY (not io.MousePos). ImGui's
    // mouse pos comes from the WndProc, which hk_wndproc only forwards while the
    // menu is open → io.MousePos freezes once the menu has been closed. The game
    // reticle still tracks the OS cursor, so read that straight from the source.
    ImVec2 os_mouse_px()
    {
        POINT pt{};
        BOOL ok = o_get_cursor_pos ? o_get_cursor_pos(&pt) : GetCursorPos(&pt);
        if (ok && g_hwnd && ScreenToClient(g_hwnd, &pt))
            return ImVec2((float)pt.x, (float)pt.y);
        return ImGui::GetIO().MousePos; // fallback
    }

    // Draws the prototype every frame the map is open. Foreground dots = always;
    // the tuning window + hotkeys = the calibration UI.
    void draw_markers_proto(bool menu_open)
    {
        goblin::worldmap_probe::LiveView v;
        bool live = goblin::worldmap_probe::get_live_view(v);
        ImGuiIO &io = ImGui::GetIO();
        ImDrawList *fg = ImGui::GetForegroundDrawList();

        // DIAG (first-open latency): how long have we been WITHOUT a live view, and is
        // the probe's published cursor null (probe hasn't found one) or non-null but
        // failing the live read? Accumulates while !live, resets on live.
        static float wait_s = 0.0f;
        if (live)
            wait_s = 0.0f;
        else
        {
            wait_s += io.DeltaTime;
            uintptr_t ac = goblin::worldmap_probe::debug_active_cursor();
            char wbuf[160];
            snprintf(wbuf, sizeof(wbuf),
                     "MARKER PROTO: no live view  waiting=%.2fs  active_cursor=%#llx  (%s)",
                     wait_s, (unsigned long long)ac,
                     ac ? "published but read failed" : "probe has no cursor yet");
            fg->AddText(ImVec2(12, 12), IM_COL32(0, 0, 0, 200), wbuf);
            fg->AddText(ImVec2(11, 11), IM_COL32(255, 220, 0, 255), wbuf);
        }

        // Magenta cross at the mouse = where we BELIEVE the game reticle is. If it
        // sits on the game's reticle, reticle==mouse holds → capture is valid.
        ImVec2 m = os_mouse_px();
        fg->AddLine(ImVec2(m.x - 12, m.y), ImVec2(m.x + 12, m.y), IM_COL32(255, 0, 255, 220), 1.0f);
        fg->AddLine(ImVec2(m.x, m.y - 12), ImVec2(m.x, m.y + 12), IM_COL32(255, 0, 255, 220), 1.0f);

        // The reticle's screen-axis coords: U = +0x104 (raw[2]), V = +0x108 (raw[3]).
        float U = v.raw[2], V = v.raw[3];
        if (live)
        {
            ImVec2 p = project_uv(v, U, V, io.DisplaySize.x, io.DisplaySize.y);
            // Our projected reticle: cyan ring + crosshair. Should sit on the game
            // reticle (= the magenta mouse cross) once calibrated.
            fg->AddCircle(p, 10.0f, IM_COL32(0, 255, 255, 255), 0, 2.0f);
            fg->AddLine(ImVec2(p.x - 14, p.y), ImVec2(p.x + 14, p.y), IM_COL32(0, 255, 255, 200), 1.0f);
            fg->AddLine(ImVec2(p.x, p.y - 14), ImVec2(p.x, p.y + 14), IM_COL32(0, 255, 255, 200), 1.0f);

            // On-screen readout (menu-CLOSED, so the reticle is live and mouse==reticle).
            // err = projected - mouse = the true model error. Verify it stays ~0 through
            // pan+zoom. (Reading this via the F1 window is useless: opening F1 freezes the
            // game reticle, so cyan and the mouse diverge by design.)
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "MARKER PROTO  zoom=%.4f  err=(%.1f, %.1f)px  scale=(%.4f,%.4f)\n"
                     "C=calibrate (reticle on mouse)   X=reset   L=log",
                     v.zoom, p.x - m.x, p.y - m.y, g_calib.scaleX, g_calib.scaleY);
            fg->AddText(ImVec2(12, 12), IM_COL32(0, 0, 0, 200), buf);          // shadow
            fg->AddText(ImVec2(11, 11), IM_COL32(0, 255, 255, 255), buf);
        }

        // Hotkeys (work menu-closed): C = 1-point calibrate (solve biasX/Y so the
        // projection of the reticle hits the mouse), L = log a row, X = reset. The
        // model is linear in bias, so a single capture pins both biases exactly.
        static bool prevC = false, prevX = false, prevL = false;
        bool downC = (GetAsyncKeyState('C') & 0x8000) != 0;
        bool downX = (GetAsyncKeyState('X') & 0x8000) != 0;
        bool downL = (GetAsyncKeyState('L') & 0x8000) != 0;
        if (downC && !prevC && live)
        {
            // Solve the residual bias so the projection hits the mouse exactly here.
            g_calib.biasX = 0.f;
            g_calib.biasY = 0.f;
            ImVec2 p0 = project_uv(v, U, V, io.DisplaySize.x, io.DisplaySize.y);
            g_calib.biasX = m.x - p0.x;
            g_calib.biasY = m.y - p0.y;
        }
        if (downL && !prevL && live)
            spdlog::info("[MARKERCAL] mouse=({:.1f},{:.1f}) pan=({:.2f},{:.2f}) zoom={:.5f} "
                         "U(+0x104)={:.2f} V(+0x108)={:.2f}", m.x, m.y, v.panX, v.panZ, v.zoom, U, V);
        if (downX && !prevX)
            g_calib = MarkerCalib{};
        prevC = downC;
        prevX = downX;
        prevL = downL;

        if (!menu_open)
            return;
        ImGui::SetNextWindowBgAlpha(0.88f);
        if (ImGui::Begin("Marker proto (affine tune)"))
        {
            if (!live)
                ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "no live map view (open the world map)");
            else
            {
                ImVec2 p = project_uv(v, U, V, io.DisplaySize.x, io.DisplaySize.y);
                ImGui::Text("U(+0x104)=%.1f  V(+0x108)=%.1f", U, V);
                ImGui::Text("pan=(%.1f, %.1f)  zoom=%.4f", v.panX, v.panZ, v.zoom);
                ImGui::Text("projected px = (%.0f, %.0f)   mouse = (%.0f, %.0f)   [%.0fx%.0f]",
                            p.x, p.y, m.x, m.y, io.DisplaySize.x, io.DisplaySize.y);
                ImGui::Text("err = (%.0f, %.0f) px", p.x - m.x, p.y - m.y);
            }
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0, 1, 1, 1), "model: screenX=U*zoom*sx - panX + bx ; Y=V*zoom*sy - panZ + by");
            ImGui::TextWrapped("C = 1-point calibrate (reticle on mouse) -> then PAN+ZOOM and watch the "
                               "cyan ring stay locked. If it drifts, nudge scaleX/scaleY ~1.0. X = reset.");
            ImGui::Separator();
            ImGui::DragFloat("scaleX", &g_calib.scaleX, 0.0005f, 0.5f, 1.5f, "%.5f");
            ImGui::DragFloat("scaleY", &g_calib.scaleY, 0.0005f, 0.5f, 1.5f, "%.5f");
            ImGui::DragFloat("biasX", &g_calib.biasX, 0.5f, -4000.0f, 4000.0f, "%.1f");
            ImGui::DragFloat("biasY", &g_calib.biasY, 0.5f, -4000.0f, 4000.0f, "%.1f");
            if (ImGui::Button("reset calib"))
                g_calib = MarkerCalib{};
        }
        ImGui::End();
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
                if (ImGui::Checkbox("Enable clustering (live — reopen map to apply)", &en))
                    goblin::ui::set_clustering_enabled(en);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Collapse dense marker piles into one cluster icon.\n"
                                      "Live (no restart); close+reopen the map to see it.");

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
                    if (ImGui::Checkbox("Scale cluster size by distance from player", &da))
                    {
                        goblin::config::clusterDistanceAdaptive = da;
                        goblin::ui::request_cluster_replan();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Full detail near you; distant dense spots merge into bigger\n"
                                          "piles (fewer far icons, faster map). Ramps from the cluster\n"
                                          "size above (near) to the far size below, same map area only.\n"
                                          "Recomputed when you open the map. Live.");
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
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Aggressive far-merge: fewest distant icons (Steam Deck / low-end).");

                    bool dbg = goblin::ui::cluster_debug();
                    if (ImGui::Checkbox("Show cluster bubbles on map (off = counts only in this menu)", &dbg))
                        goblin::ui::set_cluster_debug(dbg);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("On: a pile glyph (with its per-location count) sits on the map.\n"
                                          "Off: no bubble on the map; the per-category counts above are\n"
                                          "the source. Clustered piles stay parked either way (no freeze).");

                    if (!goblin::ui::clustering_active())
                        ImGui::TextDisabled("No location over this size — lower the cluster size.");
                }
            }

            // Dev/debug observers. Each installs a hook or starts a worker thread
            // ONCE at startup based on its config flag (dllmain setup), so these
            // are restart-required: flip + Save, then relaunch. save_all_bool_settings
            // persists every config bool, so no per-flag plumbing is needed.
            ImGui::SeparatorText("Debug");
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

        // The marker prototype draws over the open map even when the menu is closed,
        // so build a frame for it too (get_live_view() no-ops when the map is shut).
        bool proto = goblin::config::overlayMarkersProto;
        if ((g_show || proto) && g_command_queue)
        {
            g_imgui_reading_cursor = true;   // let ImGui's NewFrame see the real cursor
            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            g_imgui_reading_cursor = false;
            ImGui::NewFrame();
            if (g_show)
                draw_panel();
            if (proto)
                draw_markers_proto(g_show);
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
