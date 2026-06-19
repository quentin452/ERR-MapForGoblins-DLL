#include "goblin_worldmap_probe.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <array>
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

// CS::WorldMapCursorControl vtable RVA (doc imagebase 0x140000000). The cursor
// object's first qword IS this vtable ptr, so scanning memory for this value
// finds cursor instances directly. Resolve-by-RVA: if a game patch shifted it,
// the scan finds nothing → re-derive from the cursor ctor AOB (doc Goal 2).
constexpr uintptr_t CURSOR_VTABLE_RVA = 0x2b29a90;
constexpr ptrdiff_t OFF_X = 0xFC, OFF_Z = 0x104, OFF_Y = 0x10C;
constexpr uintptr_t CURSOR_OFF_IN_MENU = 0x2DB0;

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

// SEH-guarded 8-byte read (POD body — no C++ unwinding inside __try).
bool seh_read8(const void *src, uint64_t *out)
{
    __try { *out = *reinterpret_cast<const volatile uint64_t *>(src); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool seh_read4(const void *src, float *out)
{
    __try { *out = *reinterpret_cast<const volatile float *>(src); return true; }
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
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_READONLY) &&
            sz >= 0x1000 && sz < 0x10000000)
            scan_region_all(base, sz, vtable_va, out, CAP);
        uintptr_t next = base + sz;
        if (next <= addr) break;
        addr = next;
    }
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
    std::unordered_map<uintptr_t, char> bounds_dumped; // one-shot bounds dump per active cursor
    clock::time_point last_scan{};
    int empty_logs = 0;

    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        auto now = clock::now();

        // (Re)scan the full address space periodically (and when empty) — the menu
        // is re-created on each open, so new instances appear.
        if (candidates.empty() || now - last_scan > std::chrono::seconds(5))
        {
            candidates.clear();
            scan_all_cursors(vtable_va, candidates);
            last_scan = now;
            if (!candidates.empty())
                g_log->info("scan: {} cursor-vtable instance(s)", candidates.size());
            else if (++empty_logs % 6 == 0)
                g_log->info("no cursor-vtable instances (exe version shifted the RVA?)");
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
            if (x != l[0] || z != l[1] || y != l[2]) // moved → this is (likely) the active cursor
            {
                g_log->info("cursor @{:#x} (menu @{:#x}): +0xFC={:.2f}  +0x104={:.2f}  +0x10C={:.2f}",
                            a, a - CURSOR_OFF_IN_MENU, x, z, y);
                l = {x, z, y};

                // PROJECTION: follow cursor+0xF0 → WorldMapArea and log the LIVE
                // viewport (pan +0x378, zoom +0x380) + the static full-map rect every
                // time the cursor moves. PAN the map → pan sweeps; ZOOM → zoom changes;
                // the +0x350 rect should stay [0,0,10496,10496]. (doc §6)
                uint64_t view = 0;
                if (seh_read8(reinterpret_cast<void *>(a + OFF_VIEW_PTR), &view) && view)
                {
                    float panx, panz, zoom, r0, r1, r2, r3;
                    if (seh_read4(reinterpret_cast<void *>(view + VIEW_PAN_X), &panx) &&
                        seh_read4(reinterpret_cast<void *>(view + VIEW_PAN_Z), &panz) &&
                        seh_read4(reinterpret_cast<void *>(view + VIEW_ZOOM), &zoom) &&
                        seh_read4(reinterpret_cast<void *>(view + 0x350), &r0) &&
                        seh_read4(reinterpret_cast<void *>(view + 0x354), &r1) &&
                        seh_read4(reinterpret_cast<void *>(view + 0x358), &r2) &&
                        seh_read4(reinterpret_cast<void *>(view + 0x35c), &r3))
                        g_log->info("  VIEW @{:#x}: pan(+0x378)=({:.2f},{:.2f})  zoom(+0x380)={:.4f}"
                                    "  fullRect(+0x350)=[{:.1f},{:.1f},{:.1f},{:.1f}]",
                                    view, panx, panz, zoom, r0, r1, r2, r3);
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
