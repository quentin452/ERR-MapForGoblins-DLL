#include "loot_open_probe.hpp"

#include "../goblin_config.hpp"
#include "../modutils.hpp"

#include <spdlog/spdlog.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <string>

namespace goblin::worldmap
{
namespace
{
using CreateFileWFn = HANDLE(WINAPI *)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD,
                                       HANDLE);
CreateFileWFn o_create_file_w = nullptr;

std::atomic<long> g_map_opens{0};   // count of .msb.dcx opens seen
LARGE_INTEGER     g_qpf{};          // perf-counter frequency
LARGE_INTEGER     g_armed{};        // perf-counter at arming (timeline t0)

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

HANDLE WINAPI hk_create_file_w(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                               DWORD disp, DWORD flags, HANDLE tmpl)
{
    const bool is_map = ends_msb_dcx(name);
    LARGE_INTEGER t0{};
    if (is_map) QueryPerformanceCounter(&t0);

    HANDLE h = o_create_file_w(name, access, share, sa, disp, flags, tmpl);

    if (is_map)
    {
        LARGE_INTEGER t1{};
        QueryPerformanceCounter(&t1);
        const double open_us =
            g_qpf.QuadPart ? (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)g_qpf.QuadPart : 0.0;
        const long n = ++g_map_opens;
        const bool ok = (h != INVALID_HANDLE_VALUE);
        if (n <= 30)
            spdlog::info("[MAPOPEN #{}] +{:.0f}ms  open={:.0f}us  {}  {}", n, ms_since_armed(t1),
                         open_us, ok ? "ok" : "FAIL", to_utf8(name));
        else if (n == 31)
            spdlog::info("[MAPOPEN] (further .msb.dcx opens suppressed; still counting)");
    }
    return h;
}
} // namespace

void install_map_open_probe()
{
    if (!config::diagMapOpens) return;
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
        modutils::hook(fn, (void *)&hk_create_file_w, (void **)&o_create_file_w);
        spdlog::info("[MAPOPEN] probe armed — hooking kernel32!CreateFileW, logging *.msb.dcx opens. "
                     "If you see ZERO [MAPOPEN] lines after opening the map, the engine bypasses "
                     "CreateFileW (retry via NtCreateFile).");
    }
    catch (const std::exception &e)
    {
        spdlog::error("[MAPOPEN] hook install failed: {}", e.what());
    }
}
} // namespace goblin::worldmap
