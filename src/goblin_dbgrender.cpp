#include "goblin_dbgrender.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

#include <spdlog/spdlog.h>

namespace
{
    constexpr uintptr_t DBG_DISP_GATE_RVA = 0x3d85b18;  // DAT_143d85b18 — CSDbgDispStep run gate (findings §2)
    uint64_t g_last = 0;

    bool rpm(uintptr_t a, void *out, size_t n)
    {
        SIZE_T got = 0;
        return ReadProcessMemory(GetCurrentProcess(), (void *)a, out, n, &got) && got == n;
    }
    bool wpm(uintptr_t a, const void *in, size_t n)
    {
        SIZE_T put = 0;
        return WriteProcessMemory(GetCurrentProcess(), (void *)a, in, n, &put) && put == n;
    }
}

namespace goblin::dbgrender
{
    uint64_t last_read() { return g_last; }

    std::string probe_read()
    {
        uintptr_t base = (uintptr_t)GetModuleHandleA("eldenring.exe");
        if (!base) return "err: eldenring.exe base not found";
        uintptr_t addr = base + DBG_DISP_GATE_RVA;
        uint64_t v = 0;
        if (!rpm(addr, &v, sizeof(v))) return "err: read DAT_143d85b18 failed";
        g_last = v;
        // Classify: 0 = disabled; a small int (< 0x10000) = likely a bool/flag (safe to write 1); a large
        // module-ish value (>= 0x10000, looks like a pointer) = writing 1 would crash on deref -> DO NOT.
        const char *cls = (v == 0) ? "ZERO (disabled)"
                          : (v < 0x10000) ? "small int -> likely a FLAG (safe to set 1)"
                                          : "large -> looks like a POINTER (do NOT write 1)";
        char b[192];
        std::snprintf(b, sizeof(b), "ok dbgrender DAT_143d85b18 @%#llx = %#llx  [%s]",
                      (unsigned long long)addr, (unsigned long long)v, cls);
        return std::string(b);
    }

    bool probe_write(uint64_t value)
    {
        uintptr_t base = (uintptr_t)GetModuleHandleA("eldenring.exe");
        if (!base) return false;
        uintptr_t addr = base + DBG_DISP_GATE_RVA;
        bool ok = wpm(addr, &value, sizeof(value));
        spdlog::info("[DBGRENDER] write DAT_143d85b18 = {:#x} -> {}", value, ok ? "ok" : "FAIL");
        return ok;
    }
}
