#include "goblin_load_watchdog.hpp"
#include "goblin_config.hpp"
#include "modutils.hpp"
#include "re_signatures.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdio>
#include <thread>

// Same as the freeze watchdog: dbghelp is linked by goblin_crashdump.cpp's #pragma; keep this
// TU self-contained regardless.
#pragma comment(lib, "dbghelp.lib")

namespace
{
wchar_t g_dir[MAX_PATH] = L"";

// ── Armed-warp state (written by arm_warp on the present thread, read by the poll thread) ──
std::atomic<bool> g_armed{false};
std::atomic<int32_t> g_target_grace{0};
std::atomic<uint64_t> g_arm_tick{0};

// ── WorldChrMan static slot (self-contained resolve, mirrors goblin_world_position) ────────
// LocalPlayer = [WorldChrMan + 0x1E508]. WCM_FINDER: trailing `mov rax,[rip+disp32]` — disp @
// finder+0xA, instruction ends finder+0xE → slot = finder+0xE+disp.
void **g_wcm_slot = nullptr;
uintptr_t g_mapid_slot = 0;   // &(player-MapId singleton); MapId = *(singleton + 0x2c)

void resolve_statics()
{
    if (auto *finder = reinterpret_cast<uint8_t *>(
            modutils::scan<void>({.aob = goblin::sig::WCM_FINDER})))
    {
        int32_t disp = *reinterpret_cast<int32_t *>(finder + 0xA);
        g_wcm_slot = reinterpret_cast<void **>(finder + 0xE + disp);
    }
    g_mapid_slot = reinterpret_cast<uintptr_t>(modutils::scan<void>(
        {.aob = goblin::sig::PLAYER_MAPID_SLOT, .relative_offsets = {{3, 7}}}));
    spdlog::info("[LOADWD] statics: WorldChrMan-slot {:p}, mapId-slot {:p}", (void *)g_wcm_slot,
                 (void *)g_mapid_slot);
}

// SEH-guarded reads (noinline body so clang-cl keeps the __try around an opaque CALL —
// docs/memory/tooling/clang-cl-seh-noinline.md). The singletons may be mid-teardown during a
// load, so every deref is fault-tolerant.
struct LoadProbe { void *localplayer; uint32_t mapid; bool ok; };
__declspec(noinline) void probe_body(LoadProbe *pr)
{
    if (g_wcm_slot)
    {
        auto *wcm = *reinterpret_cast<uint8_t **>(g_wcm_slot);
        pr->localplayer = wcm ? *reinterpret_cast<void **>(wcm + 0x1E508) : nullptr;
    }
    if (g_mapid_slot)
    {
        auto *singleton = *reinterpret_cast<uint8_t **>(g_mapid_slot);
        if (singleton) pr->mapid = *reinterpret_cast<uint32_t *>(singleton + 0x2c);
    }
    pr->ok = true;
}
void probe_seh(LoadProbe *pr)
{
    pr->localplayer = nullptr;
    pr->mapid = 0;
    pr->ok = false;
    __try { probe_body(pr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { pr->ok = false; }
}

// Raw Win32 only (a stuck load may be blocked inside a lock spdlog also takes — see the freeze
// watchdog's note). Writes the stall .txt + an all-thread minidump from THIS healthy thread.
void write_stall_report(DWORD null_ms, int32_t target, uint32_t last_mapid)
{
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%ls\\MapForGoblins_load_stall_%lu.txt", g_dir,
               GetCurrentProcessId());
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char buf[640];
        int n = _snprintf(buf, sizeof(buf),
                          "MapForGoblins LOAD-STALL watchdog\r\n"
                          "detected      = %04u-%02u-%02u %02u:%02u:%02u\r\n"
                          "LocalPlayer null (world not playable) for ~%lu ms\r\n"
                          "threshold     = %u s (ini [Debug] load_watchdog_secs)\r\n"
                          "warp target   = grace rowId %d (BonfireWarpParam)\r\n"
                          "last MapId    = %08x (area %u, grid %u/%u)\r\n"
                          "The present thread is STILL beating (loading screen renders) so the\r\n"
                          "freeze watchdog can't see this. A bad/hung fast-travel most often\r\n"
                          "stalls in world streaming with no assert.\r\n"
                          "All-thread stacks: MapForGoblins_load_stall_<pid>.dmp next to this file\r\n"
                          "(symbolize MapForGoblins.dll frames with the deployed .pdb).\r\n",
                          st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                          static_cast<unsigned long>(null_ms),
                          static_cast<unsigned>(goblin::config::loadWatchdogSecs),
                          static_cast<int>(target), static_cast<unsigned>(last_mapid),
                          (last_mapid >> 24) & 0xff, (last_mapid >> 16) & 0xff,
                          (last_mapid >> 8) & 0xff);
        DWORD w = 0;
        if (n > 0) WriteFile(f, buf, static_cast<DWORD>(n), &w, nullptr);
        CloseHandle(f);
    }

    _snwprintf(path, MAX_PATH, L"%ls\\MapForGoblins_load_stall_%lu.dmp", g_dir,
               GetCurrentProcessId());
    HANDLE df = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (df != INVALID_HANDLE_VALUE)
    {
        const MINIDUMP_TYPE type =
            static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
        BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), df, type,
                                    nullptr, nullptr, nullptr);
        CloseHandle(df);
        if (!ok) DeleteFileW(path);
    }
}

void watchdog_loop()
{
    const DWORD threshold_ms = static_cast<DWORD>(goblin::config::loadWatchdogSecs) * 1000u;
    // A warp that never even started a load (LocalPlayer never went null) is not a stall — give
    // the load this long to BEGIN before we stop watching this arm.
    const DWORD start_window_ms = 20000u;

    resolve_statics();

    bool saw_null = false;           // load actually started (player went null) this arm
    ULONGLONG null_since = 0;        // tick LocalPlayer first read null this arm
    uint32_t last_mapid = 0;

    for (;;)
    {
        Sleep(500);
        if (!g_armed.load(std::memory_order_relaxed)) continue;

        const ULONGLONG arm = g_arm_tick.load(std::memory_order_relaxed);
        LoadProbe pr{};
        probe_seh(&pr);
        if (!pr.ok) continue;
        if (pr.mapid) last_mapid = pr.mapid;
        const ULONGLONG now = GetTickCount64();

        if (pr.localplayer)
        {
            if (saw_null)
            {
                // Player came back — the load completed. Disarm.
                spdlog::info("[LOADWD] warp target={} loaded OK (LocalPlayer back)",
                             g_target_grace.load());
                g_armed.store(false, std::memory_order_relaxed);
                saw_null = false; null_since = 0;
            }
            else if (now - arm > start_window_ms)
            {
                // 20 s and the load never began — the warp call didn't take us anywhere.
                spdlog::info("[LOADWD] warp target={} never started a load (disarm)",
                             g_target_grace.load());
                g_armed.store(false, std::memory_order_relaxed);
            }
            null_since = 0;
            continue;
        }

        // LocalPlayer == null: a load is in progress.
        saw_null = true;
        if (null_since == 0) null_since = now;
        const DWORD null_ms = static_cast<DWORD>(now - null_since);
        if (null_ms >= threshold_ms)
        {
            spdlog::error("[LOADWD] STUCK LOAD — LocalPlayer null {}ms after warp target={} "
                          "(last mapId {:08x}) → stall report + all-thread minidump",
                          null_ms, g_target_grace.load(), last_mapid);
            write_stall_report(null_ms, g_target_grace.load(), last_mapid);
            g_armed.store(false, std::memory_order_relaxed);
            saw_null = false; null_since = 0;
        }
    }
}
} // namespace

namespace goblin::load_watchdog
{
void arm_warp(int32_t target_grace)
{
    if (goblin::config::loadWatchdogSecs == 0) return;
    g_target_grace.store(target_grace, std::memory_order_relaxed);
    g_arm_tick.store(GetTickCount64(), std::memory_order_relaxed);
    g_armed.store(true, std::memory_order_relaxed);
    spdlog::info("[LOADWD] armed for warp target grace={}", target_grace);
}

void install(const std::filesystem::path &log_dir)
{
    // dllmain installs at two sites (early + post-init, mirroring the freeze watchdog); only the
    // first spawns the poll thread.
    static std::atomic<bool> s_installed{false};
    if (s_installed.exchange(true)) return;
    if (goblin::config::loadWatchdogSecs == 0)
    {
        spdlog::info("[LOADWD] disabled (load_watchdog_secs = 0)");
        return;
    }
    const std::wstring w = log_dir.wstring();
    wcsncpy(g_dir, w.c_str(), MAX_PATH - 1);
    g_dir[MAX_PATH - 1] = L'\0';
    std::thread(watchdog_loop).detach();
    spdlog::info("[LOADWD] armed: stuck world-load (LocalPlayer null > {}s) → load-stall triage "
                 "+ all-thread minidump in logs/ (the freeze watchdog's blind spot)",
                 goblin::config::loadWatchdogSecs);
}
} // namespace goblin::load_watchdog
