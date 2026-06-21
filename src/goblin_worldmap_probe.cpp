#include "goblin_worldmap_probe.hpp"
#include "goblin_inject.hpp"   // goblin::world_map_open() — gate the scan on map-open
#include "re_signatures.hpp"   // centralized image RVAs

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace goblin::worldmap_probe
{
namespace
{
std::shared_ptr<spdlog::logger> g_log;

// Last active map cursor (the one with a valid WorldMapArea/zoom). Published by
// the probe loop, read by get_live_view() on the render thread for the overlay-
// markers prototype. 0 = no live cursor seen yet / map closed.
std::atomic<uintptr_t> g_active_cursor{0};

// The eldenring.exe image range [base, end). A STATIC/default cursor template
// lives in here with a constant junk view (zoom≈4.88, coord≈0); only the real
// map cursor's WorldMapArea is HEAP-allocated (outside the image). Rejecting
// cursors whose +0xF0 view points inside the image kills that false positive
// (it was thrashing g_active_cursor every tick → first-open lag / jumps).
bool exe_range_inited = false;
uintptr_t g_exe_base = 0, g_exe_end = 0;

void init_exe_range()
{
    if (exe_range_inited)
        return;
    exe_range_inited = true;
    g_exe_base = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!g_exe_base)
        return;
    // PE: e_lfanew @ base+0x3C; SizeOfImage @ base + e_lfanew + 0x50 (PE64).
    uint32_t e_lfanew = *reinterpret_cast<uint32_t *>(g_exe_base + 0x3C);
    uint32_t size_of_image = *reinterpret_cast<uint32_t *>(g_exe_base + e_lfanew + 0x50);
    g_exe_end = g_exe_base + size_of_image;
}

// True if `view` points inside the exe image = the static cursor template, not a
// live heap WorldMapArea.
bool view_is_static(uintptr_t view)
{
    init_exe_range();
    return g_exe_base && view >= g_exe_base && view < g_exe_end;
}

// Cheap sanity gate before dereferencing a read-from-memory pointer (the cursor's
// +0xF0 view can be mid-transition garbage on map/DLC load). SEH is the real net,
// but clang-cl SEH is fragile, so reject implausible pointers up front too.
bool plausible_ptr(uintptr_t p)
{
    return p >= 0x10000 && p < 0x7fffffffffffull && (p & 7) == 0;
}

// CS::WorldMapCursorControl vtable RVA (doc imagebase 0x140000000). The cursor
// object's first qword IS this vtable ptr, so scanning memory for this value
// finds cursor instances directly. Resolve-by-RVA: if a game patch shifted it,
// the scan finds nothing → re-derive from the cursor ctor AOB (doc Goal 2).
constexpr uintptr_t CURSOR_VTABLE_RVA = goblin::sig::CURSOR_VTABLE_RVA;
constexpr ptrdiff_t OFF_X = 0xFC, OFF_Z = 0x104, OFF_Y = 0x10C;
constexpr uintptr_t CURSOR_OFF_IN_MENU = 0x2DB0;
// CSMenuMan static slot (RVA, re_v? Ghidra c843cc3). The world-map cursor =
// WorldMapDialog + 0x2DB0, and the dialog ptr lives somewhere in the first few KB
// of CSMenuMan → a BOUNDED walk (vs the whole-RAM scan) resolves it in O(KB).
constexpr uintptr_t CSMENUMAN_SLOT_RVA = goblin::sig::CSMENUMAN_SLOT_RVA;
constexpr uintptr_t MENU_WALK_WINDOW = 0x10000; // dialog ptr "in the first few KB"; widen to be safe

// PROJECTION transform-scan (docs/world_map_projection_re_findings.md). cursor+0xF0
// points to the CS::WorldMapArea view object; the LIVE viewport is plain floats there:
//   +0x378 = pan (vec2, marker space)   +0x380 = zoom (f32)
//   +0x350..0x35c = STATIC full-map rect [0,0,10496,10496] (sanity: should NOT move)
// The make-or-break test: PAN should sweep +0x378; ZOOM should change +0x380; the
// +0x350 rect should stay put. All reads are SEH-guarded + read-only.
constexpr ptrdiff_t OFF_VIEW_PTR = 0xF0;     // cursor -> WorldMapArea
constexpr ptrdiff_t VIEW_PAN_X = 0x378, VIEW_PAN_Z = 0x37C, VIEW_ZOOM = 0x380;
// Virtual UI canvas singleton: [DAT_1447ef360 + 0x128] -> {+0x110 originX, +0x114
// originY, +0x118 width, +0x11c height}. RVA, patch-fragile (debug only).
constexpr uintptr_t CANVAS_SINGLETON_RVA = goblin::sig::CANVAS_SINGLETON_RVA;

// Safe reads via ReadProcessMemory (NOT __try). clang-cl proved the plain load
// "can't fault" and ELIDED the __try/__except even with __declspec(noinline) — the
// guard was silently dropped and bad pointers faulted unhandled (0xC0000005 at the
// outlined `*out=*src` leaf, decoded from the crash RVA). RPM is an opaque kernel
// call the optimizer can't elide or prove safe: an invalid src returns false, never
// crashes. Slightly slower (a syscall) but bulletproof — fine once the O(KB) walk
// replaces the whole-RAM scan.
inline bool seh_read8(const void *src, uint64_t *out)
{
    SIZE_T n = 0;
    return ReadProcessMemory(GetCurrentProcess(), src, out, sizeof(*out), &n) && n == sizeof(*out);
}
inline bool seh_read4(const void *src, float *out)
{
    SIZE_T n = 0;
    return ReadProcessMemory(GetCurrentProcess(), src, out, sizeof(*out), &n) && n == sizeof(*out);
}
inline bool seh_read_i32(const void *src, int *out)
{
    SIZE_T n = 0;
    return ReadProcessMemory(GetCurrentProcess(), src, out, sizeof(*out), &n) && n == sizeof(*out);
}

// (The whole-address-space vtable scan was removed — it was O(GB), crashed at
// startup, and is superseded by the O(KB) CSMenuMan walk below.)

// O(KB) resolve: CSMenuMan → (bounded walk) → WorldMapDialog → +0x2DB0 = cursor.
// Replaces the whole-address-space vtable scan (Ghidra c843cc3): the dialog pointer
// is in the first few KB of CSMenuMan, and *(dialog + 0x2DB0) == the cursor vtable is
// the ground-truth check. Returns the cursor address, 0 if not found / map closed.
// Logs the winning offset once → can be hardcoded to a true O(1) deref next patch.
uintptr_t resolve_cursor_via_menu(uintptr_t base, uintptr_t vtable_va)
{
    uint64_t mm = 0;
    bool mm_ok = seh_read8(reinterpret_cast<void *>(base + CSMENUMAN_SLOT_RVA), &mm);
    static int diag = 0;
    if (diag < 3)
    {
        ++diag;
        g_log->info("[MENU] walk: CSMenuMan slot {:#x} -> mm={:#x} (read_ok={}, plausible={})",
                    base + CSMENUMAN_SLOT_RVA, mm, mm_ok, mm ? plausible_ptr(mm) : false);
    }
    if (!mm_ok || !mm || !plausible_ptr(mm))
        return 0;
    auto vt_is_cursor = [&](uint64_t obj) {
        uint64_t vt = 0;
        return seh_read8(reinterpret_cast<void *>(obj + CURSOR_OFF_IN_MENU), &vt) && vt == vtable_va;
    };
    // CACHED chain (found once, reused O(1)). off2==~0 → 1-level (mm+off1 = dialog).
    static uintptr_t c_off1 = ~uintptr_t(0), c_off2 = ~uintptr_t(0);
    if (c_off1 != ~uintptr_t(0))
    {
        uint64_t p = 0;
        if (seh_read8(reinterpret_cast<void *>(mm + c_off1), &p) && plausible_ptr(p))
        {
            uint64_t dlg = p;
            if (c_off2 != ~uintptr_t(0))
            {
                uint64_t q = 0;
                if (!seh_read8(reinterpret_cast<void *>(p + c_off2), &q) || !plausible_ptr(q))
                    dlg = 0;
                else
                    dlg = q;
            }
            if (dlg && vt_is_cursor(dlg))
                return dlg + CURSOR_OFF_IN_MENU;
        }
        c_off1 = c_off2 = ~uintptr_t(0); // chain went stale (reopen) → re-search
    }
    // LEVEL 1: the dialog is a flat field of CSMenuMan.
    for (uintptr_t o1 = 0; o1 < MENU_WALK_WINDOW; o1 += 8)
    {
        uint64_t p = 0;
        if (!seh_read8(reinterpret_cast<void *>(mm + o1), &p) || !plausible_ptr(p))
            continue;
        if (vt_is_cursor(p))
        {
            c_off1 = o1; c_off2 = ~uintptr_t(0);
            g_log->info("[MENU] cursor via CSMenuMan+{:#x} (L1) → cursor {:#x} (hardcode O(1))",
                        o1, p + CURSOR_OFF_IN_MENU);
            return p + CURSOR_OFF_IN_MENU;
        }
    }
    // LEVEL 2: the dialog is one deref deeper (mm → container → dialog). EXPENSIVE, so
    // run it only a few times while the map is open (0xCD reaches 7), then give up.
    static int l2_tries = 0;
    if (l2_tries < 6 && goblin::world_map_open())
    {
        ++l2_tries;
        for (uintptr_t o1 = 0; o1 < MENU_WALK_WINDOW; o1 += 8)
        {
            uint64_t p = 0;
            if (!seh_read8(reinterpret_cast<void *>(mm + o1), &p) || !plausible_ptr(p))
                continue;
            for (uintptr_t o2 = 0; o2 < 0x800; o2 += 8)
            {
                uint64_t q = 0;
                if (!seh_read8(reinterpret_cast<void *>(p + o2), &q) || !plausible_ptr(q))
                    continue;
                if (vt_is_cursor(q))
                {
                    c_off1 = o1; c_off2 = o2;
                    g_log->info("[MENU] cursor via CSMenuMan+{:#x} → +{:#x} (L2) → cursor {:#x} "
                                "(hardcode O(1))", o1, o2, q + CURSOR_OFF_IN_MENU);
                    return q + CURSOR_OFF_IN_MENU;
                }
            }
        }
        g_log->info("[MENU] L2 pass {} found no dialog (mm={:#x}) — chain deeper or different struct",
                    l2_tries, mm);
    }
    return 0;
}

// REGION-FIELD finder: the open-map region (overworld / underground / DLC) is NOT
// the sublayer flag (dead) nor fullRect (constant 10496). Delta-scan the WorldMapDialog
// (cursor−0x2DB0) + the WorldMapArea (view) for any int32 holding a SMALL value that
// CHANGES when the user switches maps → that field is the open-region id. Logs each
// change once; the user opens overworld→underground→DLC and reports which offset flips.
void region_diag(uintptr_t cursor, uintptr_t view)
{
    static std::unordered_map<uintptr_t, int> last;
    auto scan = [&](const char *tag, uintptr_t base_obj, int span) {
        for (int off = 0; off <= span; off += 4)
        {
            int val = 0;
            if (!seh_read_i32(reinterpret_cast<void *>(base_obj + off), &val)) continue;
            uintptr_t key = (base_obj == cursor - CURSOR_OFF_IN_MENU ? 0 : 0x80000000ull) + off;
            auto it = last.find(key);
            if (it != last.end() && it->second != val &&
                val >= 0 && val < 256 && it->second >= 0 && it->second < 256)
                g_log->info("[REGION-DIAG] {}+{:#x}: {} -> {}", tag, off, it->second, val);
            last[key] = val;
        }
    };
    scan("dialog", cursor - CURSOR_OFF_IN_MENU, 0x600);
    if (view) scan("view", view, 0x400);
}

// INPUT-DEVICE delta scan: which f32 fields move under MOUSE vs GAMEPAD-STICK?
// Symptom (2026-06-20): markers update on mouse motion but NOT gamepad stick — the
// reticle (+0x104/+0x108) tracks the mouse cursor but seemingly not the gamepad one.
// This logs EVERY f32 in a window on the cursor object AND the WorldMapArea (view) that
// changed since last tick, with its offset. Recipe: open the map, move ONLY the mouse →
// note the offsets logged; then move ONLY the gamepad stick → note the offsets. The field
// the projection must read is one that changes under BOTH (prime suspect: view pan +0x378).
// If NOTHING on this cursor/view changes under the stick, the gamepad drives a different
// object → bring back a bounded all-instance scan. Read-only + SEH-guarded.
void input_delta_scan(uintptr_t cursor, uintptr_t view)
{
    static std::unordered_map<uintptr_t, float> last;
    auto scan = [&](const char *tag, uintptr_t base_obj, int lo, int hi) {
        for (int off = lo; off <= hi; off += 4)
        {
            float val = 0.f;
            if (!seh_read4(reinterpret_cast<void *>(base_obj + off), &val)) continue;
            if (!std::isfinite(val)) continue;
            uintptr_t key = base_obj + off;
            auto it = last.find(key);
            if (it != last.end() && std::fabs(it->second - val) > 0.01f)
                g_log->info("[INPUT-DELTA] {}+{:#05x}: {:.3f} -> {:.3f}", tag, off, it->second, val);
            last[key] = val;
        }
    };
    // cursor coord cluster (reticle/snap fields) + the view's pan/zoom/rect cluster.
    scan("cursor", cursor, 0xE0, 0x160);
    if (view) scan("view", view, 0x340, 0x390);
}

// ── Enumerate ALL world-map cursors reachable from CSMenuMan (find the GAMEPAD one) ──
// Under gamepad the menu-walk (mouse) cursor's reticle is FROZEN while pan moves. If the
// gamepad drives a DIFFERENT WorldMapDialog/cursor than the menu-walk one, it is also
// reachable from CSMenuMan. This is a BOUNDED, SAFE enumeration — only the CSMenuMan
// window (L1 flat + L2 one-deref, exactly like resolve_cursor_via_menu), all via RPM, no
// raw deref. (Replaces an earlier whole-RAM scan that clang-cl miscompiled into an
// out-of-bounds read → crash.) Collects every dialog whose +0x2DB0 == cursor vtable.
std::vector<uintptr_t> g_all_cursors;

void enumerate_menu_cursors(uintptr_t base, uintptr_t vtable_va)
{
    g_all_cursors.clear();
    uint64_t mm = 0;
    if (!seh_read8(reinterpret_cast<void *>(base + CSMENUMAN_SLOT_RVA), &mm) || !plausible_ptr(mm))
        return;
    auto vt_is_cursor = [&](uint64_t obj) {
        uint64_t vt = 0;
        return seh_read8(reinterpret_cast<void *>(obj + CURSOR_OFF_IN_MENU), &vt) && vt == vtable_va;
    };
    auto add = [&](uint64_t dlg) {
        uintptr_t cur = dlg + CURSOR_OFF_IN_MENU;
        for (uintptr_t e : g_all_cursors)
            if (e == cur) return;
        g_all_cursors.push_back(cur);
    };
    for (uintptr_t o1 = 0; o1 < MENU_WALK_WINDOW; o1 += 8)
    {
        uint64_t p = 0;
        if (!seh_read8(reinterpret_cast<void *>(mm + o1), &p) || !plausible_ptr(p))
            continue;
        if (vt_is_cursor(p)) { add(p); continue; }       // L1: dialog is a flat field
        for (uintptr_t o2 = 0; o2 < 0x800; o2 += 8)        // L2: one deref deeper
        {
            uint64_t q = 0;
            if (!seh_read8(reinterpret_cast<void *>(p + o2), &q) || !plausible_ptr(q))
                continue;
            if (vt_is_cursor(q)) add(q);
        }
    }
    g_log->info("[ALLCURSOR] found {} world-map cursor(s) in CSMenuMan window. Move the GAMEPAD "
                "stick — the one whose +0xFC/+0x104 changes (not tagged MOUSE) is the gamepad cursor.",
                g_all_cursors.size());
    for (uintptr_t c : g_all_cursors)
        g_log->info("[ALLCURSOR]   cursor @{:#x} (dialog @{:#x})", c, c - CURSOR_OFF_IN_MENU);
}

void probe_loop()
{
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!base)
    {
        g_log->error("eldenring.exe base not found — probe aborted");
        return;
    }
    uintptr_t vtable_va = base + CURSOR_VTABLE_RVA;
    g_log->info("probe start: eldenring.exe base={:#x}, cursor vtable={:#x} (RVA {:#x})",
                base, vtable_va, CURSOR_VTABLE_RVA);

    // Several objects carry the WorldMapCursorControl vtable; only the ACTIVE map
    // cursor's coords change as you move. So track ALL instances and log each one's
    // coords on change (with its address) — the changing one is the real cursor.
    using clock = std::chrono::steady_clock;
    std::vector<uintptr_t> candidates;
    std::unordered_map<uintptr_t, std::array<float, 3>> last;
    std::unordered_map<uintptr_t, std::array<float, 3>> last_view; // panx/panz/zoom per cursor
    std::unordered_map<uintptr_t, char> bounds_dumped; // one-shot bounds dump per active cursor
    clock::time_point last_scan{};
    int empty_logs = 0;

    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto now = clock::now();

        // Drop a dead active cursor so the sticky publish can re-lock a fresh one.
        if (uintptr_t ac = g_active_cursor.load(std::memory_order_relaxed))
        {
            uint64_t vt;
            if (!seh_read8(reinterpret_cast<void *>(ac), &vt) || vt != vtable_va)
                g_active_cursor.store(0, std::memory_order_relaxed);
        }

        // PRIMARY: the menu walk (CSMenuMan → WorldMapDialog → +0x2DB0 = cursor) is cheap
        // (O(KB)) AND is itself the map-open detector — the dialog only exists while the
        // map is up. Run it EVERY tick, independent of the 0xCD gate (which oscillates
        // 3/7/0 and starved the probe). If it finds the canonical cursor, the map is open.
        uintptr_t menu_cursor = resolve_cursor_via_menu(base, vtable_va);
        if (menu_cursor)
        {
            if (candidates.size() != 1 || candidates[0] != menu_cursor)
            {
                candidates.clear();
                candidates.push_back(menu_cursor);
                g_log->info("resolve: cursor {:#x} (menu walk)", menu_cursor);
            }
            // DISABLED (gamepad-cursor WIP): the all-instance enumerate_menu_cursors scan
            // (L1 0x10000 × L2 0x800 RPM reads) ran once per map-open and slowed the map
            // load noticeably — and it never reliably found the gamepad cursor. Removed; the
            // single menu-walk cursor is used. Re-enable only behind a debug flag if needed.
        }
        else
        {
            // Menu walk found no cursor → the map is closed (no WorldMapDialog) or the
            // dialog offset is outside the walk window. NO whole-RAM scan anymore (it
            // crashed at startup + is O(GB)). Drop the active cursor; the walk re-resolves
            // the instant the map opens.
            if (g_active_cursor.load(std::memory_order_relaxed))
                g_active_cursor.store(0, std::memory_order_relaxed);
            candidates.clear();
            last.clear();
            last_view.clear();
            g_all_cursors.clear(); // re-enumerate next map-open
            continue;
        }

        for (uintptr_t a : candidates)
        {
            uint64_t v;
            if (!seh_read8(reinterpret_cast<void *>(a), &v) || v != vtable_va)
                continue; // instance died (menu closed)
            float x, z, y;
            if (!seh_read4(reinterpret_cast<void *>(a + OFF_X), &x) ||
                !seh_read4(reinterpret_cast<void *>(a + OFF_Z), &z) ||
                !seh_read4(reinterpret_cast<void *>(a + OFF_Y), &y))
                continue;
            // Many objects carry the vtable but are stale/uninitialised (junk:
            // 8e34, NaN, all-zero). The real map cursor has finite, plausible,
            // nonzero coords (the confirmed run was ~3888..6762). Filter to those.
            auto sane = [](float v) { return std::isfinite(v) && std::fabs(v) < 1e5f; };
            if (!sane(x) || !sane(z) || !sane(y) || (x == 0.f && z == 0.f))
                continue;
            auto &l = last[a];
            bool coord_moved = (x != l[0] || z != l[1] || y != l[2]);
            if (coord_moved) // reticle moved → this is (likely) the active cursor
            {
                g_log->info("cursor @{:#x} (menu @{:#x}): +0xFC={:.2f}  +0x104={:.2f}  +0x10C={:.2f}",
                            a, a - CURSOR_OFF_IN_MENU, x, z, y);
                l = {x, z, y};
            }

            // PROJECTION: follow cursor+0xF0 → WorldMapArea and read the LIVE viewport
            // (pan +0x378, zoom +0x380) + the static full-map rect EVERY tick. Log when
            // the reticle moved OR pan/zoom changed — ZOOM centers on the reticle (coord
            // unchanged) and edge-scroll PAN clamps the reticle at the edge (coord frozen
            // while pan sweeps), so gating on coord-move alone misses both. (doc §6)
            // PAN → pan sweeps; ZOOM → zoom changes; +0x350 rect stays [0,0,10496,10496].
            uint64_t view = 0;
            if (seh_read8(reinterpret_cast<void *>(a + OFF_VIEW_PTR), &view) && view &&
                plausible_ptr(view) && !view_is_static(view))
            {
                float panx, panz, zoom, r0, r1, r2, r3;
                if (seh_read4(reinterpret_cast<void *>(view + VIEW_PAN_X), &panx) &&
                    seh_read4(reinterpret_cast<void *>(view + VIEW_PAN_Z), &panz) &&
                    seh_read4(reinterpret_cast<void *>(view + VIEW_ZOOM), &zoom) &&
                    seh_read4(reinterpret_cast<void *>(view + 0x350), &r0) &&
                    seh_read4(reinterpret_cast<void *>(view + 0x354), &r1) &&
                    seh_read4(reinterpret_cast<void *>(view + 0x358), &r2) &&
                    seh_read4(reinterpret_cast<void *>(view + 0x35c), &r3))
                {
                    // Publish this cursor as the active one (valid view + zoom) for
                    // the overlay-markers prototype's get_live_view(). DIAG: log the
                    // 0→addr (and address-change) transition so the first-open latency
                    // can be measured against the map-open moment.
                    // STICKY: several mirror cursor instances carry a valid view+zoom;
                    // publishing "the last valid candidate" each scan made the active
                    // cursor SWITCH mid-session → its view centre jumped → the markers
                    // TELEPORTED (and the eyeball pan shifted per session). Lock onto the
                    // first valid one and keep it until it dies (active_dead clears it).
                    if (zoom != 0.f && !view_is_static(view))
                    {
                        uintptr_t cur = g_active_cursor.load(std::memory_order_relaxed);
                        if (cur == 0 || cur == a)
                        {
                            if (cur != a)
                                g_log->info("[PUBLISH] active cursor @{:#x} (view @{:#x}) zoom={:.4f} "
                                            "coord=({:.1f},{:.1f})", a, view, zoom, x, z);
                            g_active_cursor.store(a, std::memory_order_relaxed);
                        }
                    }

                    auto &lv = last_view[a];
                    bool view_changed = (panx != lv[0] || panz != lv[1] || zoom != lv[2]);
                    if (coord_moved || view_changed)
                    {
                        g_log->info("  VIEW @{:#x}: pan(+0x378)=({:.2f},{:.2f})  zoom(+0x380)={:.4f}"
                                    "  fullRect(+0x350)=[{:.1f},{:.1f},{:.1f},{:.1f}]",
                                    view, panx, panz, zoom, r0, r1, r2, r3);
                        lv = {panx, panz, zoom};
                    }
                    region_diag(a, view); // find the open-map region field (switch maps → flips)
                    input_delta_scan(a, view); // which f32 moves under mouse vs gamepad stick?
                }

                // One-shot per active cursor: the full float window cursor+0xE0..+0x388
                // (covers both rects + pan + zoom) + the virtual canvas (1920×1080?).
                if ((x != 0.f || z != 0.f) && !bounds_dumped.count(a))
                {
                    bounds_dumped[a] = 1;
                    g_log->info("--- window dump @{:#x} (rects, pan@+0x378, zoom@+0x380) ---", a);
                    for (ptrdiff_t off = 0xE0; off <= 0x388; off += 4)
                    {
                        float fv;
                        if (seh_read4(reinterpret_cast<void *>(a + off), &fv))
                            g_log->info("  +{:#05x} = {:.3f}", off, fv);
                    }
                    if (view)
                        for (ptrdiff_t off = 0x340; off <= 0x388; off += 4)
                        {
                            float fv;
                            if (seh_read4(reinterpret_cast<void *>(view + off), &fv))
                                g_log->info("  VIEW+{:#05x} = {:.3f}", off, fv);
                        }
                    // Virtual UI canvas: expect width≈1920, height≈1080.
                    uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
                    uint64_t canvas = 0;
                    if (exe && seh_read8(reinterpret_cast<void *>(exe + CANVAS_SINGLETON_RVA), &canvas) &&
                        canvas)
                    {
                        uint64_t c2 = 0;
                        if (seh_read8(reinterpret_cast<void *>(canvas + 0x128), &c2) && c2)
                        {
                            float ox, oy, w, h;
                            if (seh_read4(reinterpret_cast<void *>(c2 + 0x110), &ox) &&
                                seh_read4(reinterpret_cast<void *>(c2 + 0x114), &oy) &&
                                seh_read4(reinterpret_cast<void *>(c2 + 0x118), &w) &&
                                seh_read4(reinterpret_cast<void *>(c2 + 0x11c), &h))
                                g_log->info("  CANVAS: origin=({:.1f},{:.1f})  size=({:.1f},{:.1f})", ox, oy, w, h);
                        }
                    }
                }
            }
        }
    }
}
} // namespace

uintptr_t debug_active_cursor() { return g_active_cursor.load(std::memory_order_relaxed); }

bool get_live_view(LiveView &out)
{
    uintptr_t a = g_active_cursor.load(std::memory_order_relaxed);
    if (!a)
        return false;
    static uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!base)
        return false;
    uint64_t vt = 0, view = 0;
    // Confirm the cached address is still a live cursor (menu may have closed).
    if (!seh_read8(reinterpret_cast<void *>(a), &vt) || vt != base + CURSOR_VTABLE_RVA)
        return false;
    if (!seh_read8(reinterpret_cast<void *>(a + OFF_VIEW_PTR), &view) || !view ||
        !plausible_ptr(view) || view_is_static(view))
        return false;
    if (!seh_read4(reinterpret_cast<void *>(a + OFF_X), &out.cursorX) ||
        !seh_read4(reinterpret_cast<void *>(a + OFF_Z), &out.cursorZ) ||
        !seh_read4(reinterpret_cast<void *>(view + VIEW_PAN_X), &out.panX) ||
        !seh_read4(reinterpret_cast<void *>(view + VIEW_PAN_Z), &out.panZ) ||
        !seh_read4(reinterpret_cast<void *>(view + VIEW_ZOOM), &out.zoom) || out.zoom == 0.f)
        return false;
    // Cursor/snap bounds rect on the view (view+0x340..0x34c) → cursor-independent centre.
    if (!seh_read4(reinterpret_cast<void *>(view + 0x340), &out.snapMinX) ||
        !seh_read4(reinterpret_cast<void *>(view + 0x344), &out.snapMinZ) ||
        !seh_read4(reinterpret_cast<void *>(view + 0x348), &out.snapMaxX) ||
        !seh_read4(reinterpret_cast<void *>(view + 0x34c), &out.snapMaxZ))
    {
        out.snapMinX = out.snapMinZ = out.snapMaxX = out.snapMaxZ = 0.f;
    }
    // diag window cursor+0xFC..+0x118 (find which offset drives the vertical axis)
    for (int i = 0; i < 8; ++i)
        if (!seh_read4(reinterpret_cast<void *>(a + 0xFC + i * 4), &out.raw[i]))
            out.raw[i] = 0.f;
    // Snap-rect midpoint (view +0x340 minX / +0x344 minZ / +0x348 maxX / +0x34c maxZ) →
    // device-independent view centre = (pan + snapMid)/zoom (NOT the reticle).
    out.snapMidX = out.snapMidZ = 0.f;
    {
        float sminx, sminz, smaxx, smaxz;
        if (seh_read4(reinterpret_cast<void *>(view + 0x340), &sminx) &&
            seh_read4(reinterpret_cast<void *>(view + 0x344), &sminz) &&
            seh_read4(reinterpret_cast<void *>(view + 0x348), &smaxx) &&
            seh_read4(reinterpret_cast<void *>(view + 0x34c), &smaxz))
        {
            out.snapMidX = (sminx + smaxx) * 0.5f;
            out.snapMidZ = (sminz + smaxz) * 0.5f;
        }
    }
    // WorldMapArea+0x6e (i32) = areaNo of the open page (doc §3) → page filter.
    out.viewArea = -1;
    seh_read_i32(reinterpret_cast<void *>(view + 0x6e), &out.viewArea);
    // Open-map region — SOLVED RE (commit 3f4ba42, docs/windows_open_map_region_re_prompt.md).
    // The dead DAT_143d6cfc3 flag (render-only/transient) is replaced by two O(1) reads off
    // the WorldMapDialog (= cursor − 0x2DB0):
    //   page  = *(int*)(dialog + 0xA88)                       0 = base, 10 = DLC
    //   layer = *(uint8_t*)( *(void**)(dialog + 0x2B68) + 0xB8 )  0 = surface, 1 = underground
    // (underground is applied internally as page+1, not stored in `page`.) Gate:
    //   page==10 → DLC ; else layer==0 → overworld ; else underground.
    out.underground = 0;
    out.openDlc = 0;
    uintptr_t dialog = a - CURSOR_OFF_IN_MENU;
    int page = 0;
    if (seh_read_i32(reinterpret_cast<void *>(dialog + 0xA88), &page))
        out.openDlc = (page == 10) ? 1 : 0;
    uint64_t layer_obj = 0;
    if (seh_read8(reinterpret_cast<void *>(dialog + 0x2B68), &layer_obj) && layer_obj)
    {
        int layer = 0;
        if (seh_read_i32(reinterpret_cast<void *>(layer_obj + 0xB8), &layer))
            out.underground = layer & 0xFF; // the layer state is a byte
    }
    return true;
}

void initialize(const std::filesystem::path &log_path)
{
    try
    {
        g_log = spdlog::basic_logger_mt("mfg-wmprobe", log_path.string(), true);  // truncate; prev sessions archived
        g_log->set_pattern("[%H:%M:%S.%e] %v");
        g_log->flush_on(spdlog::level::info);
    }
    catch (const std::exception &e)
    {
        spdlog::error("[WM-PROBE] could not open log {}: {}", log_path.string(), e.what());
        return;
    }
    std::thread(probe_loop).detach();
    spdlog::info("[WM-PROBE] world-map cursor probe active → {}", log_path.string());
}

// Mid-session resolution diagnostic (RE docs/re/windows_midsession_resolution_swapchain_re_findings.md).
// Walks the render-output list at DAT_1447ef360+0x128..+0x130 (stride 0x170) and logs each
// entry's active float dims (+0x118/+0x11c) + dirty bit (+0x140) against the live backbuffer.
// Self-contained + RPM-guarded (no probe init needed); logs to the main MapForGoblins.log.
void dump_render_dims(float bbW, float bbH)
{
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!base) { spdlog::info("[RENDIMS] no eldenring.exe base"); return; }
    constexpr uintptr_t RENDER_MGR_SLOT_RVA = 0x47ef360; // DAT_1447ef360 (RE 3ce2b18)
    uint64_t mgr = 0;
    if (!seh_read8(reinterpret_cast<void *>(base + RENDER_MGR_SLOT_RVA), &mgr) || mgr < 0x10000)
    {
        spdlog::info("[RENDIMS] render-mgr slot (rva {:#x}) unread/null (mgr={:#x})",
                     RENDER_MGR_SLOT_RVA, mgr);
        return;
    }
    uint64_t begin = 0, end = 0;
    seh_read8(reinterpret_cast<void *>(mgr + 0x128), &begin);
    seh_read8(reinterpret_cast<void *>(mgr + 0x130), &end);
    int mgrDirty = 0;
    seh_read_i32(reinterpret_cast<void *>(mgr + 0xeb8), &mgrDirty);
    spdlog::info("[RENDIMS] backbuffer={}x{} mgr={:#x} list=[{:#x}..{:#x}] mgrDirty(+0xeb8)={}",
                 bbW, bbH, mgr, begin, end, mgrDirty & 0xff);
    if (begin < 0x10000 || end < begin || (end - begin) > 0x4000)
    {
        spdlog::info("[RENDIMS] output-list ptrs implausible — abort walk");
        return;
    }
    int i = 0;
    for (uint64_t e = begin; e < end && i < 16; e += 0x170, ++i)
    {
        float w = 0, h = 0;
        int outIdx = 0, dirty = 0;
        seh_read4(reinterpret_cast<void *>(e + 0x118), &w);
        seh_read4(reinterpret_cast<void *>(e + 0x11c), &h);
        seh_read_i32(reinterpret_cast<void *>(e + 0x128), &outIdx);
        seh_read_i32(reinterpret_cast<void *>(e + 0x140), &dirty);
        // Source dims + render-scale (mgr + outIdx*0x30 + 0x3c8/0x3cc/0x3ec): FUN_1419ebb40
        // recomputes the active dims FROM these. If source is fresh → fix(1) recompute is
        // enough; if source is also stale → fix(2) full re-apply (updates source first).
        int srcW = 0, srcH = 0;
        float rscale = 0;
        uint64_t srcBase = mgr + static_cast<uint64_t>(static_cast<uint32_t>(outIdx)) * 0x30;
        seh_read_i32(reinterpret_cast<void *>(srcBase + 0x3c8), &srcW);
        seh_read_i32(reinterpret_cast<void *>(srcBase + 0x3cc), &srcH);
        seh_read4(reinterpret_cast<void *>(srcBase + 0x3ec), &rscale);
        bool stale = (w != bbW || h != bbH);
        spdlog::info("[RENDIMS]  entry#{} outIdx={} activeWH={}x{} srcWH={}x{} rscale={} "
                     "dirty(+0x140)={} {}",
                     i, outIdx, w, h, srcW, srcH, rscale, dirty & 0xff,
                     stale ? "<-- STALE vs backbuffer" : "ok");
    }
}

} // namespace goblin::worldmap_probe
