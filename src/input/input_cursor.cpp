#include "input_cursor.hpp"
#include "input_shared.hpp"

#include <atomic>

#include <MinHook.h>
#include <spdlog/spdlog.h>

namespace goblin::input
{
namespace
{
using SetCursorPosFn = BOOL(WINAPI *)(int, int);
using ClipCursorFn = BOOL(WINAPI *)(const RECT *);
using GetCursorPosFn = BOOL(WINAPI *)(LPPOINT);

SetCursorPosFn o_set_cursor_pos = nullptr;
ClipCursorFn o_clip_cursor = nullptr;
GetCursorPosFn o_get_cursor_pos = nullptr;

// The 2D world map cursor follows the absolute OS cursor via GetCursorPos (a different path
// from the 3D camera's DirectInput). We can't freeze it globally — ImGui needs the real
// position. So return a frozen position to the GAME, the real one to ImGui (flag set only
// around ImGui's NewFrame / our own mouse-poll block).
bool g_imgui_reading_cursor = false;

// diag (docs/re/proton11_cursor_lock_re_prompt.md step 1): call counts for these 3 detours,
// logged once/sec (alongside the raw-input detours' own counters) while the panel is open.
std::atomic<unsigned> g_diag_set_cursor_pos{0};
// Separate counter for the live [DIAG] on-screen readout (config::debugCursorDiagnostic) —
// NOT the same one [CURSORDIAG] resets every ~1s in dump-to-log, so reading/resetting this
// one every frame doesn't starve that log's own accounting.
std::atomic<unsigned> g_diag_set_cursor_pos_live{0};
std::atomic<int> g_diag_last_set_cursor_pos_x{-1};
std::atomic<int> g_diag_last_set_cursor_pos_y{-1};
std::atomic<bool> g_diag_set_cursor_pos_swallowed{false};
std::atomic<unsigned> g_diag_clip_cursor{0};
std::atomic<unsigned> g_diag_get_cursor_pos{0};

// ── Cursor hooks (free the OS cursor while the menu is open) ──────────
BOOL WINAPI hk_set_cursor_pos(int x, int y)
{
    g_diag_set_cursor_pos.fetch_add(1, std::memory_order_relaxed);
    g_diag_set_cursor_pos_live.fetch_add(1, std::memory_order_relaxed);
    g_diag_last_set_cursor_pos_x.store(x, std::memory_order_relaxed);
    g_diag_last_set_cursor_pos_y.store(y, std::memory_order_relaxed);
    if (menu_open())
    {
        g_diag_set_cursor_pos_swallowed.store(true, std::memory_order_relaxed);
        return TRUE;                    // swallow the game's recenter-to-middle
    }
    g_diag_set_cursor_pos_swallowed.store(false, std::memory_order_relaxed);
    return o_set_cursor_pos(x, y);
}
BOOL WINAPI hk_clip_cursor(const RECT *rc)
{
    g_diag_clip_cursor.fetch_add(1, std::memory_order_relaxed);
    if (menu_open()) return o_clip_cursor(nullptr);   // unclip while menu is up
    return o_clip_cursor(rc);
}

BOOL WINAPI hk_get_cursor_pos(LPPOINT p)
{
    g_diag_get_cursor_pos.fetch_add(1, std::memory_order_relaxed);
    BOOL r = o_get_cursor_pos(p);
    // Freeze the cursor the GAME sees while the menu is open (so the 2D map
    // stops following it). ImGui's own NewFrame read gets the real position.
    // Report the SCREEN CENTRE (not the open-time point): the map pans on
    // (cursor - centre) delta, so a static off-centre point = a constant
    // non-zero delta = the map drifts forever (softlock). Centre = zero
    // delta = no pan.
    if (menu_open() && !g_imgui_reading_cursor && p)
    {
        p->x = GetSystemMetrics(SM_CXSCREEN) / 2;
        p->y = GetSystemMetrics(SM_CYSCREEN) / 2;
        // ER drives the 2D map camera off GetCursorPos. While a search-locate / page-switch is in
        // flight, jitter the reported cursor ±1px (net zero) so the game keeps STEPPING its map
        // (the per-frame c32f0 step, where our locate reticle-write + page switch apply) with the F1
        // panel still open. 1px = no drift. (Centring itself is driven on the game thread via the
        // c32f0 reticle-target write, not from this cursor position.)
        if (nav_frames_active() > 0)
        {
            static int s_cjit = 0;
            s_cjit ^= 1;
            p->x += s_cjit ? 1 : -1;
        }
    }
    return r;
}
} // namespace

void install_cursor_hooks()
{
    // Cursor hooks (user32) — let the OS cursor move freely over the menu by
    // neutralising the game's per-frame recenter/clip while the panel is open.
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (!u32)
    {
        spdlog::error("[OVERLAY] user32.dll not found — cursor hooks skipped");
        return;
    }
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
    hook_u32("GetCursorPos", reinterpret_cast<void *>(&hk_get_cursor_pos),
             reinterpret_cast<void **>(&o_get_cursor_pos));
}

BOOL set_cursor_pos_real(int x, int y)
{
    return o_set_cursor_pos ? o_set_cursor_pos(x, y) : FALSE;
}

bool clip_cursor_hooked() { return o_clip_cursor != nullptr; }

BOOL clip_cursor_real(const RECT *rc)
{
    return o_clip_cursor ? o_clip_cursor(rc) : FALSE;
}

BOOL get_cursor_pos_real(LPPOINT p)
{
    return o_get_cursor_pos ? o_get_cursor_pos(p) : GetCursorPos(p);
}

void set_imgui_reading_cursor(bool v) { g_imgui_reading_cursor = v; }

unsigned diag_set_cursor_pos_exchange() { return g_diag_set_cursor_pos.exchange(0, std::memory_order_relaxed); }
unsigned diag_clip_cursor_exchange() { return g_diag_clip_cursor.exchange(0, std::memory_order_relaxed); }
unsigned diag_get_cursor_pos_exchange() { return g_diag_get_cursor_pos.exchange(0, std::memory_order_relaxed); }
unsigned diag_set_cursor_pos_live_exchange() { return g_diag_set_cursor_pos_live.exchange(0, std::memory_order_relaxed); }
int diag_last_set_cursor_pos_x() { return g_diag_last_set_cursor_pos_x.load(std::memory_order_relaxed); }
int diag_last_set_cursor_pos_y() { return g_diag_last_set_cursor_pos_y.load(std::memory_order_relaxed); }
bool diag_set_cursor_pos_swallowed() { return g_diag_set_cursor_pos_swallowed.load(std::memory_order_relaxed); }
} // namespace goblin::input
