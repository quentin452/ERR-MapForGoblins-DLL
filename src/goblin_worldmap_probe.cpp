#include "goblin_worldmap_probe.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>

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

// Walk committed private RW memory for the cursor vtable → cursor obj address.
uintptr_t scan_for_cursor(uintptr_t vtable_va)
{
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0;
    while (VirtualQuery(reinterpret_cast<void *>(addr), &mbi, sizeof(mbi)))
    {
        uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        size_t sz = mbi.RegionSize;
        bool scanny = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
                      (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_READONLY) &&
                      sz >= 0x1000 && sz < 0x10000000;
        if (scanny)
        {
            uintptr_t hit = scan_region(base, sz, vtable_va);
            if (hit)
                return hit;
        }
        uintptr_t next = base + sz;
        if (next <= addr) break; // overflow / no progress
        addr = next;
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

    uintptr_t cur = 0;       // cached cursor-object address
    int miss = 0;
    float lx = 0, lz = 0, ly = 0;
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));

        // Re-validate cache (menu re-created each open → ptr moves/dies).
        if (cur)
        {
            uint64_t v;
            if (!seh_read8(reinterpret_cast<void *>(cur), &v) || v != vtable_va)
                cur = 0;
        }
        if (!cur)
        {
            cur = scan_for_cursor(vtable_va);
            if (cur)
                g_log->info("cursor obj @ {:#x} (world-map menu @ {:#x})", cur,
                            cur - CURSOR_OFF_IN_MENU);
            else
            {
                if (++miss % 25 == 0)
                    g_log->info("cursor not found (open the world map; or the exe "
                                "version != RE doc → vtable RVA shifted)");
                continue;
            }
        }

        float x, z, y;
        if (seh_read4(reinterpret_cast<void *>(cur + OFF_X), &x) &&
            seh_read4(reinterpret_cast<void *>(cur + OFF_Z), &z) &&
            seh_read4(reinterpret_cast<void *>(cur + OFF_Y), &y))
        {
            // Log only on change (cursor moved) to keep the log readable.
            if (x != lx || z != lz || y != ly)
            {
                g_log->info("cursor +0xFC={:.2f}  +0x104={:.2f}  +0x10C={:.2f}", x, z, y);
                lx = x; lz = z; ly = y;
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
