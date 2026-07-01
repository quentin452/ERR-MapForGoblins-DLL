#include "input_rawinput.hpp"
#include "input_shared.hpp"

#include <atomic>
#include <cstdint>

#include <MinHook.h>
#include <spdlog/spdlog.h>
#include <imgui.h>

namespace goblin::input
{
namespace
{
using GetRawInputDataFn = UINT(WINAPI *)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using GetRawInputBufferFn = UINT(WINAPI *)(PRAWINPUT, PUINT, UINT);

GetRawInputDataFn o_get_raw_input_data = nullptr;
GetRawInputBufferFn o_get_raw_input_buffer = nullptr;

std::atomic<unsigned> g_diag_get_raw_input_data{0};
std::atomic<unsigned> g_diag_get_raw_input_buffer{0};

// ROOT CAUSE, confirmed live by <user> 2026-07-01 via the [DIAG] crosshairs (goblin_overlay.cpp):
// the raw OS cursor (GetCursorPos) reads as permanently frozen — center from the very first F1
// open (not just after Alt+Tab), and after an Alt+Tab it freezes wherever it was at the moment
// of the transition instead of resuming. Windows/Wine simply isn't updating the absolute cursor
// position at all while this game holds raw-input mouse capture — GetCursorPos can never be
// trusted here, focus transition or not.
//
// Fix: track our OWN virtual absolute cursor by accumulating the SAME raw mouse deltas the
// game's own camera already relies on (captured in the raw-input hooks below, right before we
// blank them for the game) — this is data proven to keep working across Alt+Tab (the user's
// camera control itself isn't reported broken), unlike GetCursorPos. Seeded once at first use to
// screen centre (matches where the frozen GetCursorPos reads anyway) since a real starting
// position isn't obtainable from anything reliable.
//
// NOTE (2026-07-01, cursor-hooks slice): this virtual cursor turned out to be diagnosing a
// problem that didn't actually exist — the real fix was the g_imgui_reading_cursor exemption in
// input_cursor.cpp's hk_get_cursor_pos. Kept only as the [DIAG] on-screen comparison value.
std::atomic<float> g_virtual_cursor_x{0.f};
std::atomic<float> g_virtual_cursor_y{0.f};
std::atomic<bool> g_virtual_cursor_seeded{false};

void accumulate_virtual_cursor(LONG dx, LONG dy, USHORT flags)
{
    ImGuiIO &io = ImGui::GetIO();
    // <user> 2026-07-01: the seed/pivot point was inconsistent across launches — sometimes
    // near screen centre, sometimes near the top. Root cause: this can fire before
    // io.DisplaySize is populated from the swapchain (a timing race, not guaranteed to have
    // run yet the very first time raw input arrives) — the old code fell back to a
    // HARDCODED 1920x1080 guess in that case, seeding at (960,540) regardless of the real
    // resolution. On any non-1920x1080 display that's not the real centre at all, landing
    // wherever 540px happens to fall on the actual screen (e.g. visibly "near the top" on a
    // taller display). Fix: refuse to seed (or accumulate) until DisplaySize is verified
    // valid — retried on the next raw input event instead of guessing.
    if (io.DisplaySize.x <= 0.f || io.DisplaySize.y <= 0.f)
        return;
    const float dispW = io.DisplaySize.x;
    const float dispH = io.DisplaySize.y;
    if (!g_virtual_cursor_seeded.exchange(true, std::memory_order_relaxed))
    {
        g_virtual_cursor_x.store(dispW * 0.5f, std::memory_order_relaxed);
        g_virtual_cursor_y.store(dispH * 0.5f, std::memory_order_relaxed);
    }
    if (flags & MOUSE_MOVE_ABSOLUTE)
    {
        // Rare (VM/tablet input): lLastX/Y are already normalized 0..65535 absolute coords.
        g_virtual_cursor_x.store((static_cast<float>(dx) / 65535.f) * dispW, std::memory_order_relaxed);
        g_virtual_cursor_y.store((static_cast<float>(dy) / 65535.f) * dispH, std::memory_order_relaxed);
        return;
    }
    // <user> 2026-07-01: virtual cursor drifted away from the real mouse the farther it
    // moved (worse near the bottom of the screen than the top in their testing — consistent
    // with error growing with total travel, not a fixed offset). Root cause: raw input
    // lLastX/lLastY are raw hardware "mickeys", NOT screen pixels — feeding them 1:1 assumed
    // a mapping that doesn't hold. Scale by the user's actual Windows pointer-speed setting
    // (SPI_GETMOUSESPEED, 1..20, Windows default 10 == "1 mickey per pixel" baseline) instead
    // of guessing a constant — self-adjusts to their real OS config. Doesn't replicate
    // Windows' full non-linear "enhance pointer precision" acceleration curve if that's
    // enabled; a linear approximation is a large improvement over the prior flat 1:1 either
    // way and doesn't need another calibration round-trip.
    static float s_speedScale = []() {
        int mouseSpeed = 10;
        ::SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &mouseSpeed, 0);
        return static_cast<float>(mouseSpeed) / 10.0f;
    }();  // queried once (not per-event -- this can fire many times/frame during fast
          // movement) since the OS pointer-speed setting essentially never changes mid-session
    float nx = g_virtual_cursor_x.load(std::memory_order_relaxed) + static_cast<float>(dx) * s_speedScale;
    float ny = g_virtual_cursor_y.load(std::memory_order_relaxed) + static_cast<float>(dy) * s_speedScale;
    nx = nx < 0.f ? 0.f : (nx > dispW ? dispW : nx);
    ny = ny < 0.f ? 0.f : (ny > dispH ? dispH : ny);
    g_virtual_cursor_x.store(nx, std::memory_order_relaxed);
    g_virtual_cursor_y.store(ny, std::memory_order_relaxed);
}

// Raw input — ER reads gameplay keyboard/mouse here (not via window messages), so we
// neutralise it while the menu is open to fully disable game commands. ImGui still gets
// keyboard/mouse via the WndProc + cursor hooks.
UINT WINAPI hk_get_raw_input_data(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdr)
{
    g_diag_get_raw_input_data.fetch_add(1, std::memory_order_relaxed);
    UINT ret = o_get_raw_input_data(h, cmd, data, size, hdr);
    // While the menu is open, blank the raw event so the game sees no mouse
    // movement / clicks / key presses. (ImGui's input comes from the
    // WndProc, not from here, so the panel stays fully usable.)
    if (menu_open() && data && cmd == RID_INPUT)
    {
        auto *ri = reinterpret_cast<RAWINPUT *>(data);
        if (ri->header.dwType == RIM_TYPEMOUSE)
        {
            // Capture the REAL delta for our own virtual-cursor tracking (see
            // accumulate_virtual_cursor's comment) before it gets blanked below for the game.
            accumulate_virtual_cursor(ri->data.mouse.lLastX, ri->data.mouse.lLastY,
                                      ri->data.mouse.usFlags);
            // During an item-search nav, feed a 1px net-zero (±1 alternating) delta so the game
            // keeps stepping/rendering its world map (which is otherwise frozen by the input blank)
            // — lets our page/layer switch + pan actually take effect with the F1 panel open. The
            // map cursor jitters 1px (negligible, nets to zero); clicks/keys stay suppressed.
            if (nav_frames_active() > 0)
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
    g_diag_get_raw_input_buffer.fetch_add(1, std::memory_order_relaxed);
    // Always call through for the real data first — needed for our own virtual-cursor
    // tracking (see accumulate_virtual_cursor's comment). Previously this short-circuited to
    // 0 immediately on a real read while the menu was open, giving us zero visibility into
    // deltas the game's own camera was still receiving via this exact API (confirmed the game
    // uses this batched path, not the singular GetRawInputData, by [CURSORDIAG]'s
    // raw_input_buffer counter being the one that's consistently nonzero).
    UINT n = o_get_raw_input_buffer(data, size, hdr);
    if (menu_open() && data != nullptr && n != static_cast<UINT>(-1) && n > 0)
    {
        PRAWINPUT ri = data;
        for (UINT i = 0; i < n; ++i)
        {
            if (ri->header.dwType == RIM_TYPEMOUSE)
                accumulate_virtual_cursor(ri->data.mouse.lLastX, ri->data.mouse.lLastY,
                                          ri->data.mouse.usFlags);
            // NEXTRAWINPUTBLOCK expands to a QWORD-based alignment macro that isn't visible
            // with this project's xwin/clang-cl SDK headers — inlined equivalent (8-byte
            // align, matching RAWINPUT_ALIGN's own definition) instead of fighting the include.
            {
                const uint64_t next = (reinterpret_cast<uint64_t>(reinterpret_cast<uint8_t *>(ri) + ri->header.dwSize) + 7ull) & ~7ull;
                ri = reinterpret_cast<PRAWINPUT>(next);
            }
        }
    }
    // Batched raw input. While the menu is open, report zero buffered events
    // for actual reads (data != null); pass size-queries through so the
    // game's buffer sizing stays correct.
    if (menu_open() && data != nullptr) return 0;
    return n;
}
} // namespace

void install_rawinput_hooks()
{
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (!u32)
    {
        spdlog::error("[OVERLAY] user32.dll not found — raw input hooks skipped");
        return;
    }
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
    hook_u32("GetRawInputData", reinterpret_cast<void *>(&hk_get_raw_input_data),
             reinterpret_cast<void **>(&o_get_raw_input_data));
    hook_u32("GetRawInputBuffer", reinterpret_cast<void *>(&hk_get_raw_input_buffer),
             reinterpret_cast<void **>(&o_get_raw_input_buffer));
}

float virtual_cursor_x() { return g_virtual_cursor_x.load(std::memory_order_relaxed); }
float virtual_cursor_y() { return g_virtual_cursor_y.load(std::memory_order_relaxed); }

unsigned diag_get_raw_input_data_exchange() { return g_diag_get_raw_input_data.exchange(0, std::memory_order_relaxed); }
unsigned diag_get_raw_input_buffer_exchange() { return g_diag_get_raw_input_buffer.exchange(0, std::memory_order_relaxed); }
} // namespace goblin::input
