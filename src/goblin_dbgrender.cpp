#include "goblin_dbgrender.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <vector>

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

    std::string dump(uintptr_t rva, size_t len)
    {
        uintptr_t base = (uintptr_t)GetModuleHandleA("eldenring.exe");
        if (!base) return "err: no base";
        if (len > 0x400) len = 0x400;
        std::vector<uint8_t> buf(len);
        if (!rpm(base + rva, buf.data(), len)) return "err: read failed";
        std::string out;
        char b[96];
        for (size_t i = 0; i < len; i += 16)
        {
            std::snprintf(b, sizeof(b), "%#08llx: ", (unsigned long long)(rva + i));
            out += b;
            for (size_t j = 0; j < 16 && i + j < len; ++j)
            {
                std::snprintf(b, sizeof(b), "%02x ", buf[i + j]);
                out += b;
            }
            out += "\n";
        }
        return out;
    }

    // Crash-safe windowed scan for a vtable qword (base+vt_rva) -> the first live instance.
    static uintptr_t scan_vtable(uintptr_t vt_rva)
    {
        uintptr_t base = (uintptr_t)GetModuleHandleA("eldenring.exe");
        if (!base) return 0;
        uintptr_t want = base + vt_rva;
        SYSTEM_INFO si; GetSystemInfo(&si);
        uintptr_t addr = (uintptr_t)si.lpMinimumApplicationAddress, aend = (uintptr_t)si.lpMaximumApplicationAddress;
        const size_t WIN = 8 * 1024 * 1024;
        std::vector<uint8_t> buf(WIN + 16);
        MEMORY_BASIC_INFORMATION mbi;
        while (addr < aend && VirtualQuery((void *)addr, &mbi, sizeof(mbi)) == sizeof(mbi))
        {
            uintptr_t rb = (uintptr_t)mbi.BaseAddress; size_t rs = mbi.RegionSize; DWORD pr = mbi.Protect;
            bool ok = mbi.State == MEM_COMMIT && rs >= 16 &&
                      (pr & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY)) && !(pr & (PAGE_GUARD | PAGE_NOACCESS));
            if (ok)
                for (size_t off = 0; off < rs; off += WIN)
                {
                    size_t want_n = rs - off; if (want_n > WIN + 16) want_n = WIN + 16;
                    if (!rpm(rb + off, buf.data(), want_n)) continue;
                    size_t scan_to = want_n >= 8 ? want_n - 8 : 0;
                    for (size_t i = 0; i < scan_to; i += 8)
                        if (*(const uint64_t *)(buf.data() + i) == want) return rb + off + i;
                }
            uintptr_t nxt = rb + rs; if (nxt <= addr) break; addr = nxt;
        }
        return 0;
    }

    std::string find_hkdbg()
    {
        constexpr uintptr_t CSHKDBG_VT_RVA = 0x2b92230;   // CSHkDebugDisp / hkDebugDisplayHandler (findings §1)
        uintptr_t inst = scan_vtable(CSHKDBG_VT_RVA);
        if (!inst) return "err: no CSHkDebugDisp instance found (vtable er+0x2b92230)";
        uint8_t head[0x80];
        std::string out;
        char b[96];
        std::snprintf(b, sizeof(b), "ok CSHkDebugDisp inst=%#llx  head:\n", (unsigned long long)inst);
        out += b;
        if (rpm(inst, head, sizeof(head)))
            for (size_t i = 0; i < sizeof(head); i += 16)
            {
                std::snprintf(b, sizeof(b), "+%03zx: ", i); out += b;
                for (size_t j = 0; j < 16; ++j) { std::snprintf(b, sizeof(b), "%02x ", head[i + j]); out += b; }
                out += "\n";
            }
        return out;
    }

    uint8_t peek8(uintptr_t rva)
    {
        uintptr_t base = (uintptr_t)GetModuleHandleA("eldenring.exe");
        uint8_t v = 0xFF;
        if (base) rpm(base + rva, &v, 1);
        return v;
    }
    bool poke8(uintptr_t rva, uint8_t val)
    {
        uintptr_t base = (uintptr_t)GetModuleHandleA("eldenring.exe");
        if (!base) return false;
        bool ok = wpm(base + rva, &val, 1);
        spdlog::info("[DBGRENDER] poke8 er+{:#x} = {} -> {}", rva, val, ok ? "ok" : "FAIL");
        return ok;
    }
}
