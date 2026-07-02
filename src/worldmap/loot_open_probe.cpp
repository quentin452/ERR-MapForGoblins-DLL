#include "loot_open_probe.hpp"

#include "../goblin_config.hpp"
#include "../modutils.hpp"
#include "loot_disk.hpp"  // on_map_opened_path — CreateFileW map-dir discovery

#include <spdlog/spdlog.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>

namespace goblin::worldmap
{
namespace
{
using CreateFileWFn = HANDLE(WINAPI *)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD,
                                       HANDLE);
CreateFileWFn o_create_file_w = nullptr;

std::atomic<long> g_map_opens{0};   // count of .msb.dcx opens seen
std::atomic<long> g_boot_opens{0};  // [BOOTIO] count of ALL opens seen
LARGE_INTEGER     g_qpf{};          // perf-counter frequency
LARGE_INTEGER     g_armed{};        // perf-counter at arming (timeline t0)

// [BOOTIO] per-open lines cap — boots open a few hundred to a couple thousand
// files; past the cap we only emit a running count so the log stays bounded.
constexpr long BOOT_IO_MAX_LINES = 1500;
constexpr long BOOT_IO_TICK      = 500;
std::mutex g_boot_io_mx;  // serialize [BOOTIO] writes (opens land on many threads)

// Cheap suffix test on the raw wide path: does it end with ".msb.dcx"? (case-
// insensitive). Avoids constructing std::wstring on EVERY file open in the game.
bool ends_msb_dcx(LPCWSTR p)
{
    if (!p) return false;
    size_t n = 0;
    while (p[n] && n < 0x8000) ++n;
    static const wchar_t suf[] = L".msb.dcx";  // 8 chars
    if (n < 8) return false;
    const wchar_t *e = p + (n - 8);
    for (int i = 0; i < 8; ++i)
        if (towlower(e[i]) != suf[i]) return false;
    return true;
}

std::string to_utf8(LPCWSTR w)
{
    if (!w) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string s((size_t)(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    return s;
}

double ms_since_armed(const LARGE_INTEGER &now)
{
    if (!g_qpf.QuadPart) return 0.0;
    return (double)(now.QuadPart - g_armed.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
}

void log_boot_open(LPCWSTR name, HANDLE h, const LARGE_INTEGER &t0)
{
    LARGE_INTEGER t1{};
    QueryPerformanceCounter(&t1);
    const long n = ++g_boot_opens;
    if (n > BOOT_IO_MAX_LINES)
    {
        if (n % BOOT_IO_TICK == 0)
        {
            std::lock_guard<std::mutex> lk(g_boot_io_mx);
            spdlog::info("[BOOTIO] {} opens so far (+{:.0f}ms; per-open lines capped at {})", n,
                         ms_since_armed(t1), BOOT_IO_MAX_LINES);
        }
        return;
    }
    const double open_us = g_qpf.QuadPart
                               ? (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)g_qpf.QuadPart
                               : 0.0;
    const bool ok = (h != INVALID_HANDLE_VALUE);
    std::lock_guard<std::mutex> lk(g_boot_io_mx);
    spdlog::info("[BOOTIO #{}] +{:.0f}ms open={:.0f}us {}  {}", n, ms_since_armed(t1), open_us,
                 ok ? "ok" : "FAIL", to_utf8(name));
}

HANDLE WINAPI hk_create_file_w(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                               DWORD disp, DWORD flags, HANDLE tmpl)
{
    const bool is_map = ends_msb_dcx(name);
    const bool verbose = is_map && config::diagMapOpens;
    const bool boot_io = config::diagBootIo;
    LARGE_INTEGER t0{};
    if (verbose || boot_io) QueryPerformanceCounter(&t0);

    HANDLE h = o_create_file_w(name, access, share, sa, disp, flags, tmpl);

    if (boot_io) log_boot_open(name, h, t0);

    if (is_map)
    {
        // Discovery: feed the real resolved path to the disk-loot map-dir fallback
        // (cheap no-op once the dir is Found). This is what completes a Searching
        // state when the ancestor-walk missed the mod's map folder.
        on_map_opened_path(name);
        if (verbose)
        {
            LARGE_INTEGER t1{};
            QueryPerformanceCounter(&t1);
            const double open_us = g_qpf.QuadPart
                                       ? (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)g_qpf.QuadPart
                                       : 0.0;
            const long n = ++g_map_opens;
            const bool ok = (h != INVALID_HANDLE_VALUE);
            if (n <= 30)
                spdlog::info("[MAPOPEN #{}] +{:.0f}ms  open={:.0f}us  {}  {}", n, ms_since_armed(t1),
                             open_us, ok ? "ok" : "FAIL", to_utf8(name));
            else if (n == 31)
                spdlog::info("[MAPOPEN] (further .msb.dcx opens suppressed; still counting)");
        }
    }
    return h;
}
} // namespace

void install_map_open_probe()
{
    // Armed when the verbose probe is on OR the disk-loot feature is on (the
    // observer doubles as the map-dir discovery fallback — see on_map_opened_path)
    // OR the boot I/O profile is on. Idempotent: called from the early boot-io
    // arm point AND the normal post-from_params site.
    static std::atomic<bool> installed{false};
    if (!config::diagMapOpens && !config::lootFromDiskMsb && !config::diagBootIo) return;
    if (installed.exchange(true)) return;
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_armed);
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    void *fn = k ? (void *)GetProcAddress(k, "CreateFileW") : nullptr;
    if (!fn)
    {
        spdlog::warn("[MAPOPEN] kernel32!CreateFileW not found — probe disabled");
        return;
    }
    try
    {
        // Boot-io must observe the WHOLE boot, so it can't wait for enable_hooks()
        // (end of init, ~14s in) — enable immediately. The map-dir/[MAPOPEN] roles
        // keep the queued path (first map open is minutes later; no need to differ).
        if (config::diagBootIo)
            modutils::hook_now(fn, (void *)&hk_create_file_w, (void **)&o_create_file_w);
        else
            modutils::hook(fn, (void *)&hk_create_file_w, (void **)&o_create_file_w);
        spdlog::info("[MAPOPEN] CreateFileW observer armed ({}{}{}). Watches *.msb.dcx opens{}.",
                     config::lootFromDiskMsb ? "map-dir discovery" : "",
                     config::diagMapOpens ? (config::lootFromDiskMsb ? " + verbose log" : "verbose log")
                                          : "",
                     config::diagBootIo ? " + BOOT-IO all-files profile (live now)" : "",
                     config::diagBootIo ? " + ALL opens" : "");
    }
    catch (const std::exception &e)
    {
        spdlog::error("[MAPOPEN] hook install failed: {}", e.what());
    }
}
} // namespace goblin::worldmap
