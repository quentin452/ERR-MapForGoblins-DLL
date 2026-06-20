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
        // S = zoom EXACTLY (scale 1.0). The earlier 0.987 was mouse-capture noise and
        // left a ~1.3% residual = the micro reticle offset (grows from screen centre).
        // Proven end-to-end err=(0,-0.1)px at scale 1.0; nudge only if drift reappears.
        float scaleX = 1.0f, scaleY = 1.0f, biasX = 0.0f, biasY = 0.0f;
    };
    MarkerCalib g_calib;

    // Render-ease gain on the grace screen-shift. DEFAULT 0 = OFF: the reticle ease source
    // (+0x10C − +0x104) is SNAP-contaminated (when locked onto an icon +0x104 snaps but
    // +0x10C stays at the cursor → a bogus non-zero delta at rest that drags every grace).
    // F/H still dial it for experiments; the grid diag (key I) is the cleaner signal.
    float g_easeGain = 0.0f;

    // ★ THE view centre = (pan + snapMid)/zoom — NOT the reticle (+0xFC). Proven by the
    // gamepad symptom (2026-06-20): the old reticle centre makes markers "jamais centré" on
    // the stick and only aligns when you move the MOUSE — because the reticle-centre formula
    // `screen = (marker − reticle)·zoom + screenCentre` is correct ONLY when the reticle sits
    // at screen centre; the mouse lets you put it there, the gamepad reticle sits off-centre.
    // The engine pan setter FUN_1409cd100 does `pan = zoom·viewCentre − snapMid` (snapMid =
    // midpoint of WorldMapArea +0x340..+0x34c), so the marker at screen centre is
    // `viewCentre = (pan + snapMid)/zoom`. Under mouse it EQUALS the reticle (drop-in for the
    // known-good baseline); under gamepad the reticle is off-centre but viewCentre is right →
    // markers stay locked on the map for BOTH devices, no mouse wiggle needed. (Earlier "pan
    // is instance-variant" was bare `pan` missing the per-page snapMid term — fixed here.)
    // Cross-checked against the origin RE line (docs/windows_worldmap_viewcenter_re_findings.md):
    // that brief reaches the SAME cursor-independent centre via the pan setter + transform
    // FUN_1409cd0a0 (`screen = marker·zoom − pan`, centre = (pan + screenCentre)/zoom), and
    // confirms via [INPUT-DELTA] that the GAMEPAD stick pans +0x378/+0x37c (+ zoom +0x380) and
    // never moves the reticle (+0xFC/+0x104 fire only on mouse hover) — consistent with snapMid.
    // Default = (pan+snapMid)/zoom (cursor-INDEPENDENT). This is the fix the user CONFIRMED
    // kills the camera-frozen / mouse-to-screen-edge drift (bug 1) — markers no longer track
    // the cursor. Y toggles to the raw-reticle baseline for A/B.
    bool g_pan_center = true;

    // (markerU, markerV) marker coords → backbuffer px.
    ImVec2 project_uv(const goblin::worldmap_probe::LiveView &v, float mU, float mV,
                      float realW, float realH)
    {
        float centerU, centerV;
        if (g_pan_center)
        {
            // Device-independent view centre = (pan+snapMid)/zoom, PLUS the engine's render
            // ease. The game DRAWS from the eased reticle +0x10C (raw[4/5]), not the target
            // +0x104 (raw[2/3]) — confirmed in-game (the +0x10C dot lands on the real reticle
            // during fast moves). The ease in marker space = (+0x10C − +0x104); the cursor
            // offset cancels in that difference, so adding it to the target centre yields the
            // DISPLAYED centre (cursor-independent). At rest the two are equal → no change.
            centerU = (v.panX + v.snapMidX) / v.zoom;
            centerV = (v.panZ + v.snapMidZ) / v.zoom;
        }
        else
        {
            centerU = v.raw[0]; centerV = v.raw[1]; // raw reticle (mouse-only) for A/B compare
        }
        // Same proven reticle-form math for both paths (screen-centre anchor + scale/bias).
        // NOTE: the g_pan_center path uses snapMid = (snapMin+snapMax)/2 (the origin RE line's
        // cursor-independent centre, equivalent to (pan + (min+max)/2)/zoom) — see project below.
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

    // ── In-DLL world→render AFFINE solver (replaces the by-hand diagonal calib) ──
    // The world→render step is a full affine `render = M·world + T` (M = scale·
    // rotation, shared across pages; T per page) — by-hand origin·scale came out
    // ROTATED (see docs/marker_mapspace_CT_recipe.md). The overlay already reads the
    // live cursor render coords (+0x104/+0x108) for calibration, so we recover M/T
    // IN-DLL from (world→render) pairs — no external Cheat Engine needed.
    struct CalPair { int page; float wx, wz, rx, rz; };
    std::vector<CalPair> g_cal_pairs;

    struct AffineFit
    {
        bool enabled = true;          // affine vs the diagonal origin baseline
        // shared M = [[a,b],[c,d]]; seed = the 90° axis-swap @ 0.5 hypothesis
        // (renderX ≈ 0.5·worldZ, renderZ ≈ 0.5·worldX) — confirm signs live.
        // NOTE the seed has det<0 = a REFLECTION (mirror), not a rotation; the true
        // map frame is likely a 90° ROTATION (det>0) = one off-diagonal sign flipped.
        // Use the F1 presets + live a/b/c/d + global gtx/gty pan to dial it by eye.
        // DEFAULT = user's hand-tuned OVERWORLD fit (2026-06-20): M = swap@1.0, render
        // rotation −90°, pan (−2170, 850), centroid pivot. Roughly aligns 60/61; eyeball-
        // approximate (scale not exact → slight de-centering when zoomed far). Underground
        // (12) / DLC (40-43) need their own pan; precise per-page bake via lstsq later.
        float a = 0.f, b = 1.f, c = 1.f, d = 0.f;
        float e[64] = {}, f[64] = {}; // per-page T
        float gtx = -1086.f, gty = -341.f; // overworld pan (fixed-pivot, user-tuned 06-20)
        bool pivot = true;            // eyeball: rotate about the marker CENTROID (so
                                      // changing M spins in place, not flings about (0,0))
        float screen_rot = -90.f;     // REAL rotation field (deg, CW), applied in RENDER
                                      // space so the live pan/zoom tracks. User: −90.
        // UNDERGROUND (page 12 + DLC 40-43) has its OWN render frame (the game switches
        // canvas) → own rotation + pan, picked when the DAT_143d6cfc3 sublayer flag is
        // set. Seeded from overworld; dial while the underground map is open.
        float screen_rot_u = -90.f;
        float gtx_u = -2170.f, gty_u = 850.f;
    };
    float g_centroidX = 0.f, g_centroidZ = 0.f; // world centroid of drawn graces (pivot)
    AffineFit g_aff;

    // Solve A·x = b (3×3) by Gaussian elimination w/ partial pivoting. A is mutated.
    bool solve3x3(double A[3][3], double b[3], double out[3])
    {
        for (int col = 0; col < 3; ++col)
        {
            int piv = col;
            for (int r = col + 1; r < 3; ++r)
                if (std::fabs(A[r][col]) > std::fabs(A[piv][col])) piv = r;
            if (std::fabs(A[piv][col]) < 1e-9) return false;
            if (piv != col)
            {
                for (int c = 0; c < 3; ++c) std::swap(A[piv][c], A[col][c]);
                std::swap(b[piv], b[col]);
            }
            for (int r = 0; r < 3; ++r)
            {
                if (r == col) continue;
                double fac = A[r][col] / A[col][col];
                for (int c = 0; c < 3; ++c) A[r][c] -= fac * A[col][c];
                b[r] -= fac * b[col];
            }
        }
        for (int i = 0; i < 3; ++i) out[i] = b[i] / A[i][i];
        return true;
    }

    // Fit shared M + per-page T from g_cal_pairs. Pass 1: per page with ≥3 pairs,
    // least-squares solve [a,b,e]/[c,d,f]; average a,b,c,d → shared M. Pass 2: per
    // page T = mean residual (render − M·world) so M is shared, T per-page. Logs
    // M, each page's T, and mean per-pair residual (sub-pixel = good). Returns #pages.
    int solve_affine(AffineFit &af)
    {
        std::map<int, std::vector<const CalPair *>> by;
        for (auto &p : g_cal_pairs) by[p.page & 63].push_back(&p);

        double sa = 0, sb = 0, sc = 0, sd = 0;
        int mcount = 0;
        for (auto &kv : by)
        {
            auto &pts = kv.second;
            if (pts.size() < 3) continue;
            double ATA[3][3] = {}, atx[3] = {}, atz[3] = {};
            for (auto *p : pts)
            {
                double row[3] = {p->wx, p->wz, 1.0};
                for (int i = 0; i < 3; ++i)
                {
                    for (int j = 0; j < 3; ++j) ATA[i][j] += row[i] * row[j];
                    atx[i] += row[i] * p->rx;
                    atz[i] += row[i] * p->rz;
                }
            }
            double A2[3][3], bb[3], rx[3], rz[3];
            std::memcpy(A2, ATA, sizeof A2); std::memcpy(bb, atx, sizeof bb);
            if (!solve3x3(A2, bb, rx)) continue;
            std::memcpy(A2, ATA, sizeof A2); std::memcpy(bb, atz, sizeof bb);
            if (!solve3x3(A2, bb, rz)) continue;
            sa += rx[0]; sb += rx[1]; sc += rz[0]; sd += rz[1]; ++mcount;
            spdlog::info("[AFFINE] page {} (n={}) M=[[{:.5f},{:.5f}],[{:.5f},{:.5f}]] T=({:.2f},{:.2f})",
                         kv.first, pts.size(), rx[0], rx[1], rz[0], rz[1], rx[2], rz[2]);
        }
        if (mcount == 0)
        {
            spdlog::warn("[AFFINE] need >=3 pairs on at least one page (have {} pairs)", g_cal_pairs.size());
            return 0;
        }
        af.a = (float)(sa / mcount); af.b = (float)(sb / mcount);
        af.c = (float)(sc / mcount); af.d = (float)(sd / mcount);

        int pages = 0;
        for (auto &kv : by)
        {
            int pg = kv.first;
            double te = 0, tf = 0;
            for (auto *p : kv.second)
            {
                te += p->rx - (af.a * p->wx + af.b * p->wz);
                tf += p->rz - (af.c * p->wx + af.d * p->wz);
            }
            af.e[pg] = (float)(te / kv.second.size());
            af.f[pg] = (float)(tf / kv.second.size());
            double mr = 0;
            for (auto *p : kv.second)
            {
                double dx = p->rx - (af.a * p->wx + af.b * p->wz + af.e[pg]);
                double dz = p->rz - (af.c * p->wx + af.d * p->wz + af.f[pg]);
                mr += std::sqrt(dx * dx + dz * dz);
            }
            ++pages;
            spdlog::info("[AFFINE] page {} T=({:.2f},{:.2f}) meanResidual={:.2f}px (n={})",
                         pg, af.e[pg], af.f[pg], mr / kv.second.size(), kv.second.size());
        }
        spdlog::info("[AFFINE] SHARED M=[[{:.5f},{:.5f}],[{:.5f},{:.5f}]] from {} page(s) -> bake M once + per-page T",
                     af.a, af.b, af.c, af.d, mcount);
        return pages;
    }

    // Draws the prototype every frame the map is open. Foreground dots = always;
    // the tuning window + hotkeys = the calibration UI.
    void draw_markers_proto(bool menu_open)
    {
        goblin::worldmap_probe::LiveView v;
        bool live = goblin::worldmap_probe::get_live_view(v);
        ImGuiIO &io = ImGui::GetIO();
        ImDrawList *fg = ImGui::GetForegroundDrawList();

        // RENDER-EASE MATCH (low-pass, F/H). Bug 2: we project the engine's TARGET fields
        // (pan +0x378, reticle +0x104) which are INSTANT, but ER DRAWS them eased toward the
        // target each frame (cursor +0x130 0.1f lerp / snap-anim FUN_1409bc8c0). Proven by the
        // cyan(target) vs game-reticle(eased) gap on fast moves. So our overlay leads the eased
        // map during motion. Match it: low-pass the SAME fields with the engine's alpha —
        //   s += (raw - s)*alpha.   alpha=1 -> instant (current). alpha~0.1 -> ER's ease.
        // Eased fields: panX/panZ (grace centre) AND reticle raw[2]/raw[3] (the cyan). At rest
        // s==raw so static placement is untouched; only the in-motion render-lead is removed.
        // FRAMERATE-INDEPENDENT ease (F/H dial TAU = the time-constant in seconds). For a
        // constant-velocity scroll the steady-state lag of an exp filter is (1−a)/a·v·dt,
        // which is STILL dt-dependent (the bug: grid lag changed with fps even after dt-norm).
        // Use a = dt/(dt+tau) instead → steady lag = v·tau EXACTLY, frame-independent. tau≈0.02s
        // ↔ the 0.5/frame at ~50fps. Re-seed on map close (!live) so reopen doesn't ease from a
        // stale pan (= icons frozen off-screen).
        static float g_easeTau = 0.02f;
        {
            static float sX = 0, sZ = 0, sZoom = 0;
            static bool sInit = false;
            if (live)
            {
                float dt = io.DeltaTime; if (dt < 0.0f) dt = 0.0f; else if (dt > 0.1f) dt = 0.1f;
                float a = (g_easeTau > 0.0001f) ? dt / (dt + g_easeTau) : 1.0f;
                if (a < 0.0f) a = 0.0f; else if (a > 1.0f) a = 1.0f;
                if (!sInit) { sX = v.panX; sZ = v.panZ; sZoom = v.zoom; sInit = true; }
                sX    += (v.panX - sX)    * a;
                sZ    += (v.panZ - sZ)    * a;
                sZoom += (v.zoom - sZoom) * a; // zoom eases too, else zoom drifts
                v.panX = sX; v.panZ = sZ; v.zoom = sZoom; // grid + graces; reticle left raw as target ref
            }
            else
                sInit = false; // map closed → re-seed fresh on reopen
        }

        // UPDATE-RATE THROTTLE (T / Z). Hypothesis: ER refreshes the displayed map at a fixed
        // lower rate (e.g. 30 Hz) while we re-sample every Present frame, so between the
        // engine's updates our markers run ahead. Quantize our pan/reticle sampling to
        // g_mapFps Hz (hold the value between ticks). g_mapFps<=0 = every frame (off).
        static float g_mapFps = 0.0f;
        {
            static float acc = 0.0f, hX = 0, hZ = 0, hRU = 0, hRV = 0;
            static bool hInit = false;
            if (live && g_mapFps > 0.0f)
            {
                acc += io.DeltaTime;
                if (!hInit || acc >= 1.0f / g_mapFps)
                {
                    hX = v.panX; hZ = v.panZ; hRU = v.raw[2]; hRV = v.raw[3];
                    acc = 0.0f; hInit = true;
                }
                v.panX = hX; v.panZ = hZ; v.raw[2] = hRU; v.raw[3] = hRV;
            }
            else
                hInit = false;
        }
        {
            char lb[96];
            snprintf(lb, sizeof(lb), "EASETAU=%.4fs (F/H, R=.02)   grid=I   MAPFPS=%.0f (T/Z)",
                     g_easeTau, g_mapFps);
            fg->AddText(ImVec2(12, 31), IM_COL32(0, 0, 0, 200), lb);
            fg->AddText(ImVec2(11, 30), IM_COL32(120, 255, 160, 255), lb);
            // DIAG: live snap-rect + pan + reticle. Watch in UNDERGROUND while moving the
            // mouse with the camera frozen — if snapMin/snapMax (or snapMid) CHANGE with the
            // cursor, the snap-rect is cursor-coupled there (= bug1 reappears via the centre).
            char sb[200];
            snprintf(sb, sizeof(sb),
                     "snap[%.1f,%.1f .. %.1f,%.1f] mid(%.1f,%.1f) pan(%.1f,%.1f) ret(%.1f,%.1f) z%.3f",
                     v.snapMinX, v.snapMinZ, v.snapMaxX, v.snapMaxZ, v.snapMidX, v.snapMidZ,
                     v.panX, v.panZ, v.raw[2], v.raw[3], v.zoom);
            fg->AddText(ImVec2(12, 49), IM_COL32(0, 0, 0, 200), sb);
            fg->AddText(ImVec2(11, 48), IM_COL32(255, 220, 120, 255), sb);
        }

        // TERRAIN-GRID DIAG (toggle I). Draws a grid at fixed marker-space intervals through
        // OUR projection (the target frame). Compare its lines to the game's map features
        // during a scroll/lock: if our grid LEADS/LAGS the map art, that gap IS the bug-2
        // target-vs-displayed offset, seen directly without the snap-contaminated reticle.
        static bool g_grid = false;
        if (g_grid && live)
        {
            const float step = 512.0f;            // marker-space spacing
            float cU = (v.panX + v.snapMidX) / v.zoom; // view-centre marker coord
            float cV = (v.panZ + v.snapMidZ) / v.zoom;
            int N = 12;
            float baseU = roundf(cU / step) * step, baseV = roundf(cV / step) * step;
            ImU32 gcol = IM_COL32(255, 0, 255, 90);
            for (int k = -N; k <= N; ++k)
            {
                float lu = baseU + k * step, lv = baseV + k * step;
                ImVec2 a = project_uv(v, lu, cV - N * step, io.DisplaySize.x, io.DisplaySize.y);
                ImVec2 b = project_uv(v, lu, cV + N * step, io.DisplaySize.x, io.DisplaySize.y);
                fg->AddLine(a, b, gcol, 1.0f);     // vertical line (const U)
                ImVec2 c = project_uv(v, cU - N * step, lv, io.DisplaySize.x, io.DisplaySize.y);
                ImVec2 d = project_uv(v, cU + N * step, lv, io.DisplaySize.x, io.DisplaySize.y);
                fg->AddLine(c, d, gcol, 1.0f);     // horizontal line (const V)
            }
        }

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

        // FRAME-LAG CAPTURE (toggle V). One CSV row per frame: the pan we sampled this
        // frame + the reticle marker coord + the OS mouse px (= the game reticle's true
        // screen pos = ground truth). Grep "[LAGCSV]" out of the log → feed the data-driven
        // test: it cross-correlates the per-frame gap (reticle_projected − mouse) against the
        // 1-frame model (pan[n] − pan[n−1])·scale to prove the lag is exactly one frame.
        //   columns: frame,panX,panZ,zoom,reticleU,reticleV,mouseX,mouseY,slX,slZ
        // slX=retU*zoom-panX, slZ=retV*zoom-panZ = the mouse in screen-local px. If ret and
        // pan are sampled in sync this is CONSTANT (mouse fixed); any per-frame drift during a
        // scroll = the exact ret-vs-pan desync in px = the bug-2 lag, measured directly.
        static bool g_csv = false;
        static unsigned long g_csv_frame = 0;
        if (g_csv && live)
            spdlog::info("[LAGCSV] {},{:.4f},{:.4f},{:.6f},{:.4f},{:.4f},{:.2f},{:.2f},{:.3f},{:.3f}",
                         g_csv_frame++, v.panX, v.panZ, v.zoom, v.raw[2], v.raw[3], m.x, m.y,
                         v.raw[2] * v.zoom - v.panX, v.raw[3] * v.zoom - v.panZ);

        if (live)
        {
            ImVec2 p = project_uv(v, U, V, io.DisplaySize.x, io.DisplaySize.y);
            // Our projected reticle: cyan ring + crosshair. Should sit on the game
            // reticle (= the magenta mouse cross) once calibrated.
            fg->AddCircle(p, 10.0f, IM_COL32(0, 255, 255, 255), 0, 2.0f);
            fg->AddLine(ImVec2(p.x - 14, p.y), ImVec2(p.x + 14, p.y), IM_COL32(0, 255, 255, 200), 1.0f);
            fg->AddLine(ImVec2(p.x, p.y - 14), ImVec2(p.x, p.y + 14), IM_COL32(0, 255, 255, 200), 1.0f);

            // FIELD-ID DIAG (set PANALPHA=1 with R first so nothing is eased): the game draws
            // its reticle from SOME cursor field; project all three pairs and see which dot
            // lands on the white game reticle during a FAST move. cyan=+0x104, RED=+0xFC,
            // ORANGE=+0x10C. The match = the field the engine renders from (the eased one).
            ImVec2 pF = project_uv(v, v.raw[0], v.raw[1], io.DisplaySize.x, io.DisplaySize.y); // +0xFC
            ImVec2 pT = project_uv(v, v.raw[4], v.raw[5], io.DisplaySize.x, io.DisplaySize.y); // +0x10C
            fg->AddCircle(pF, 7.0f, IM_COL32(255, 60, 60, 255), 0, 2.0f);   // red = +0xFC
            fg->AddCircle(pT, 13.0f, IM_COL32(255, 170, 0, 255), 0, 2.0f);  // orange = +0x10C

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

        // ── PHASE 1: draw real GRACES from MAP_ENTRIES, projected by the same affine.
        // marker axes: U(+0x104)=marker Z=gridZ*256+posZ, V(+0x108)=marker X=gridX*256+posX
        // (G toggles a swap in case the axis order is transposed). Filter to the open
        // page (areaNo == viewArea) so other pages' markers don't bleed in.
        // World-scale: the baked grid*256+pos overshoots the reticle space (~0..10496)
        // by ~2x (gridXNo is half-tiles: "world tile XX = gridXNo/2"), so scale grace
        // world coords into reticle space. Live-tunable [ / ] to dial the exact factor.
        static float g_world_scale = 0.5f;
        static bool g_graces = true;
        // DIAGONAL baseline: render = (world − origin[page]) · scale, K-calibrated.
        // Superseded by the affine g_aff (M·world+T) which captures the rotation the
        // diagonal can't; kept as a fallback (press U to compare). See solve_affine().
        static float g_origin_u[64] = {}, g_origin_v[64] = {};
        static bool g_seeded = false;
        if (!g_seeded)
        {
            g_seeded = true;
            g_origin_u[60] = 5565.f;
            g_origin_u[61] = 301.f;  g_origin_v[61] = -390.f;
        }
        // captured from the solo'd grace this frame, for the K calibration:
        static float g_solo_wU = 0, g_solo_wV = 0;
        static int g_solo_page = -1;
        // Region-by-region test: cycle which single areaNo is drawn (clearer than the
        // whole map at once). Built once from the grace set; -1 index = all.
        namespace gen = goblin::generated;
        static std::vector<int> grace_areas;
        static int area_idx = -1; // -1 = all areas
        if (grace_areas.empty())
        {
            for (const auto &e : goblin::live_graces())
            {
                int a = e.areaNo;
                if (std::find(grace_areas.begin(), grace_areas.end(), a) == grace_areas.end())
                    grace_areas.push_back(a);
            }
            std::sort(grace_areas.begin(), grace_areas.end());
        }
        int area_filter = (area_idx >= 0 && area_idx < (int)grace_areas.size())
                              ? grace_areas[area_idx] : -1;
        // SOLO mode: isolate ONE grace (within the area filter) — draw it big + show
        // its raw data, so a single known grace can be matched against the game icon.
        // O = toggle solo, . / , = next/prev grace.
        static int g_solo = -1;                 // -1 = off (draw all in filter)
        static int g_grace_filtered_count = 0;  // # graces in the current area filter
        if (live && g_graces)
        {
            // REGION GATING (mirrors the game's native areaNo+tab display). 4 map groups
            // = isDLC*2 | isUG: 0 base-overworld {60,61}, 1 base-underground {12}, 2 DLC
            // overworld, 3 DLC underground {40-43}. DLC reuses area 60/61 (projected) but
            // carries a DLC tab (6800-6999) → tab_id is what separates it from base.
            // Classification is data-derived → cache it once (per MAP_ENTRY index), plus a
            // FIXED per-group pivot (stable pan, see fixed-pivot note).
            static bool piv_done = false;
            static float pivX[4] = {0}, pivZ[4] = {0};
            static std::vector<uint8_t> g_greg; // per entry: 0..3 group, 0xFF = not a grace
            if (!piv_done)
            {
                piv_done = true;
                const auto &graces = goblin::live_graces();   // LIVE WorldMapPointParam, no bake
                g_greg.assign(graces.size(), 0xFF);
                double sx[4] = {0}, sz[4] = {0}; int sn[4] = {0};
                for (size_t i = 0; i < graces.size(); ++i)
                {
                    const goblin::LiveGrace &e = graces[i];
                    int ga; float wx, wz;
                    goblin::marker_world_pos(e.areaNo, e.gridXNo, e.gridZNo,
                                             e.posX, e.posZ, ga, wx, wz,
                                             /*conv_underground=*/true);
                    int pg = ga & 63;
                    // DLC vs base by FINAL page (61/40-43 = DLC) — fixes Shadow Keep (area
                    // 21 → page 61, missed by the old tab heuristic). UNDERGROUND is by the
                    // ORIGINAL areaNo (12 / 40-43): area 12 now projects to overworld
                    // map-space (pg=60) so it would mis-classify as overworld — but it's an
                    // UG-layer grace, so gate it by its source areaNo. Catacombs/caves
                    // (areas 30-39) → page 60 = base overworld, correct (game shows them).
                    bool isug  = (e.areaNo == 12) || (e.areaNo >= 40 && e.areaNo <= 43);
                    bool isdlc = (pg == 61) || (e.areaNo >= 40 && e.areaNo <= 43);
                    int grp = (isdlc ? 2 : 0) | (isug ? 1 : 0);
                    g_greg[i] = (uint8_t)grp;
                    sx[grp] += wx; sz[grp] += wz; ++sn[grp];
                }
                for (int g = 0; g < 4; ++g)
                    if (sn[g]) { pivX[g] = (float)(sx[g] / sn[g]); pivZ[g] = (float)(sz[g] / sn[g]); }
            }
            // Which group's map is OPEN — from the SOLVED region getter (probe reads the
            // WorldMapDialog page+layer, commit 3f4ba42): openDlc = DLC map (page==10),
            // underground = layer byte. Replaces the old player_in_dlc()+dead-flag guess
            // (player position ≠ open map; you can open any discovered map).
            int open_grp = (v.openDlc ? 2 : 0) | ((v.underground != 0) ? 1 : 0);
            bool ug_open = (open_grp & 1);
            g_centroidX = pivX[open_grp];
            g_centroidZ = pivZ[open_grp];
            int total = 0, drawn = 0, fidx = 0, culled = 0;
            int drawn_by_area[64] = {0};   // DIAG: original areaNo histogram of drawn graces
            char solo_info[160] = "";
            const auto &graces = goblin::live_graces();   // LIVE WorldMapPointParam, no bake
            for (size_t i = 0; i < graces.size(); ++i)
            {
                const goblin::LiveGrace &e = graces[i];
                if (area_filter >= 0 && e.areaNo != area_filter)
                    continue;
                int myidx = fidx++;
                ++total;
                if (g_solo >= 0 && myidx != g_solo)
                    continue; // solo: skip every grace but the selected one
                // Project the row to UNIFIED overworld coords (legacy dungeons like
                // Stormveil/area-10 are page-local until projected → else they pile up).
                int ga;
                float wx, wz;
                goblin::marker_world_pos(e.areaNo, e.gridXNo, e.gridZNo,
                                         e.posX, e.posZ, ga, wx, wz,
                                         /*conv_underground=*/true);
                int pg = ga & 63;
                if (g_greg[i] != open_grp)
                    continue; // draw only the OPEN map group (base/DLC × OW/UG)
                // world axis → render axis. AFFINE (default): render = M·world + T[pg]
                // (M shared, T per-page; solved in-DLL from P/M calibration). U toggles
                // back to the diagonal origin·scale baseline for comparison. The rotation
                // + pan are per-LAYER (overworld vs underground have different frames).
                float L_rot = ug_open ? g_aff.screen_rot_u : g_aff.screen_rot;
                float L_px  = ug_open ? g_aff.gtx_u : g_aff.gtx;
                float L_py  = ug_open ? g_aff.gty_u : g_aff.gty;
                float gU, gV;
                // DLC UNDERGROUND only (group 3, area 40-43) keeps the eyeball — its page-10
                // converter isn't dumped yet. Everything else uses the EXACT converter:
                // base overworld (60), base underground (12, now unified), AND DLC overworld
                // (61) — the agent dump proved areaNo 60 and 61 have IDENTICAL constants, so
                // DLC overworld is the SAME formula, not a separate one.
                bool dlc_ug = (open_grp == 3);
                if (!dlc_ug)
                {
                    // EXACT world->map-space (agent RE 0a30738: FUN_140876140 + live
                    // CS::WorldMapViewModel dump). origin (7168,16384), bias 128, scale 1.0,
                    // Z-flipped:  mapX = worldX - 7040 ;  mapZ = -worldZ + 16512.
                    gU = wx - 7040.0f;
                    gV = -wz + 16512.0f;
                }
                else if (g_aff.enabled && g_aff.pivot)
                {
                    // rotate about the centroid, placed at render-centre + pan → M only
                    // ROTATES the cloud (in place), never translates it off-screen.
                    const float RC = 5248.f; // render-space centre (10496/2)
                    float dx = wx - g_centroidX, dz = wz - g_centroidZ;
                    float u0 = g_aff.a * dx + g_aff.b * dz; // M (scale/orient) rel. centroid
                    float v0 = g_aff.c * dx + g_aff.d * dz;
                    // REAL rotation, applied in RENDER space (NOT screen) so project_uv's
                    // live pan/zoom tracks on top — screen-space rotation rotated the
                    // pan/zoom response too ("n'importe quoi" on zoom). This is correct.
                    if (L_rot != 0.f)
                    {
                        float rr = L_rot * 3.14159265f / 180.f;
                        float cs = cosf(rr), sn = sinf(rr);
                        float u1 = u0 * cs - v0 * sn, v1 = u0 * sn + v0 * cs;
                        u0 = u1; v0 = v1;
                    }
                    gU = u0 + RC + L_px;
                    gV = v0 + RC + L_py;
                }
                else if (g_aff.enabled)
                {
                    gU = g_aff.a * wx + g_aff.b * wz + g_aff.e[pg] + g_aff.gtx;
                    gV = g_aff.c * wx + g_aff.d * wz + g_aff.f[pg] + g_aff.gty;
                }
                else
                {
                    gU = (wx - g_origin_u[pg]) * g_world_scale;
                    gV = (wz - g_origin_v[pg]) * g_world_scale;
                }
                // (rotation is applied in RENDER space above so pan/zoom track)
                ImVec2 gp = project_uv(v, gU, gV, io.DisplaySize.x, io.DisplaySize.y);
                // RENDER-EASE SHIFT: the game draws the eased frame (+0x10C), we project the
                // target. Shift every grace by the reticle's screen-space ease vector =
                // project(+0x10C) − project(+0x104) = (raw[4]−raw[2])·zoom·scale. Static-marker
                // correct (unlike the centre-ease), cursor-independent, 0 at rest. F/H = gain.
                gp.x += (v.raw[4] - v.raw[2]) * v.zoom * g_calib.scaleX * g_easeGain;
                gp.y += (v.raw[5] - v.raw[3]) * v.zoom * g_calib.scaleY * g_easeGain;
                bool solo = (g_solo >= 0 && myidx == g_solo);
                if (solo) { g_solo_wU = wx; g_solo_wV = wz; g_solo_page = pg; }
                if (!solo && (gp.x < -16 || gp.y < -16 || gp.x > io.DisplaySize.x + 16 ||
                              gp.y > io.DisplaySize.y + 16))
                {
                    ++culled; // ImGui does NOT CPU-cull primitives outside the clip rect (it
                              // still builds their vertices, GPU clips) → culling here saves
                              // the per-marker vertex work. Count it for tuning.
                    continue;
                }
                float r = solo ? 12.0f : 5.0f;
                ImU32 col = solo ? IM_COL32(255, 60, 60, 255) : IM_COL32(90, 230, 130, 235);
                fg->AddCircleFilled(gp, r, col);
                fg->AddCircle(gp, r, IM_COL32(0, 0, 0, 220), 0, 1.5f);
                ++drawn;
                if (e.areaNo < 64) ++drawn_by_area[e.areaNo]; // DIAG histogram
                if (solo)
                    snprintf(solo_info, sizeof(solo_info),
                             "SOLO #%d area=%d page=%d world=(%.0f,%.0f) offset[%d]=(%.0f,%.0f) px=(%.0f,%.0f)  K=calib page",
                             myidx, (int)e.areaNo, pg, wx, wz, pg,
                             g_origin_u[pg], g_origin_v[pg], gp.x, gp.y);
            }
            if (g_solo >= 0)
            {
                fg->AddText(ImVec2(12, 60), IM_COL32(0, 0, 0, 200), solo_info);
                fg->AddText(ImVec2(11, 59), IM_COL32(255, 120, 120, 255), solo_info);
            }
            g_grace_filtered_count = total;
            char gbuf[320];
            snprintf(gbuf, sizeof(gbuf),
                     "GRACES drawn=%d/%d area=%s solo=%s | proj=%s pairs=%d [O=solo ./,=cycle  P=pair M=solve U=toggle Del=clear]",
                     drawn, total, area_filter < 0 ? "ALL" : std::to_string(area_filter).c_str(),
                     g_solo < 0 ? "off" : std::to_string(g_solo).c_str(),
                     g_aff.enabled ? "AFFINE" : "diag", (int)g_cal_pairs.size());
            fg->AddText(ImVec2(12, 44), IM_COL32(0, 0, 0, 200), gbuf);
            fg->AddText(ImVec2(11, 43), IM_COL32(90, 230, 130, 255), gbuf);
            // DIAG: which ORIGINAL areaNos are drawn on this open map (spot UG leaks) +
            // the live open-region read (open_grp / dlc / ug) — all on screen, no logs.
            char abuf[320];
            int ap = snprintf(abuf, sizeof(abuf), "OPEN grp=%d dlc=%d ug=%d | DRAWN area: ",
                              open_grp, (int)v.openDlc, (int)v.underground);
            for (int a = 0; a < 64 && ap < (int)sizeof(abuf) - 16; ++a)
                if (drawn_by_area[a])
                    ap += snprintf(abuf + ap, sizeof(abuf) - ap, "%d:%d ", a, drawn_by_area[a]);
            fg->AddText(ImVec2(12, 76), IM_COL32(0, 0, 0, 200), abuf);
            fg->AddText(ImVec2(11, 75), IM_COL32(255, 220, 120, 255), abuf);
            // Also log it (once per change) so it can be copied from the log file.
            static std::string s_last_hist;
            if (abuf != s_last_hist)
            {
                s_last_hist = abuf;
                spdlog::info("[GRACE-DIAG] open_grp={} (dlc={} ug={}) drawn={}/{} culled-offscreen={}  {}",
                             open_grp, (int)v.openDlc, (int)v.underground, drawn, total, culled, abuf);
            }
        }

        // Hotkeys (work menu-closed): C = 1-point calibrate (solve biasX/Y so the
        // projection of the reticle hits the mouse), L = log a row, X = reset,
        // G = swap grace axes. The model is linear in bias, so one capture pins both.
        // F / H = nudge the pan low-pass alpha (scroll-transition smoothing); R = reset 1.0.
        // (Letter keys: OEM [ ] were unreachable on AZERTY.)
        static bool prevLB = false, prevRB = false, prevBS = false;
        bool downLB = (GetAsyncKeyState('F') & 0x8000) != 0; // convScale -
        bool downRB = (GetAsyncKeyState('H') & 0x8000) != 0; // convScale +
        bool downBS = (GetAsyncKeyState('R') & 0x8000) != 0; // reset 1.0
        if (downLB && !prevLB) { g_easeTau -= 0.005f; if (g_easeTau < 0.0f) g_easeTau = 0.0f; spdlog::info("[EASETAU] {:.4f}", g_easeTau); }
        if (downRB && !prevRB) { g_easeTau += 0.005f; spdlog::info("[EASETAU] {:.4f}", g_easeTau); }
        if (downBS && !prevBS) { g_easeTau = 0.02f; spdlog::info("[EASETAU] reset 0.02"); }
        prevLB = downLB; prevRB = downRB; prevBS = downBS;

        // T / Z = map-update-rate throttle (Hz); 0 = off (every frame).
        static bool prevT = false, prevZ = false;
        bool downT = (GetAsyncKeyState('T') & 0x8000) != 0;
        bool downZ = (GetAsyncKeyState('Z') & 0x8000) != 0;
        if (downT && !prevT) { g_mapFps -= 5.0f; if (g_mapFps < 0.0f) g_mapFps = 0.0f; spdlog::info("[MAPFPS] {:.0f}", g_mapFps); }
        if (downZ && !prevZ) { g_mapFps += 5.0f; spdlog::info("[MAPFPS] {:.0f}", g_mapFps); }
        prevT = downT; prevZ = downZ;

        // I = toggle the terrain-grid diagnostic.
        static bool prevI = false;
        bool downI = (GetAsyncKeyState('I') & 0x8000) != 0;
        if (downI && !prevI) { g_grid = !g_grid; spdlog::info("[GRID] {}", g_grid ? "ON" : "OFF"); }
        prevI = downI;

        static bool prevC = false, prevX = false, prevL = false;
        bool downC = (GetAsyncKeyState('C') & 0x8000) != 0;
        bool downX = (GetAsyncKeyState('X') & 0x8000) != 0;
        bool downL = (GetAsyncKeyState('L') & 0x8000) != 0;

        // V = toggle the per-frame frame-lag CSV capture ([LAGCSV] rows in the log).
        static bool prevV = false;
        bool downV = (GetAsyncKeyState('V') & 0x8000) != 0;
        if (downV && !prevV)
        {
            g_csv = !g_csv;
            spdlog::info("[LAGCSV] capture {} (cols: frame,panX,panZ,zoom,reticleU,reticleV,mouseX,mouseY)",
                         g_csv ? "ON" : "OFF");
        }
        prevV = downV;

        // Y = toggle the projection centre: stable pan-based (fixes markers-follow-reticle)
        // vs the old reticle-coupled +0xFC. Default = pan-based.
        static bool prevY = false;
        bool downY = (GetAsyncKeyState('Y') & 0x8000) != 0;
        if (downY && !prevY) g_pan_center = !g_pan_center;
        prevY = downY;

        // PageDown / PageUp dial the grace world-scale live (physical keys = AZERTY-safe).
        // Hold Shift for fine ±0.001 steps.
        static bool prevPD = false, prevPU = false;
        bool downPD = (GetAsyncKeyState(VK_NEXT) & 0x8000) != 0;   // PageDown
        bool downPU = (GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0;  // PageUp
        float step = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 0.001f : 0.01f;
        if (downPD && !prevPD)
            g_world_scale -= step;
        if (downPU && !prevPU)
            g_world_scale += step;
        prevPD = downPD;
        prevPU = downPU;

        // N / B = cycle the drawn areaNo, A = all (region-by-region test).
        static bool prevN = false, prevB = false, prevA = false;
        bool downN = (GetAsyncKeyState('N') & 0x8000) != 0;
        bool downB = (GetAsyncKeyState('B') & 0x8000) != 0;
        bool downA = (GetAsyncKeyState('A') & 0x8000) != 0;
        if (downN && !prevN && !grace_areas.empty())
            area_idx = (area_idx + 1) % (int)grace_areas.size();
        if (downB && !prevB && !grace_areas.empty())
            area_idx = (area_idx <= 0 ? (int)grace_areas.size() - 1 : area_idx - 1);
        if (downA && !prevA)
            area_idx = -1;
        prevN = downN;
        prevB = downB;
        prevA = downA;

        // O = toggle solo (isolate one grace), . / , = next/prev grace in the filter.
        static bool prevO = false, prevDot = false, prevComma = false;
        bool downO = (GetAsyncKeyState('O') & 0x8000) != 0;
        bool downDot = (GetAsyncKeyState(VK_OEM_PERIOD) & 0x8000) != 0;
        bool downComma = (GetAsyncKeyState(VK_OEM_COMMA) & 0x8000) != 0;
        int fc = g_grace_filtered_count > 0 ? g_grace_filtered_count : 1;
        if (downO && !prevO)
            g_solo = (g_solo < 0) ? 0 : -1;
        if (downDot && !prevDot && g_solo >= 0)
            g_solo = (g_solo + 1) % fc;
        if (downComma && !prevComma && g_solo >= 0)
            g_solo = (g_solo <= 0 ? fc - 1 : g_solo - 1);
        prevO = downO;
        prevDot = downDot;
        prevComma = downComma;

        // K = calibrate the solo'd grace's PAGE origin: put the reticle on the grace's
        // REAL game icon, then origin = world − reticleRender/scale (render=(world−o)·s).
        static bool prevK = false;
        bool downK = (GetAsyncKeyState('K') & 0x8000) != 0;
        if (downK && !prevK && live && g_solo >= 0 && g_solo_page >= 0 &&
            g_world_scale != 0.f)
        {
            g_origin_u[g_solo_page] = g_solo_wU - v.raw[2] / g_world_scale;
            g_origin_v[g_solo_page] = g_solo_wV - v.raw[3] / g_world_scale;
            spdlog::info("[ORIGINCAL] page={} origin=({:.1f},{:.1f}) "
                         "(world=({:.1f},{:.1f}) reticle=({:.1f},{:.1f}))",
                         g_solo_page, g_origin_u[g_solo_page], g_origin_v[g_solo_page],
                         g_solo_wU, g_solo_wV, v.raw[2], v.raw[3]);
        }
        prevK = downK;

        // ── In-DLL AFFINE calibration hotkeys (menu-CLOSED so the reticle is live) ──
        // P = push a (world→render) PAIR: solo a grace (O + ./,), hover its REAL game
        //     icon so the reticle sits on it, press P. Collect ≥3 per page.
        // M = solve the affine from the pairs (shared M + per-page T) and apply it.
        // U = toggle affine vs the diagonal baseline.  Del = clear pairs.
        static bool prevP = false, prevM = false, prevU = false, prevDel = false;
        bool downP = (GetAsyncKeyState('P') & 0x8000) != 0;
        bool downM = (GetAsyncKeyState('M') & 0x8000) != 0;
        bool downU = (GetAsyncKeyState('U') & 0x8000) != 0;
        bool downDel = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
        if (downP && !prevP && live && g_solo >= 0 && g_solo_page >= 0)
        {
            g_cal_pairs.push_back({g_solo_page, g_solo_wU, g_solo_wV, v.raw[2], v.raw[3]});
            spdlog::info("[AFFINE] pair #{} page={} world=({:.1f},{:.1f}) render=({:.1f},{:.1f})",
                         g_cal_pairs.size(), g_solo_page, g_solo_wU, g_solo_wV, v.raw[2], v.raw[3]);
        }
        if (downM && !prevM)
        {
            if (solve_affine(g_aff) > 0) g_aff.enabled = true;
        }
        if (downU && !prevU)
        {
            g_aff.enabled = !g_aff.enabled;
            spdlog::info("[AFFINE] enabled={}", g_aff.enabled);
        }
        if (downDel && !prevDel)
        {
            g_cal_pairs.clear();
            spdlog::info("[AFFINE] pairs cleared");
        }
        prevP = downP; prevM = downM; prevU = downU; prevDel = downDel;

        // J = one-shot dump of EVERY grace's raw row + computed unified world coord,
        // so we can correlate against the reticle's true marker coord (hover a known
        // grace + press L) and reverse the data→marker-space transform.
        static bool prevJ = false;
        bool downJ = (GetAsyncKeyState('J') & 0x8000) != 0;
        if (downJ && !prevJ)
        {
            namespace gen = goblin::generated;
            spdlog::info("[GRACEDUMP] begin (reticle now U={:.1f} V={:.1f} pan=({:.1f},{:.1f}) zoom={:.4f})",
                         v.raw[2], v.raw[3], v.panX, v.panZ, v.zoom);
            int n = 0;
            const auto &dump_graces = goblin::live_graces();
            for (size_t i = 0; i < dump_graces.size() && n < 500; ++i)
            {
                const goblin::LiveGrace &e = dump_graces[i];
                int ga;
                float wx, wz;
                goblin::marker_world_pos(e.areaNo, e.gridXNo, e.gridZNo,
                                         e.posX, e.posZ, ga, wx, wz,
                                         /*conv_underground=*/true);
                spdlog::info("[GRACE] area={} grid=({},{}) pos=({:.1f},{:.1f}) -> world=({:.1f},{:.1f})",
                             (int)e.areaNo, (int)e.gridXNo, (int)e.gridZNo,
                             e.posX, e.posZ, wx, wz);
                ++n;
            }
            spdlog::info("[GRACEDUMP] end ({} graces)", n);
        }
        prevJ = downJ;
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
                ImGui::Text("U(+0x104)=%.1f  V(+0x108)=%.1f   reticle-ctr(+0xFC)=(%.1f,%.1f)",
                            U, V, v.raw[0], v.raw[1]);
                ImGui::Text("pan=(%.1f, %.1f)  zoom=%.4f   centre=%s (Y toggles)",
                            v.panX, v.panZ, v.zoom, g_pan_center ? "PAN (stable)" : "RETICLE (+0xFC)");
                ImGui::Text("projected px = (%.0f, %.0f)   mouse = (%.0f, %.0f)   [%.0fx%.0f]",
                            p.x, p.y, m.x, m.y, io.DisplaySize.x, io.DisplaySize.y);
                ImGui::Text("err = (%.0f, %.0f) px", p.x - m.x, p.y - m.y);
            }
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0, 1, 1, 1), "model: px = U*zoom*sx - panX + bx  (RE: screen = marker*zoom - pan)");
            ImGui::TextWrapped("C = 1-point calibrate (reticle on mouse) -> then PAN+ZOOM and watch the "
                               "cyan ring stay locked. If it drifts, nudge scaleX/scaleY ~1.0. X = reset.");
            ImGui::Separator();
            ImGui::DragFloat("scaleX", &g_calib.scaleX, 0.0005f, 0.5f, 1.5f, "%.5f");
            ImGui::DragFloat("scaleY", &g_calib.scaleY, 0.0005f, 0.5f, 1.5f, "%.5f");
            ImGui::DragFloat("biasX", &g_calib.biasX, 0.5f, -4000.0f, 4000.0f, "%.1f");
            ImGui::DragFloat("biasY", &g_calib.biasY, 0.5f, -4000.0f, 4000.0f, "%.1f");
            if (ImGui::Button("reset calib"))
                g_calib = MarkerCalib{};

            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.5f, 1, 0.6f, 1), "WORLD->RENDER affine: render = M*world + T[page] + pan");
            ImGui::Checkbox("affine enabled (else diagonal baseline)", &g_aff.enabled);
            float det = g_aff.a * g_aff.d - g_aff.b * g_aff.c;
            ImGui::Text("M = [[%.4f, %.4f], [%.4f, %.4f]]  det=%.4f (%s)", g_aff.a, g_aff.b,
                        g_aff.c, g_aff.d, det, det < 0 ? "MIRROR" : "rotation");
            // REAL ROTATION FIELD — separate from scale. Rotates the whole projected
            // marker layer in screen space about the map centre. Use M=diag (pure 0.5
            // scale) + this for a clean 90deg. Buttons step CW; slider for any angle.
            ImGui::TextColored(ImVec4(1, 0.85f, 0.2f, 1), "ROTATION (render, zoom-stable): %.0f deg CW  [user: -90]", g_aff.screen_rot);
            if (ImGui::Button("0"))   g_aff.screen_rot = 0.f;    ImGui::SameLine();
            if (ImGui::Button("90 CW"))  g_aff.screen_rot = 90.f;  ImGui::SameLine();
            if (ImGui::Button("180")) g_aff.screen_rot = 180.f;  ImGui::SameLine();
            if (ImGui::Button("270 CW")) g_aff.screen_rot = 270.f;
            ImGui::DragFloat("rotation deg", &g_aff.screen_rot, 1.f, -360.f, 360.f, "%.0f");
            ImGui::TextDisabled("(tip: set M to 'diag' below, then dial this rotation + pan)");
            // EYEBALL DIAL: rotation presets @ scale 0.5 (one off-diagonal sign each),
            // then pan with gtx/gty until page 60 lines up. If rotate+pan aligns -> it's
            // a clean 90deg rotation; if not -> T differs per page -> use Solve.
            // ORIENTATION = the 8 dihedral signed-permutations (the true map frame is
            // scale·{swap or not}·{sign flips}, NOT an arbitrary angle). Three toggles
            // cover all 8 with NO surprise minus: default (swap on, no negate) = the
            // 0.5/0.5 convention. negX/negZ are what fix a MIRRORED ("wrong direction")
            // cluster. (Edit a,b,c,d below for a fine non-dihedral tweak if ever needed.)
            static float g_oscale = 1.0f; // matches the default M = swap@1.0
            static bool g_swap = true, g_negX = false, g_negZ = false;
            bool ch = false;
            ch |= ImGui::DragFloat("scale", &g_oscale, 0.005f, 0.1f, 1.5f, "%.3f");
            ch |= ImGui::Checkbox("swap axes", &g_swap); ImGui::SameLine();
            ch |= ImGui::Checkbox("negate X", &g_negX); ImGui::SameLine();
            ch |= ImGui::Checkbox("negate Z", &g_negZ);
            if (ch)
            {
                float s = g_oscale, a, b, c, d;
                if (g_swap) { a = 0; b = s; c = s; d = 0; }   // renderX~worldZ, renderZ~worldX
                else        { a = s; b = 0; c = 0; d = s; }   // renderX~worldX, renderZ~worldZ
                if (g_negX) { a = -a; b = -b; }               // flip render X
                if (g_negZ) { c = -c; d = -d; }               // flip render Z
                g_aff.a = a; g_aff.b = b; g_aff.c = c; g_aff.d = d;
            }
            ImGui::TextDisabled("quick presets (scale 0.5):");
            ImGui::SameLine();
            if (ImGui::Button("90 CW"))  { g_aff.a=0; g_aff.b=-0.5f; g_aff.c=0.5f;  g_aff.d=0; }
            ImGui::SameLine();
            if (ImGui::Button("90 CCW")) { g_aff.a=0; g_aff.b=0.5f;  g_aff.c=-0.5f; g_aff.d=0; }
            ImGui::SameLine();
            if (ImGui::Button("mirror"))  { g_aff.a=0; g_aff.b=0.5f;  g_aff.c=0.5f;  g_aff.d=0; }
            ImGui::SameLine();
            if (ImGui::Button("diag"))    { g_aff.a=0.5f; g_aff.b=0; g_aff.c=0; g_aff.d=0.5f; }
            ImGui::DragFloat4("a,b,c,d", &g_aff.a, 0.01f, -2.f, 2.f, "%.3f");
            ImGui::Checkbox("rotate around centroid (eyeball: M spins in place)", &g_aff.pivot);
            ImGui::DragFloat("pan X (gtx)", &g_aff.gtx, 10.f, -12000.f, 12000.f, "%.0f");
            ImGui::DragFloat("pan Y (gty)", &g_aff.gty, 10.f, -12000.f, 12000.f, "%.0f");
            ImGui::Separator();
            {
                bool dlcF = goblin::player_in_dlc();
                int ogF = (dlcF ? 2 : 0) | ((live && v.underground) ? 1 : 0);
                const char *gname[4] = {"base-OW", "base-UG", "DLC-OW", "DLC-UG"};
                ImGui::TextColored(live && v.underground ? ImVec4(1, 0.6f, 0.2f, 1) : ImVec4(0.5f, 0.7f, 1, 1),
                                   "OPEN GROUP: %s  (sublayer DAT=%d, player_in_dlc=%d)",
                                   gname[ogF], live ? v.underground : -1, dlcF);
                ImGui::TextDisabled("  ^ sublayer must flip 0<->non0 when you toggle UG; else flag is wrong");
            }
            ImGui::TextDisabled("underground (page 12 + DLC 40-43) own rotation + pan:");
            ImGui::DragFloat("UG rotation deg", &g_aff.screen_rot_u, 1.f, -360.f, 360.f, "%.0f");
            ImGui::DragFloat("UG pan X", &g_aff.gtx_u, 10.f, -12000.f, 12000.f, "%.0f");
            ImGui::DragFloat("UG pan Y", &g_aff.gty_u, 10.f, -12000.f, 12000.f, "%.0f");
            if (g_aff.pivot)
            {
                // The FINAL bakeable transform render = M'*world + T' (rotation folded in,
                // per the CURRENT layer). M' = R(rot)*M ; T' = RC + pan - M'*pivot. Give me
                // these for the open layer and I bake render=M'*world+T' (no pivot/pan/RC).
                bool ugF = live && v.underground;
                float rotF = ugF ? g_aff.screen_rot_u : g_aff.screen_rot;
                float pxF  = ugF ? g_aff.gtx_u : g_aff.gtx;
                float pyF  = ugF ? g_aff.gty_u : g_aff.gty;
                const float RC = 5248.f;
                float rr = rotF * 3.14159265f / 180.f, cs = cosf(rr), sn = sinf(rr);
                float a1 = cs * g_aff.a - sn * g_aff.c, b1 = cs * g_aff.b - sn * g_aff.d;
                float c1 = sn * g_aff.a + cs * g_aff.c, d1 = sn * g_aff.b + cs * g_aff.d;
                float Te = RC + pxF - (a1 * g_centroidX + b1 * g_centroidZ);
                float Tf = RC + pyF - (c1 * g_centroidX + d1 * g_centroidZ);
                ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1),
                                   "BAKE [%s] pivot=(%.0f,%.0f):", ugF ? "UNDERGROUND" : "overworld",
                                   g_centroidX, g_centroidZ);
                ImGui::Text("  M'=[[%.4f,%.4f],[%.4f,%.4f]]  T'=(%.1f, %.1f)", a1, b1, c1, d1, Te, Tf);
            }
            ImGui::Text("collected pairs: %d", (int)g_cal_pairs.size());
            ImGui::TextWrapped("EYEBALL: pick a preset, then drag pan X/Y until graces sit on the "
                               "game icons (page 60). Aligns by rotate+pan alone => clean 90deg. "
                               "Or capture: solo a grace (O, ./,), hover its real icon, P (>=3/page), Solve.");
            if (ImGui::Button("Solve affine (M)"))
                solve_affine(g_aff);
            ImGui::SameLine();
            if (ImGui::Button("Clear pairs (Del)"))
                g_cal_pairs.clear();
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
