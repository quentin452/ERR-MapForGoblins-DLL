#include "goblin_worldmap_probe.hpp"
#include "goblin_inject.hpp"   // goblin::world_map_open() — gate the scan on map-open

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

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
constexpr uintptr_t CURSOR_VTABLE_RVA = 0x2b29a90;
constexpr ptrdiff_t OFF_X = 0xFC, OFF_Z = 0x104, OFF_Y = 0x10C;
constexpr uintptr_t CURSOR_OFF_IN_MENU = 0x2DB0;
// CSMenuMan static slot (RVA, re_v? Ghidra c843cc3). The world-map cursor =
// WorldMapDialog + 0x2DB0, and the dialog ptr lives somewhere in the first few KB
// of CSMenuMan → a BOUNDED walk (vs the whole-RAM scan) resolves it in O(KB).
constexpr uintptr_t CSMENUMAN_SLOT_RVA = 0x3d6b7b0;
constexpr uintptr_t MENU_WALK_WINDOW = 0x8000; // dialog ptr "in the first few KB"; widen to be safe

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
constexpr uintptr_t CANVAS_SINGLETON_RVA = 0x47ef360;

// SEH-guarded reads. __declspec(noinline) is REQUIRED: if the compiler inlines
// these, clang-cl merges/hoists the raw load out of the __try region and the SEH
// guard is silently lost → a bad pointer faults unhandled (observed: a 0xC0000005
// crash reading view+0x378 on DLC map entry, where cursor+0xF0 was mid-transition
// garbage). Keeping them as real calls keeps each __try self-contained.
__declspec(noinline) bool seh_read8(const void *src, uint64_t *out)
{
    __try { *out = *reinterpret_cast<const volatile uint64_t *>(src); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
__declspec(noinline) bool seh_read4(const void *src, float *out)
{
    __try { *out = *reinterpret_cast<const volatile float *>(src); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
__declspec(noinline) bool seh_read_i32(const void *src, int *out)
{
    __try { *out = *reinterpret_cast<const volatile int *>(src); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Scan one region for the vtable qword (8-aligned). Region-level SEH so a page
// that faults mid-scan just aborts this region. POD-only inside __try.
uintptr_t scan_region(uintptr_t base, size_t size, uintptr_t vtable_va)
{
    uintptr_t hit = 0;
    __try
    {
        uintptr_t end = base + size;
        for (uintptr_t p = base; p + 8 <= end; p += 8)
            if (*reinterpret_cast<const volatile uint64_t *>(p) == vtable_va) { hit = p; break; }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { hit = 0; }
    return hit;
}

// Collect ALL addresses holding the cursor vtable (there are several instances;
// only the active map cursor's coords change as you move). Region-level SEH.
void scan_region_all(uintptr_t base, size_t size, uintptr_t vtable_va,
                     std::vector<uintptr_t> &out, size_t cap)
{
    __try
    {
        uintptr_t end = base + size;
        for (uintptr_t p = base; p + 8 <= end && out.size() < cap; p += 8)
            if (*reinterpret_cast<const volatile uint64_t *>(p) == vtable_va)
                out.push_back(p);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void scan_all_cursors(uintptr_t vtable_va, std::vector<uintptr_t> &out)
{
    constexpr size_t CAP = 256;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0;
    while (out.size() < CAP && VirtualQuery(reinterpret_cast<void *>(addr), &mbi, sizeof(mbi)))
    {
        uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        size_t sz = mbi.RegionSize;
        // RW only: the cursor instances live on the heap (READWRITE). Scanning
        // READONLY regions just doubled the cost for nothing (cut → faster first find).
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            mbi.Protect == PAGE_READWRITE &&
            sz >= 0x1000 && sz < 0x10000000)
            scan_region_all(base, sz, vtable_va, out, CAP);
        uintptr_t next = base + sz;
        if (next <= addr) break;
        addr = next;
    }
}

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
    for (uintptr_t off = 0; off < MENU_WALK_WINDOW; off += 8)
    {
        uint64_t p = 0;
        if (!seh_read8(reinterpret_cast<void *>(mm + off), &p) || !p || !plausible_ptr(p))
            continue;
        uint64_t vt = 0;
        if (seh_read8(reinterpret_cast<void *>(p + CURSOR_OFF_IN_MENU), &vt) && vt == vtable_va)
        {
            static uintptr_t logged_off = ~uintptr_t(0);
            if (logged_off != off)
            {
                logged_off = off;
                g_log->info("[MENU] cursor via CSMenuMan+{:#x} → dialog {:#x} → cursor {:#x} "
                            "(hardcode this offset for O(1))", off, p, p + CURSOR_OFF_IN_MENU);
            }
            return p + CURSOR_OFF_IN_MENU;
        }
    }
    return 0;
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
        }
        else
        {
            // Walk found nothing (map closed, OR the dialog offset is wrong). Throttled
            // whole-RAM scan (1/s, gate-INDEPENDENT — the 0xCD gate flakes 3/7/0) so the
            // overlay keeps working while we fix the walk.
            if (now - last_scan > std::chrono::milliseconds(1000))
            {
                candidates.clear();
                scan_all_cursors(vtable_va, candidates);
                last_scan = now;
                if (!candidates.empty())
                {
                    g_log->info("resolve: {} cursor (scan fallback — menu walk missed)", candidates.size());
                    // DIAG (one-shot): the scan found the cursor; locate dialog =
                    // cursor−0x2DB0 inside CSMenuMan so we can fix the walk's offset/chain.
                    static bool diag2 = false;
                    if (!diag2)
                    {
                        diag2 = true;
                        uint64_t mm = 0;
                        seh_read8(reinterpret_cast<void *>(base + CSMENUMAN_SLOT_RVA), &mm);
                        uintptr_t dialog = candidates[0] - CURSOR_OFF_IN_MENU;
                        g_log->info("[MENU-DIAG] scan cursor={:#x} dialog={:#x} mm={:#x}",
                                    candidates[0], dialog, mm);
                        bool found = false;
                        if (mm)
                            for (uintptr_t off = 0; off < 0x40000; off += 8)
                            {
                                uint64_t p = 0;
                                if (seh_read8(reinterpret_cast<void *>(mm + off), &p) && p == dialog)
                                {
                                    g_log->info("[MENU-DIAG] dialog ptr at CSMenuMan+{:#x} "
                                                "(hardcode/widen the walk)", off);
                                    found = true;
                                    break;
                                }
                            }
                        if (!found)
                            g_log->info("[MENU-DIAG] dialog NOT a flat field in CSMenuMan[0..0x40000] "
                                        "— needs a deref chain (mm→X→dialog)");
                    }
                }
                else if (++empty_logs % 6 == 0)
                    g_log->info("no cursor (menu walk + scan both empty — offset/RVA shift?)");
            }
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
    // diag window cursor+0xFC..+0x118 (find which offset drives the vertical axis)
    for (int i = 0; i < 8; ++i)
        if (!seh_read4(reinterpret_cast<void *>(a + 0xFC + i * 4), &out.raw[i]))
            out.raw[i] = 0.f;
    // WorldMapArea+0x6e (i32) = areaNo of the open page (doc §3) → page filter.
    out.viewArea = -1;
    seh_read_i32(reinterpret_cast<void *>(view + 0x6e), &out.viewArea);
    // Sublayer flag DAT_143d6cfc3 (eldenring.exe+0x3D6CFC3): 0 = overworld, !=0 =
    // underground. Cheap 1-byte read → lets the overlay switch overworld/underground
    // projection params + draw only the matching page's graces.
    out.underground = 0;
    int ug = 0;
    if (seh_read_i32(reinterpret_cast<void *>(base + 0x3D6CFC3), &ug))
        out.underground = ug & 0xFF; // the flag is a byte
    return true;
}

void initialize(const std::filesystem::path &log_path)
{
    try
    {
        g_log = spdlog::basic_logger_mt("mfg-wmprobe", log_path.string(), false);
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

} // namespace goblin::worldmap_probe
