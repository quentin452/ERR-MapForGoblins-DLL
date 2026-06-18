#include "goblin_crashdump.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <cstdio>
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
uintptr_t g_er_base = 0;   // eldenring.exe

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
    char buf[1024];
    int n = _snprintf(
        buf, sizeof(buf),
        "MapForGoblins crash triage\r\n"
        "exception_code = 0x%08lX\r\n"
        "fault_address  = 0x%p\r\n"
        "fault_module   = %s\r\n"
        "fault_base     = 0x%p  (+0x%llX)\r\n"
        "MapForGoblins.dll base = 0x%p\r\n"
        "eldenring.exe    base  = 0x%p\r\n",
        code, addr, mod, reinterpret_cast<void *>(modbase),
        static_cast<unsigned long long>(addr ? reinterpret_cast<uintptr_t>(addr) - modbase
                                             : 0),
        reinterpret_cast<void *>(g_self_base), reinterpret_cast<void *>(g_er_base));
    if (n > 0)
    {
        DWORD w = 0;
        WriteFile(f, buf, static_cast<DWORD>(n), &w, nullptr);
    }
    CloseHandle(f);
}

LONG WINAPI goblin_crash_filter(EXCEPTION_POINTERS *ep)
{
    if (InterlockedCompareExchange(&g_in_handler, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    // Text triage first — survives even if the minidump below writes nothing.
    write_crash_triage(ep);

    // Build "<dir>/MapForGoblins_crash_<pid>.dmp". No std::string / spdlog here
    // — the process is already unwinding a crash; keep it to raw Win32 + swprintf.
    wchar_t path[MAX_PATH] = {0};
    _snwprintf(path, MAX_PATH, L"%ls\\MapForGoblins_crash_%lu.dmp",
               g_dump_dir, GetCurrentProcessId());

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
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
    HMODULE self = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&goblin_crash_filter), &self))
        g_self_base = reinterpret_cast<uintptr_t>(self);

    // SetUnhandledExceptionFilter returns the prior filter; keep it to chain.
    g_prev_filter = SetUnhandledExceptionFilter(goblin_crash_filter);
}
} // namespace goblin
