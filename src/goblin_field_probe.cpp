#include "goblin_field_probe.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <spdlog/spdlog.h>

#include "from/params.hpp"

namespace
{
// eldenring.exe image bounds (set at arm time) — the RIP filter that auto-skips every mod read.
uintptr_t g_exe_base = 0, g_exe_end = 0;
// The watched address + a one-shot guard so we log a single clean hit then disarm.
uintptr_t g_watch_addr = 0;
std::atomic<bool> g_done{false};
PVOID g_veh = nullptr;
std::vector<DWORD> g_armed_tids;   // threads we set DR0 on (to disarm)
uint64_t g_dr7 = 0;

void compute_exe_bounds()
{
    HMODULE h = GetModuleHandleA("eldenring.exe");
    if (!h) return;
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(h);
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(reinterpret_cast<uint8_t *>(h) + dos->e_lfanew);
    g_exe_base = reinterpret_cast<uintptr_t>(h);
    g_exe_end = g_exe_base + nt->OptionalHeader.SizeOfImage;
}

// DR7: enable L0 + RW0 + LEN0 for DR0. rw: 0b11 = read|write (no read-only on x86), 0b01 = write.
// len: 1->00, 2->01, 8->10, 4->11.  (LE/GE legacy bits set for good measure.)
uint64_t make_dr7(int len, bool write_only)
{
    uint64_t rw = write_only ? 0b01 : 0b11;
    uint64_t lb = (len == 1) ? 0b00 : (len == 2) ? 0b01 : (len == 8) ? 0b10 : 0b11;
    return 0x100 /*LE*/ | 0x1 /*L0*/ | (rw << 16) | (lb << 18);
}

void set_dr_on_thread(DWORD tid, uintptr_t addr, uint64_t dr7)
{
    HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
    if (!t) return;
    bool self = (tid == GetCurrentThreadId());
    if (!self) SuspendThread(t);
    CONTEXT ctx;
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(t, &ctx))
    {
        ctx.Dr0 = addr;
        ctx.Dr7 = dr7;
        ctx.Dr6 = 0;
        SetThreadContext(t, &ctx);
    }
    if (!self) ResumeThread(t);
    CloseHandle(t);
}

void for_each_thread(uintptr_t addr, uint64_t dr7, bool record)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    DWORD pid = GetCurrentProcessId();
    if (Thread32First(snap, &te))
    {
        do
        {
            if (te.th32OwnerProcessID != pid) continue;
            set_dr_on_thread(te.th32ThreadID, addr, dr7);
            if (record) g_armed_tids.push_back(te.th32ThreadID);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

void disarm()
{
    for (DWORD tid : g_armed_tids)
        set_dr_on_thread(tid, 0, 0);   // Dr0=0, Dr7=0 → disabled
    g_armed_tids.clear();
}

LONG CALLBACK veh(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = ep->ContextRecord;
    if (!(c->Dr6 & 0x1))                       // not our DR0 breakpoint
        return EXCEPTION_CONTINUE_SEARCH;
    c->Dr6 = 0;                                // ack the debug status

    uintptr_t rip = static_cast<uintptr_t>(c->Rip);
    // Filter: only the GAME's own read site is the source of truth. Every mod (MapForGoblins,
    // reforged, exposers, …) reads the same field — skip those and keep watching.
    if (rip < g_exe_base || rip >= g_exe_end)
        return EXCEPTION_CONTINUE_EXECUTION;
    if (g_done.exchange(true))                 // already logged one clean hit
        return EXCEPTION_CONTINUE_EXECUTION;

    // For DATA breakpoints the saved RIP is the NEXT instruction; the accessing instruction ENDS
    // here. Dump a window before+after so offset_resolver.py can back-decode + author the AOB.
    uint8_t buf[40] = {0};
    uintptr_t start = rip - 24;
    std::memcpy(buf, reinterpret_cast<void *>(start), sizeof(buf));
    char hex[40 * 3 + 1];
    int n = 0;
    for (int i = 0; i < (int)sizeof(buf); ++i) n += std::snprintf(hex + n, sizeof(hex) - n, "%02X ", buf[i]);

    spdlog::warn("[FWA] eldenring.exe READ of {:#x} by rip=er+0x{:x} (RVA; access is the instr ENDING "
                 "at this rip)", g_watch_addr, rip - g_exe_base);
    spdlog::warn("[FWA]   bytes[rip-24 .. rip+16) = {}", hex);
    spdlog::warn("[FWA]   -> author: py D:\\ghidra_scripts\\offset_resolver.py author 0x<accessing-rip>"
                 "  (the instr just before er+0x{:x})", rip - g_exe_base);

    disarm();                                  // one-shot
    return EXCEPTION_CONTINUE_EXECUTION;
}

bool parse_spec(const std::string &spec, std::wstring &param, uint64_t &row, ptrdiff_t &off,
                int &len, bool &write_only)
{
    // ParamName:rowId:offset[:len[:rw]]
    std::vector<std::string> parts;
    size_t p = 0;
    while (p <= spec.size())
    {
        size_t c = spec.find(':', p);
        if (c == std::string::npos) c = spec.size();
        parts.push_back(spec.substr(p, c - p));
        p = c + 1;
        if (c == spec.size()) break;
    }
    if (parts.size() < 3 || parts[0].empty()) return false;
    param.assign(parts[0].begin(), parts[0].end());
    row = std::strtoull(parts[1].c_str(), nullptr, 0);
    off = static_cast<ptrdiff_t>(std::strtoll(parts[2].c_str(), nullptr, 0));
    len = (parts.size() > 3 && !parts[3].empty()) ? std::atoi(parts[3].c_str()) : 4;
    if (len != 1 && len != 2 && len != 4 && len != 8) len = 4;
    write_only = (parts.size() > 4 && (parts[4] == "w" || parts[4] == "write"));
    return true;
}
} // namespace

void goblin::field_probe::initialize(const std::string &spec)
{
    std::wstring param;
    uint64_t row = 0;
    ptrdiff_t off = 0;
    int len = 4;
    bool write_only = false;
    if (!parse_spec(spec, param, row, off, len, write_only))
    {
        spdlog::error("[FWA] bad probe_field_spec '{}' — expected ParamName:rowId:offset[:len[:rw]]", spec);
        return;
    }

    // Resolve the LIVE row address via the same param chain the loot path uses.
    uintptr_t addr = 0;
    try
    {
        auto seq = from::params::get_param<uint8_t>(param);
        uint8_t *r = seq.try_get(row);
        if (!r)
        {
            spdlog::error("[FWA] row {} not found in {} — pick a row id present in the regulation",
                          row, std::string(param.begin(), param.end()));
            return;
        }
        addr = reinterpret_cast<uintptr_t>(r) + off;
    }
    catch (...)
    {
        spdlog::error("[FWA] param '{}' not found", std::string(param.begin(), param.end()));
        return;
    }

    if (len != 1 && (addr % len) != 0)
    {
        spdlog::warn("[FWA] addr {:#x} not {}-aligned — HW bp needs alignment; falling back to len=1", addr, len);
        len = 1;
    }

    compute_exe_bounds();
    if (!g_exe_base)
    {
        spdlog::error("[FWA] could not resolve eldenring.exe bounds");
        return;
    }

    g_watch_addr = addr;
    g_dr7 = make_dr7(len, write_only);
    g_veh = AddVectoredExceptionHandler(1, veh);
    for_each_thread(addr, g_dr7, /*record=*/true);

    spdlog::warn("[FWA] ARMED hw-bp on {}[{}]+{:#x} = {:#x} (len={} {}) — {} threads. Trigger the game's "
                 "read in-game; the first eldenring.exe access logs [FWA] (mod reads are filtered).",
                 std::string(param.begin(), param.end()), row, (uint64_t)off, addr, len,
                 write_only ? "write" : "read|write", (int)g_armed_tids.size());
}
