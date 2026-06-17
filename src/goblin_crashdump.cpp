#include "goblin_crashdump.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cwchar>

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

LONG WINAPI goblin_crash_filter(EXCEPTION_POINTERS *ep)
{
    if (InterlockedCompareExchange(&g_in_handler, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

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

        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                          type, ep ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(file);

        OutputDebugStringW(L"[MapForGoblins] crash minidump written: ");
        OutputDebugStringW(path);
    }

    // Let the previous handler (and the OS default) run as usual.
    if (g_prev_filter)
        return g_prev_filter(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace

namespace goblin
{
void install_crash_handler(const std::filesystem::path &dump_dir)
{
    std::error_code ec;
    std::filesystem::create_directories(dump_dir, ec);

    const std::wstring w = dump_dir.wstring();
    wcsncpy(g_dump_dir, w.c_str(), MAX_PATH - 1);
    g_dump_dir[MAX_PATH - 1] = L'\0';

    // SetUnhandledExceptionFilter returns the prior filter; keep it to chain.
    g_prev_filter = SetUnhandledExceptionFilter(goblin_crash_filter);
}
} // namespace goblin
