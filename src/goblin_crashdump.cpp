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

    // SetUnhandledExceptionFilter returns the prior filter; keep it to chain.
    g_prev_filter = SetUnhandledExceptionFilter(goblin_crash_filter);
}
} // namespace goblin
