#include "goblin_crashdump.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <utility>
#include <vector>

// dbghelp provides MiniDumpWriteDump. clang-cl honours #pragma comment(lib),
// so no CMake change is strictly required, but the lib is also listed in
// target_link_libraries for clarity.
#pragma comment(lib, "dbghelp.lib")

namespace
{
// Directory the dump is written to (resolved once at install time).
wchar_t g_dump_dir[MAX_PATH] = {0};

// The filter that was registered before ours — we chain to it so the game's
// / Seamless Co-op's own handlers still run.
LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter = nullptr;

// Re-entrancy / double-fire guard: a fault inside the dump writer (or a second
// thread faulting) must not recurse.
volatile LONG g_in_handler = 0;

// Module bases captured at install, so the offline reader can tell whether the
// fault is in the game or in us even when the full minidump fails to write.
uintptr_t g_self_base = 0; // MapForGoblins.dll
uintptr_t g_self_end = 0;
uintptr_t g_er_base = 0;   // eldenring.exe
uintptr_t g_er_end = 0;

// dbghelp symbol engine, initialised at INSTALL time (allocating mid-crash is
// asking for a double fault; SymFromAddr in the handler then only does the
// deferred module load + lookup). Needs MapForGoblins.pdb next to the DLL
// (/Z7 + lld-link /debug — see clang-cl-xwin.cmake); without it every lookup
// just fails and the triage falls back to raw +0xRVA lines as before.
bool g_sym_ready = false;

// Resolve `addr` to "name+0xdisp" via dbghelp. Returns false (out untouched)
// when symbols are unavailable — e.g. eldenring.exe (no PDB), or wine's dbghelp
// failing to parse ours. POD-only, fixed buffers: this runs mid-crash.
static bool symbolize_addr(uintptr_t addr, char *out, size_t outsz)
{
    if (!g_sym_ready) return false;
    char buf[sizeof(SYMBOL_INFO) + 256] = {};
    auto *si = reinterpret_cast<SYMBOL_INFO *>(buf);
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (!SymFromAddr(GetCurrentProcess(), static_cast<DWORD64>(addr), &disp, si))
        return false;
    _snprintf(out, outsz, "%s+0x%llX", si->Name,
              static_cast<unsigned long long>(disp));
    out[outsz - 1] = '\0';
    return true;
}

// SizeOfImage from the PE header at `base` (PE64: e_lfanew@+0x3C, SizeOfImage@
// +0x50 of the optional header). 0 on failure. POD-only.
static uintptr_t image_end(uintptr_t base)
{
    if (!base) return 0;
    __try
    {
        uint32_t e_lfanew = *reinterpret_cast<uint32_t *>(base + 0x3C);
        uint32_t size = *reinterpret_cast<uint32_t *>(base + e_lfanew + 0x50);
        return base + size;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

// Write a tiny TEXT triage next to the dump: exception code + faulting address +
// the module it lands in. Independent of MiniDumpWriteDump (which can produce a
// 0-byte file on a hard crash), so the one fact that matters always survives.
// POD-only (no C++ unwinding) — we're mid-crash.
static void write_crash_triage(EXCEPTION_POINTERS *ep)
{
    wchar_t txt[MAX_PATH] = {0};
    _snwprintf(txt, MAX_PATH, L"%ls\\MapForGoblins_crash_%lu.txt", g_dump_dir,
               GetCurrentProcessId());
    HANDLE f = CreateFileW(txt, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;

    DWORD code = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode : 0;
    void *addr = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionAddress
                                             : nullptr;
    char mod[MAX_PATH] = "?";
    uintptr_t modbase = 0;
    if (addr)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(addr, &mbi, sizeof(mbi)))
        {
            modbase = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
            GetModuleFileNameA(reinterpret_cast<HMODULE>(mbi.AllocationBase), mod,
                               MAX_PATH);
        }
    }
    char fault_sym[320] = "";
    if (addr)
        symbolize_addr(reinterpret_cast<uintptr_t>(addr), fault_sym, sizeof(fault_sym));
    char buf[1400];
    int n = _snprintf(
        buf, sizeof(buf),
        "MapForGoblins crash triage\r\n"
        "exception_code = 0x%08lX\r\n"
        "fault_address  = 0x%p\r\n"
        "fault_module   = %s\r\n"
        "fault_base     = 0x%p  (+0x%llX)\r\n"
        "fault_symbol   = %s\r\n"
        "MapForGoblins.dll base = 0x%p\r\n"
        "eldenring.exe    base  = 0x%p\r\n",
        code, addr, mod, reinterpret_cast<void *>(modbase),
        static_cast<unsigned long long>(addr ? reinterpret_cast<uintptr_t>(addr) - modbase
                                             : 0),
        fault_sym[0] ? fault_sym : "? (no pdb / not our module)",
        reinterpret_cast<void *>(g_self_base), reinterpret_cast<void *>(g_er_base));
    if (n > 0)
    {
        DWORD w = 0;
        WriteFile(f, buf, static_cast<DWORD>(n), &w, nullptr);
    }

    // Poor-man's stack trace: scan the crashing thread's stack for qwords that fall
    // inside MapForGoblins.dll or eldenring.exe — probable return addresses. Reveals
    // whether the MOD is in the call chain (mod-triggered fault / DL_PANIC) and gives
    // the eldenring.exe offsets to symbolise in Ghidra. Naive (no unwind info) but
    // robust mid-crash: just reads stack memory. POD-only.
    if (ep && ep->ContextRecord)
    {
        uintptr_t rsp = static_cast<uintptr_t>(ep->ContextRecord->Rsp);
        const char hdr[] = "\r\nstack (probable return addrs, top-down):\r\n";
        DWORD w = 0;
        WriteFile(f, hdr, sizeof(hdr) - 1, &w, nullptr);
        int printed = 0;
        for (int i = 0; i < 4096 && printed < 48; i++)
        {
            uintptr_t slot = rsp + static_cast<uintptr_t>(i) * 8;
            uintptr_t val = 0;
            MEMORY_BASIC_INFORMATION mq;
            if (!VirtualQuery(reinterpret_cast<void *>(slot), &mq, sizeof(mq)) ||
                mq.State != MEM_COMMIT)
                break; // ran off the stack
            __try { val = *reinterpret_cast<uintptr_t *>(slot); }
            __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
            const char *m = nullptr;
            uintptr_t b = 0;
            if (g_self_base && val >= g_self_base && val < g_self_end) { m = "MapForGoblins.dll"; b = g_self_base; }
            else if (g_er_base && val >= g_er_base && val < g_er_end) { m = "eldenring.exe"; b = g_er_base; }
            if (!m) continue;
            // Symbolize our own frames (PDB next to the DLL); eldenring.exe has
            // no PDB so its lines stay raw +0xRVA for Ghidra as before.
            char sym[320] = "";
            if (b == g_self_base)
                symbolize_addr(val, sym, sizeof(sym));
            char line[480];
            int ln = _snprintf(line, sizeof(line), "  [+0x%05X] %s +0x%llX%s%s\r\n",
                               (unsigned)(i * 8), m,
                               static_cast<unsigned long long>(val - b),
                               sym[0] ? "  " : "", sym);
            if (ln > 0) WriteFile(f, line, static_cast<DWORD>(ln), &w, nullptr);
            printed++;
        }
    }
    CloseHandle(f);
}

LONG WINAPI goblin_crash_filter(EXCEPTION_POINTERS *ep)
{
    if (InterlockedCompareExchange(&g_in_handler, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    // Text triage first — survives even if the minidump below writes nothing.
    write_crash_triage(ep);

    // Only write the heavy (~13 MB) minidump when the fault is in OUR DLL — a bug
    // we can act on. Game/ntdll faults (incl. the eldenring.exe crash-on-exit while
    // the game tears down) get the lightweight .txt triage only, saving the dump's
    // disk per non-actionable crash. The minidump under this Proton/Wine has been
    // unreliable for non-our-code faults anyway; the .txt has code+addr+module.
    bool ours = false;
    if (ep && ep->ExceptionRecord && g_self_base)
    {
        MEMORY_BASIC_INFORMATION m;
        if (VirtualQuery(ep->ExceptionRecord->ExceptionAddress, &m, sizeof(m)))
            ours = reinterpret_cast<uintptr_t>(m.AllocationBase) == g_self_base;
    }

    // Build "<dir>/MapForGoblins_crash_<pid>.dmp". No std::string / spdlog here
    // — the process is already unwinding a crash; keep it to raw Win32 + swprintf.
    wchar_t path[MAX_PATH] = {0};
    _snwprintf(path, MAX_PATH, L"%ls\\MapForGoblins_crash_%lu.dmp",
               g_dump_dir, GetCurrentProcessId());

    HANDLE file = ours ? CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr)
                       : INVALID_HANDLE_VALUE;
    if (!ours)
        OutputDebugStringW(L"[MapForGoblins] fault outside our DLL — minidump skipped (see .txt)");
    if (file != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mei = {};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;

        // Triage dump: module list + every thread's stack & context, plus the
        // exception record/context — no heap. Matches the parser in
        // docs/crash_dump_diagnostics.md and stays ~tens of MB.
        const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);

        BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                    type, ep ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(file);

        if (ok)
        {
            OutputDebugStringW(L"[MapForGoblins] crash minidump written: ");
            OutputDebugStringW(path);
        }
        else
        {
            // A hard crash can leave a 0-byte dump — delete it so retention keeps
            // only real dumps; the .txt triage already has the key facts.
            DeleteFileW(path);
            OutputDebugStringW(L"[MapForGoblins] minidump failed; see .txt triage");
        }
    }

    // Let the previous handler (and the OS default) run as usual.
    if (g_prev_filter)
        return g_prev_filter(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace

// Keep only the `keep` most-recent MapForGoblins_crash_*.dmp files (each is
// tens of MB). Without this they accumulate forever — one per crash, never
// cleaned. Run at install time (before any dump this session), so pruning to
// `keep` leaves room for the current session's dump. Best-effort: all errors
// swallowed via error_code so this never disturbs init.
static void prune_old_dumps(const std::filesystem::path &dir, size_t keep)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<std::pair<fs::file_time_type, fs::path>> dumps;
    for (auto it = fs::directory_iterator(dir, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec))
    {
        const fs::path &p = it->path();
        const std::string name = p.filename().string();
        if (name.rfind("MapForGoblins_crash_", 0) == 0 && p.extension() == ".dmp")
        {
            auto t = fs::last_write_time(p, ec);
            if (!ec)
                dumps.emplace_back(t, p);
        }
    }
    if (dumps.size() <= keep)
        return;
    std::sort(dumps.begin(), dumps.end(),
              [](const auto &a, const auto &b) { return a.first > b.first; });
    for (size_t i = keep; i < dumps.size(); i++)
        fs::remove(dumps[i].second, ec);
}

namespace goblin
{
void install_crash_handler(const std::filesystem::path &dump_dir)
{
    std::error_code ec;
    std::filesystem::create_directories(dump_dir, ec);
    prune_old_dumps(dump_dir, 4); // keep newest 4 + this session's = max 5

    const std::wstring w = dump_dir.wstring();
    wcsncpy(g_dump_dir, w.c_str(), MAX_PATH - 1);
    g_dump_dir[MAX_PATH - 1] = L'\0';

    // Capture module bases for the triage file (so a fault address can be mapped
    // to game-vs-mod offline even if the full minidump fails).
    g_er_base = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    g_er_end = image_end(g_er_base);
    HMODULE self = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&goblin_crash_filter), &self))
    {
        g_self_base = reinterpret_cast<uintptr_t>(self);
        g_self_end = image_end(g_self_base);
    }

    // Symbol engine for crash-time name resolution. Initialised HERE (not in the
    // handler — dbghelp init allocates). Deferred loads: the PDB is only parsed
    // on the first SymFromAddr, i.e. mid-crash, best-effort. Search path = the
    // DLL's own directory, where the deploy step puts MapForGoblins.pdb.
    if (g_self_base)
    {
        char dll_path[MAX_PATH] = {0};
        GetModuleFileNameA(reinterpret_cast<HMODULE>(g_self_base), dll_path, MAX_PATH);
        if (char *slash = strrchr(dll_path, '\\')) *slash = '\0';
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME |
                      SYMOPT_FAIL_CRITICAL_ERRORS);
        g_sym_ready = SymInitialize(GetCurrentProcess(),
                                    dll_path[0] ? dll_path : nullptr, TRUE) != FALSE;
    }

    // SetUnhandledExceptionFilter returns the prior filter; keep it to chain.
    g_prev_filter = SetUnhandledExceptionFilter(goblin_crash_filter);
}

std::pair<uintptr_t, uintptr_t> self_module_range()
{
    return {g_self_base, g_self_end};
}
} // namespace goblin
