#include "goblin_freeze_watchdog.hpp"
#include "goblin_config.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdio>
#include <thread>

// dbghelp is already linked by goblin_crashdump.cpp's #pragma, but keep this TU
// self-contained in case that file is ever split out.
#pragma comment(lib, "dbghelp.lib")

namespace
{
std::atomic<uint64_t> g_present_beat{0};
wchar_t g_dir[MAX_PATH] = L"";

// The whole dump path uses raw Win32 only: the very thing we're reporting is a suspected
// deadlock, and spdlog (or anything that takes a lock) may be PART of it. A frozen present
// thread could be blocked inside the logger's mutex — calling the logger here would then
// freeze the watchdog too, silently.
void write_freeze_report(uint64_t beat, DWORD stalled_ms)
{
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%ls\\MapForGoblins_freeze_%lu.txt", g_dir,
               GetCurrentProcessId());
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char buf[512];
        int n = _snprintf(buf, sizeof(buf),
                          "MapForGoblins freeze watchdog\r\n"
                          "detected      = %04u-%02u-%02u %02u:%02u:%02u\r\n"
                          "present beat  = %llu (unchanged for ~%lu ms)\r\n"
                          "threshold     = %u s (ini [Debug] freeze_watchdog_secs)\r\n"
                          "NO exception fired - this is a stall/deadlock, not a crash.\r\n"
                          "All-thread stacks: MapForGoblins_freeze_<pid>.dmp next to this file\r\n"
                          "(symbolize the MapForGoblins.dll frames with the deployed .pdb).\r\n",
                          st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                          static_cast<unsigned long long>(beat),
                          static_cast<unsigned long>(stalled_ms),
                          static_cast<unsigned>(goblin::config::freezeWatchdogSecs));
        DWORD w = 0;
        if (n > 0) WriteFile(f, buf, static_cast<DWORD>(n), &w, nullptr);
        CloseHandle(f);
    }

    _snwprintf(path, MAX_PATH, L"%ls\\MapForGoblins_freeze_%lu.dmp", g_dir,
               GetCurrentProcessId());
    HANDLE df = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (df != INVALID_HANDLE_VALUE)
    {
        // Same triage-grade type as the crash dump: module list + every thread's stack &
        // context, no heap. Written from THIS healthy thread — the frozen threads' contexts
        // are exactly what we're after.
        const MINIDUMP_TYPE type =
            static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
        BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), df, type,
                                    nullptr, nullptr, nullptr);
        CloseHandle(df);
        if (!ok)
            DeleteFileW(path); // 0-byte husk on failure — the .txt still records the stall
    }
}

void watchdog_loop()
{
    const DWORD threshold_ms =
        static_cast<DWORD>(goblin::config::freezeWatchdogSecs) * 1000u;
    uint64_t last_beat = 0;
    ULONGLONG last_change = GetTickCount64();
    bool dumped = false;
    for (;;)
    {
        Sleep(1000);
        const uint64_t beat = g_present_beat.load(std::memory_order_relaxed);
        const ULONGLONG now = GetTickCount64();
        if (beat != last_beat)
        {
            last_beat = beat;
            last_change = now;
            continue;
        }
        // No frame rendered since last_change. Only meaningful once the overlay has rendered
        // at least once (beat > 0) — before that, "no beats" is just the boot sequence.
        if (beat == 0 || dumped)
            continue;
        const DWORD stalled = static_cast<DWORD>(now - last_change);
        if (stalled >= threshold_ms)
        {
            dumped = true; // one report per session — a real freeze doesn't recover anyway
            write_freeze_report(beat, stalled);
        }
    }
}
} // namespace

namespace goblin::freeze_watchdog
{
void beat_present() { g_present_beat.fetch_add(1, std::memory_order_relaxed); }

void install(const std::filesystem::path &log_dir)
{
    if (goblin::config::freezeWatchdogSecs == 0)
    {
        spdlog::info("[FREEZEWD] disabled (freeze_watchdog_secs = 0)");
        return;
    }
    const std::wstring w = log_dir.wstring();
    wcsncpy(g_dir, w.c_str(), MAX_PATH - 1);
    g_dir[MAX_PATH - 1] = L'\0';
    std::thread(watchdog_loop).detach();
    spdlog::info("[FREEZEWD] armed: present-thread stall > {}s → freeze triage + all-thread "
                 "minidump in logs/ (no exception needed)",
                 goblin::config::freezeWatchdogSecs);
}
} // namespace goblin::freeze_watchdog
