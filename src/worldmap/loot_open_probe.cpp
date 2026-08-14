#include "loot_open_probe.hpp"

#include "../goblin_config.hpp"
#include "../goblin_sidecar.hpp"  // note_save_file_opened — sidecar save detection (Phase 1)
#include "../modutils.hpp"
#include "loot_disk.hpp"  // on_map_opened_path — CreateFileW map-dir discovery; parse_tile

#include <spdlog/spdlog.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace goblin::worldmap
{
namespace
{
using CreateFileWFn = HANDLE(WINAPI *)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD,
                                       HANDLE);
CreateFileWFn o_create_file_w = nullptr;

std::atomic<long> g_map_opens{0};   // count of map-file opens seen
std::atomic<long> g_boot_opens{0};  // [BOOTIO] count of ALL opens seen
LARGE_INTEGER     g_qpf{};          // perf-counter frequency
LARGE_INTEGER     g_armed{};        // perf-counter at arming (timeline t0)

// [BOOTIO] per-open lines cap — boots open a few hundred to a couple thousand
// files; past the cap we only emit a running count so the log stays bounded.
constexpr long BOOT_IO_MAX_LINES = 1500;
constexpr long BOOT_IO_TICK      = 500;
std::mutex g_boot_io_mx;  // serialize [BOOTIO] writes (opens land on many threads)

// Case-insensitive suffix test on the raw wide path. Avoids constructing
// std::wstring on EVERY file open in the game.
bool ends_ci(LPCWSTR p, const wchar_t *suf)
{
    if (!p || !suf) return false;
    size_t n = 0;
    while (p[n] && n < 0x8000) ++n;
    size_t slen = 0;
    while (suf[slen]) ++slen;
    if (n < slen) return false;
    const wchar_t *e = p + (n - slen);
    for (size_t i = 0; i < slen; ++i)
        if (towlower(e[i]) != suf[i]) return false;
    return true;
}

// Game data files the game opens that our readers care about: the parseable MapStudio MSBs
// (.msb.dcx / .msb), the tile bundle (.mapbnd / .mapbnd.dcx — recorded for the Oodle-join
// slice), and the msg/menu/event resources (msgbnd, tpf, sblytbnd, tpfbhd/bdt, emevd) — the
// exact-path capture is the mod-agnostic answer for THOSE readers too (Golden Age ships its
// own msg/menu/event under <root>/GA/ while the ancestor walk resolves vanilla — see the
// [GAMEFILE]/EMEVD wrong-path audit 2026-08-14).
bool is_map_file(LPCWSTR p)
{
    return ends_ci(p, L".msb.dcx") || ends_ci(p, L".msb") || ends_ci(p, L".mapbnd.dcx") ||
           ends_ci(p, L".mapbnd") || ends_ci(p, L".msgbnd.dcx") || ends_ci(p, L".msgbnd") ||
           ends_ci(p, L".emevd.dcx") || ends_ci(p, L".tpf.dcx") || ends_ci(p, L".tpf") ||
           ends_ci(p, L".sblytbnd.dcx") || ends_ci(p, L".tpfbhd") || ends_ci(p, L".tpfbdt");
}
bool is_msb_file(LPCWSTR p)
{
    return ends_ci(p, L".msb.dcx") || ends_ci(p, L".msb");
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

// ── Captured map-path registry (the resident-MSB path source's enumeration) ──
std::mutex                   g_map_paths_mx;
std::vector<CapturedMapFile> g_map_paths;  // successful opens only, deduped by exact path

void record_map_open(LPCWSTR name)
{
    if (!name) return;
    std::string s = to_utf8(name);
    if (s.empty()) return;
    CapturedMapFile f;
    f.path = s;
    f.isMsb = is_msb_file(name);
    // Tile name from the filename (free by construction — the path IS the file's identity):
    // strip the known suffix, parse the m{AA}_{BB}_{CC}_00 stem (LOD0 rule, shared with the
    // disk loader). Non-tile map files (rare) stay recorded for the join, just unnamed.
    const char *stripped = nullptr;
    if (ends_ci(name, L".msb.dcx")) stripped = ".msb.dcx";
    else if (ends_ci(name, L".msb")) stripped = ".msb";
    else if (ends_ci(name, L".mapbnd.dcx")) stripped = ".mapbnd.dcx";
    else if (ends_ci(name, L".mapbnd")) stripped = ".mapbnd";
    if (stripped)
    {
        std::string stem = s.substr(0, s.size() - std::string(stripped).size());
        size_t sep = stem.find_last_of("/\\");
        if (sep != std::string::npos) stem = stem.substr(sep + 1);
        int a = 0, x = 0, z = 0;
        if (parse_tile(stem, a, x, z))
        {
            f.name = stem;
            f.area = (uint8_t)a;
            f.gx = (uint8_t)x;
            f.gz = (uint8_t)z;
        }
    }
    std::lock_guard<std::mutex> lk(g_map_paths_mx);
    for (const auto &p : g_map_paths)
        if (p.path == f.path) return;  // same tile re-streamed — keep the first
    g_map_paths.push_back(std::move(f));
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
    const bool is_map = is_map_file(name);
    const bool verbose = is_map && config::diagMapOpens;
    const bool boot_io = config::diagBootIo;
    LARGE_INTEGER t0{};
    if (verbose || boot_io) QueryPerformanceCounter(&t0);

    HANDLE h = o_create_file_w(name, access, share, sa, disp, flags, tmpl);

    if (boot_io) log_boot_open(name, h, t0);

    // Sidecar save (Phase 1): the game opening its save file (ER0000.err / .sl2) is our
    // load/save signal. GENERIC_WRITE in the access mask = a save in progress vs a load
    // read. Cheap filter (extension + basename) lives inside note_save_file_opened; a no-op
    // unless [Sidecar] sidecar_save is on. Only act on a successful open.
    if (config::sidecarSave && h != INVALID_HANDLE_VALUE)
        goblin::sidecar::note_save_file_opened(name, (access & GENERIC_WRITE) != 0);

    if (is_map)
    {
        if (h != INVALID_HANDLE_VALUE)
            record_map_open(name);  // exact-path capture for the resident-MSB path source
        // Discovery: feed the real resolved path to the disk-loot map-dir fallback
        // (cheap no-op once the dir is Found). This is what completes a Searching
        // state when the ancestor-walk missed the mod's map folder. MSB opens only —
        // the .mapbnd parent (map\) is not a MapStudio dir.
        if (is_msb_file(name))
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
                spdlog::info("[MAPOPEN] (further map-file opens suppressed; still counting)");
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
    if (!config::diagMapOpens && !config::lootFromDiskMsb && !config::diagBootIo &&
        !config::sidecarSave)
        return;
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
        spdlog::info("[MAPOPEN] CreateFileW observer armed ({}{}{}). Watches map-file opens "
                     "(.msb.dcx/.msb/.mapbnd[.dcx]){}.",
                     config::lootFromDiskMsb ? "map-dir discovery + path capture" : "",
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

std::vector<CapturedMapFile> captured_map_files()
{
    std::lock_guard<std::mutex> lk(g_map_paths_mx);
    return g_map_paths;
}

std::string captured_path_for(const std::string &rel_path)
{
    // Exact captured path for a VIRTUAL game path (e.g. "msg/engus/item_dlc02.msgbnd.dcx"):
    // the game opens its real files through the loader's redirect (below CreateFileW), so the
    // captured path's TAIL is the virtual path — match on it. This is the mod-agnostic ground
    // truth for read_game_file_decompressed / read_loose_file_decompressed: read the file the
    // GAME actually uses, before any ancestor-walk re-resolution (which misses exotic mounts
    // like GA's <root>/GA/ and silently falls back to the vanilla install).
    if (rel_path.empty()) return {};
    std::string rel = rel_path;
    for (char &c : rel) c = (char)std::tolower((unsigned char)c);
    std::lock_guard<std::mutex> lk(g_map_paths_mx);
    for (const auto &f : g_map_paths)
    {
        std::string p = f.path;
        for (char &c : p) c = (char)std::tolower((unsigned char)c);
        if (p.size() > rel.size() + 1 &&
            (p[p.size() - rel.size() - 1] == '\\' || p[p.size() - rel.size() - 1] == '/') &&
            p.compare(p.size() - rel.size(), rel.size(), rel) == 0)
            return f.path;
    }
    return {};
}
} // namespace goblin::worldmap
