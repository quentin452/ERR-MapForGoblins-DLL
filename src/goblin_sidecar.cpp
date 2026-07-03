#include "goblin_sidecar.hpp"

#include "goblin_config.hpp"     // config::sidecarSave
#include "goblin_markers.hpp"    // markers::set_event_flag — flag replay (slice 1b)
#include "goblin_inventory.hpp"  // give_item — custom-item strip/reinject (Phase 2)
#include "modutils.hpp"          // scan/hook — save-fn observer (Phase 2 RE)
#include "re_signatures.hpp"     // sig::SAVE_FN

#include <atomic>
#include <map>
#include <mini/ini.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace fs = std::filesystem;

namespace goblin::sidecar
{
namespace
{
std::mutex g_mtx;
fs::path g_save_path;      // the game's real save file (ER0000.err / .sl2)
fs::path g_mfg_path;       // <save>.mfg — our sidecar
bool g_loaded = false;     // a load() has run for the current path
bool g_dirty = false;      // in-memory state changed since the last save

std::set<uint32_t> g_flags;
std::unordered_map<std::string, std::string> g_kv;
std::map<uint32_t, int32_t> g_items;  // custom item id (category-encoded) → qty (Phase 2)
// Author-surface (custom_items.toml) items: declarative, re-registered from the toml EVERY boot.
// Granted/stripped exactly like g_items, but NEVER written to or cleared by the .mfg (load()/save()
// touch only g_items) — the toml is their source of truth, so they stay save-clean without persisting.
std::map<uint32_t, int32_t> g_author_items;

// Merged view (g_items ∪ g_author_items) for grant/strip. Author qty wins on a collision (unlikely —
// author ids live in a reserved band). Caller holds g_mtx.
std::map<uint32_t, int32_t> all_custom_items_locked()
{
    std::map<uint32_t, int32_t> m = g_items;
    for (auto &[id, qty] : g_author_items) m[id] = qty;
    return m;
}
std::string g_guid;        // self-stamped binding id (forward-compat; identity RE is Phase 1c)

// Lifecycle (slice 1b). g_prev_world_loaded is touched ONLY by tick() (poll thread) — no
// lock. g_replay_pending crosses poll→present, so it's atomic.
bool g_prev_world_loaded = false;
std::atomic<bool> g_replay_pending{false};

// Phase 2 strip/reinject (Variant A — the clean vanilla save). Wired ON: the strip@entry /
// reinject@exit bracket lives in hk_serialize (SERIALIZE_FN 0x67dc00), the whole-slot game-data
// serialize, where the live inventory is written to the out-buffer INSIDE the call. So the custom
// items are removed from the live inventory just before the buffer is written and restored the
// instant it returns — atomic, synchronous, on the save worker thread. This SUPERSEDES the old
// CreateFileW-triggered strip (Phase-2 data-layer), which fired too LATE (ER had already serialized
// the inventory by file-open time; the item survived at full count — see shadow_sidecar_save_plan.md
// Phase 2). Off = the [items] store still persists to the .mfg but nothing touches the live
// inventory. The flag replay (slice 1b) is independent of this flag.
constexpr bool kItemStripReinjectWired = true;

// A stable-ish id for binding the sidecar to a save. Character-identity RE (steam id + slot)
// is deferred (Phase 1c); until then the sidecar binds by living next to the save file, and
// this guid is stamped for the later identity cross-check. Uniqueness across saves on one
// machine is enough here — QPC ^ tick ^ pid, hex.
std::string make_guid()
{
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    uint64_t v = (uint64_t)qpc.QuadPart ^ (GetTickCount64() << 1) ^
                 ((uint64_t)GetCurrentProcessId() << 32);
    char b[24];
    std::snprintf(b, sizeof(b), "%016llx", (unsigned long long)v);
    return b;
}

// Does the path look like an ER save file we should shadow? ER0000.sl2 (vanilla),
// ER0000.err (ERR ME3 redirect), co-op variants — match the `.sl2` / `.err` extension
// on a basename starting "ER" (case-insensitive), skipping `.bak` backups.
bool is_er_save(const fs::path &p)
{
    std::string ext = p.extension().string();
    for (auto &c : ext) c = (char)towlower(c);
    if (ext != ".sl2" && ext != ".err") return false;
    std::string stem = p.stem().string();
    return stem.size() >= 2 && (stem[0] == 'E' || stem[0] == 'e') &&
           (stem[1] == 'R' || stem[1] == 'r');
}

std::vector<std::string> split_csv(const std::string &s)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ','))
        if (!tok.empty()) out.push_back(tok);
    return out;
}

// Serialize the current state to `ini` (caller holds the lock).
void to_ini(mINI::INIStructure &ini)
{
    ini["meta"]["version"] = "1";
    ini["meta"]["guid"] = g_guid;
    ini["meta"]["savefile"] = g_save_path.filename().string();
    std::string flags;
    for (uint32_t f : g_flags)
    {
        if (!flags.empty()) flags += ",";
        flags += std::to_string(f);
    }
    ini["flags"]["custom"] = flags;
    // Custom items: one row per item, key = decimal id, value = qty. These are re-granted on
    // load and stripped around a game save (Phase 2).
    ini["items"].clear();
    for (auto &[id, qty] : g_items) ini["items"][std::to_string(id)] = std::to_string(qty);
    // kv rows live in their own section (mINI lowercases keys — acceptable for our own keys).
    ini["kv"].clear();
    for (auto &[k, v] : g_kv) ini["kv"][k] = v;
}

// Populate state from `ini` (caller holds the lock).
void from_ini(const mINI::INIStructure &ini)
{
    g_flags.clear();
    g_kv.clear();
    g_items.clear();
    if (ini.has("meta") && ini.get("meta").has("guid"))
        g_guid = ini.get("meta").get("guid");
    if (g_guid.empty()) g_guid = make_guid();
    if (ini.has("flags") && ini.get("flags").has("custom"))
        for (auto &tok : split_csv(ini.get("flags").get("custom")))
        {
            try { g_flags.insert((uint32_t)std::stoul(tok)); } catch (...) {}
        }
    if (ini.has("items"))
        for (auto &[k, v] : ini.get("items"))
        {
            try { g_items[(uint32_t)std::stoul(k)] = (int32_t)std::stol(v); } catch (...) {}
        }
    if (ini.has("kv"))
        for (auto &[k, v] : ini.get("kv"))
            g_kv[k] = v;
}

bool save_locked()
{
    if (g_mfg_path.empty()) return false;
    if (g_guid.empty()) g_guid = make_guid();
    mINI::INIStructure ini;
    to_ini(ini);
    // Atomic: generate to a temp sibling, then rename over the target so a crash mid-write
    // never leaves a truncated .mfg. mINI::generate(true) pretty-prints; write to temp path.
    fs::path tmp = g_mfg_path;
    tmp += ".tmp";
    try
    {
        mINI::INIFile f(tmp.string());
        if (!f.generate(ini, true)) return false;
        fs::rename(tmp, g_mfg_path);  // atomic on the same volume
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[SIDECAR] save failed: {}", e.what());
        std::error_code ec;
        fs::remove(tmp, ec);
        return false;
    }
    g_dirty = false;
    spdlog::info("[SIDECAR] saved {} ({} flags, {} kv)", g_mfg_path.string(), g_flags.size(),
                 g_kv.size());
    return true;
}

// SEH-guarded single SetEventFlag call: a flag id from a hand-edited / foreign .mfg could
// be out of range and fault inside the game fn (no bounds guarantee). Wraps a lone opaque
// CALL only (clang-cl keeps that; lint_seh enforces it). Runs on the present thread.
__declspec(noinline) bool seh_set_flag(uint32_t f)
{
    __try { return goblin::markers::set_event_flag(f, 1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Remove all sidecar custom items from the LIVE inventory (give_item negative qty). Called at
// ── Save-function RE probe (one-shot) ────────────────────────────────────────
// Called from the CreateFileW save-WRITE hook, so the stack above us is the game's save
// chain: [note_save_file_opened] ← [hk_create_file_w] ← [game file wrapper] ← … ← [the
// SAVE ROUTINE that serializes then writes]. We want to hook that routine (strip at entry,
// reinject at exit — an atomic bracket around serialization). This logs the call chain: each
// return address as eldenring.exe+RVA, the containing function's ENTRY (found by scanning
// back to the CC-padding boundary), and the entry's first bytes (for an AOB). Read
// docs/plans/shadow_sidecar_save_plan.md Phase 2 — identify the frame that brackets
// serialize+write, then hook it. See [SAVERE] in the log.
// Does buf[k..] look like a real x64 function prologue? (the common MSVC/clang shapes ER uses).
bool looks_like_prologue(const uint8_t *p)
{
    // push rbx/rbp/rsi/rdi
    if (p[0] >= 0x53 && p[0] <= 0x57) return true;
    // 40 53 / 40 55.. (push with REX), 41 54.. (push r12-15)
    if (p[0] == 0x40 && p[1] >= 0x53 && p[1] <= 0x57) return true;
    if (p[0] == 0x41 && p[1] >= 0x54 && p[1] <= 0x57) return true;
    // 48 89 5C/74/7C 24 .. (mov [rsp+x], reg — save reg prologue)
    if (p[0] == 0x48 && p[1] == 0x89 && (p[2] == 0x5C || p[2] == 0x74 || p[2] == 0x7C) && p[3] == 0x24)
        return true;
    // 48 83 EC (sub rsp, imm8) / 48 81 EC (imm32)
    if (p[0] == 0x48 && (p[1] == 0x83 || p[1] == 0x81) && p[2] == 0xEC) return true;
    // 48 8B C4 (mov rax,rsp — frame setup) / 4C 8B DC (mov r11,rsp)
    if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0xC4) return true;
    if (p[0] == 0x4C && p[1] == 0x8B && p[2] == 0xDC) return true;
    return false;
}

uintptr_t find_fn_entry(uintptr_t ra, uintptr_t base, uintptr_t hi)
{
    if (ra < base + 0x8000 || ra > hi) return ra;
    uint8_t buf[0x8000];
    uintptr_t start = ra - sizeof(buf) + 1;  // [ra-0x7FFF, ra]
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (void *)start, buf, sizeof(buf), &got) ||
        got != sizeof(buf))
        return ra;
    // Nearest CC→non-CC transition whose following bytes are a valid prologue (MSVC int3-pads
    // between functions; but CC also appears inside bodies, so validate the prologue shape).
    for (int k = (int)sizeof(buf) - 1; k >= 2; --k)
        if (buf[k - 1] == 0xCC && buf[k] != 0xCC && looks_like_prologue(&buf[k]))
            return start + k;
    // fallback: nearest CC boundary at all.
    for (int k = (int)sizeof(buf) - 1; k >= 1; --k)
        if (buf[k] != 0xCC && buf[k - 1] == 0xCC) return start + k;
    return ra;
}

// ── Save-fn observer (Phase 2 RE, read-only) ─────────────────────────────────
// Hook the outermost save routine (sig::SAVE_FN) and log entry/exit + thread + call count, to
// confirm it fires on a save, is save-specific (not floods every frame / loads), and brackets
// serialize+write (one call per save). NO strip yet. 4-arg passthrough (the fn uses rcx/rdx;
// extra regs are forwarded harmlessly).
using SaveFn = uint64_t(*)(void *, void *, void *, void *);
SaveFn g_orig_save = nullptr;
std::atomic<long> g_save_calls{0};

uint64_t hk_save(void *a, void *b, void *c, void *d)
{
    long n = ++g_save_calls;
    bool log = (n <= 40) || (n % 200 == 0);
    if (log)
        spdlog::info("[SAVEFN] #{} ENTER tid={} a={} b={}", n, GetCurrentThreadId(), a, b);
    uint64_t r = g_orig_save(a, b, c, d);
    if (log) spdlog::info("[SAVEFN] #{} exit", n);
    return r;
}

// Serialize hook (Phase 2 strip/reinject bracket, Variant A). SERIALIZE_FN (0x67dc00) is the whole-slot
// game-data serialize: the live inventory is written to the out-buffer INSIDE this call (after entry,
// via FUN_140257f20). So strip@entry / reinject@exit brackets it atomically — the custom items are gone
// from the live inventory while the buffer is written, then restored the instant the write returns, all
// synchronously on the save worker thread (observer-confirmed tid=344; give_item proven stable there).
// Still logs the caller chain (dedup) for diagnostics. 4-arg passthrough.
using SerFn = uint64_t(*)(void *, void *, void *, void *);
SerFn g_orig_ser = nullptr;
std::atomic<long> g_ser_calls{0};
std::atomic<bool> g_in_serialize{false};  // re-entrancy guard: never double-strip a nested serialize

std::set<uintptr_t> g_ser_callers;  // dedup by direct caller RVA (guarded by g_mtx)

// Snapshot of the nodes strip_items() zeroed at serialize entry; restored at exit. Only ever
// touched on the save thread inside the g_in_serialize bracket, so no extra lock needed.
std::vector<goblin::inventory::StripEntry> g_strip_snapshot;

void strip_items();       // fwd — zero custom-item nodes in the LIVE inventory (snapshot kept)
void restore_stripped();  // fwd — restore the zeroed nodes byte-exact after the buffer is written
void reinject_items();    // fwd — world-enter re-grant of loaded items (give_item +qty)

uint64_t hk_serialize(void *a, void *b, void *c, void *d)
{
    ++g_ser_calls;
    using RtlCapFn = USHORT(WINAPI *)(ULONG, ULONG, PVOID *, PULONG);
    static auto rtl =
        (RtlCapFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlCaptureStackBackTrace");
    void *fr[40] = {};
    USHORT m = rtl ? rtl(1, 40, fr, nullptr) : 0;  // skip our own frame
    uintptr_t base = (uintptr_t)GetModuleHandleA("eldenring.exe");
    uintptr_t direct = 0;
    for (USHORT i = 0; i < m; i++)
        if ((uintptr_t)fr[i] >= base && (uintptr_t)fr[i] < base + 0x10000000)
        { direct = (uintptr_t)fr[i]; break; }
    // Log each DISTINCT direct caller once (dedup) — the save-path caller (SaveLoad2 / content build)
    // shows up as a new entry vs the boot/gameplay noise. Cap at 40 distinct.
    bool fresh = false;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        fresh = direct && g_ser_callers.size() < 300 && g_ser_callers.insert(direct).second;
    }
    if (fresh)
    {
        std::string s;
        for (USHORT i = 0; i < m && (int)s.size() < 480; i++)
        {
            uintptr_t v = (uintptr_t)fr[i];
            if (v >= base && v < base + 0x10000000)
            {
                char x[24];
                std::snprintf(x, sizeof(x), " er+0x%llx", (unsigned long long)(v - base));
                s += x;
            }
        }
        spdlog::info("[SERFN] caller#{} tid={} chain:{}", g_ser_callers.size(),
                     GetCurrentThreadId(), s);
    }

    // Strip@entry / reinject@exit bracket. Guard against a nested serialize double-stripping (they
    // shouldn't nest on one thread, but the guard is cheap insurance). Both halves are no-ops when the
    // [items] store is empty. If g_orig_ser crashes we skip reinject — the game is dying anyway.
    bool bracket =
        kItemStripReinjectWired && !g_in_serialize.exchange(true, std::memory_order_acquire);
    if (bracket) strip_items();
    uint64_t r = g_orig_ser(a, b, c, d);
    if (bracket)
    {
        restore_stripped();
        g_in_serialize.store(false, std::memory_order_release);
    }
    return r;
}

void probe_save_callstack()
{
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;
    using RtlCapFn = USHORT(WINAPI *)(ULONG, ULONG, PVOID *, PULONG);
    auto rtl = (RtlCapFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlCaptureStackBackTrace");
    if (!rtl) { spdlog::warn("[SAVERE] RtlCaptureStackBackTrace unavailable"); return; }
    void *frames[40];
    USHORT n = rtl(0, 40, frames, nullptr);
    uintptr_t base = (uintptr_t)GetModuleHandleA("eldenring.exe");
    uintptr_t hi = base + 0x10000000;  // generous .text upper bound
    spdlog::info("[SAVERE] save write-open callstack ({} frames), er_base={:#x}:", n, base);
    for (USHORT i = 0; i < n; i++)
    {
        uintptr_t ra = (uintptr_t)frames[i];
        if (ra < base || ra > hi) { spdlog::info("[SAVERE]  #{:02d} {:p} (extern)", i, (void *)ra); continue; }
        // Reliable callee entry: the return address ra is right after a `call`. For a direct
        // `E8 rel32` at ra-5, the callee (this frame's INNER function) = ra + int32(ra-4). That
        // is the EXACT entry of the next-inner frame's function — no CC-scan heuristic.
        uint8_t pre[5] = {};
        SIZE_T g2 = 0;
        ReadProcessMemory(GetCurrentProcess(), (void *)(ra - 5), pre, 5, &g2);
        uintptr_t callee = 0;
        if (g2 == 5 && pre[0] == 0xE8)
        {
            int32_t rel = *reinterpret_cast<int32_t *>(pre + 1);
            callee = ra + rel;
        }
        uint8_t ch[16] = {};
        if (callee >= base && callee <= hi)
            ReadProcessMemory(GetCurrentProcess(), (void *)callee, ch, sizeof(ch), &g2);
        spdlog::info("[SAVERE]  #{:02d} ret=er+{:#x}  callee(er+{:#x}) hdr={:02x} {:02x} {:02x} {:02x} "
                     "{:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}",
                     i, ra - base, callee ? callee - base : 0, ch[0], ch[1], ch[2], ch[3], ch[4],
                     ch[5], ch[6], ch[7], ch[8], ch[9], ch[10], ch[11]);
    }
}

// Zero every custom-item node in the live inventory just before the save serialize reads it, so
// the item is written as absent (the clean vanilla save). AddItemFunc can't remove (add-only, see
// goblin_inventory.hpp), so this is a direct node edit via inventory::strip_goods() — the exact
// decrement the game's own FUN_14024bfe0 does. The snapshot is restored byte-exact at exit by
// restore_stripped(). Runs on the save thread inside the g_in_serialize bracket.
void strip_items()
{
    std::vector<uint32_t> ids;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        for (auto &[id, qty] : all_custom_items_locked())
            if (qty > 0) ids.push_back(id);
    }
    g_strip_snapshot = goblin::inventory::strip_goods(ids);
    if (!ids.empty())
        spdlog::info("[SIDECAR] stripped {} node(s) for {} custom item(s) (pre-save)",
                     g_strip_snapshot.size(), ids.size());
}

// Restore the nodes strip_items() zeroed, the instant the serialize write returns.
void restore_stripped()
{
    goblin::inventory::restore_goods(g_strip_snapshot);
    if (!g_strip_snapshot.empty())
        spdlog::info("[SIDECAR] restored {} stripped node(s) (post-save)", g_strip_snapshot.size());
    g_strip_snapshot.clear();
}

// Re-grant sidecar custom items into the LIVE inventory up to their target qty. Called on
// world-enter. IDEMPOTENT: grants only the missing delta (target − current held) via the exact
// give_item(+1) primitive, so a cold load (item stripped from the save, held=0) grants the full
// qty while a warp/area-change re-enter (item still live) grants nothing — no per-save inflation.
// give_item(+N) in one call is unreliable for N>1 (caps at ~1000), hence the +1 loop.
void reinject_items()
{
    std::map<uint32_t, int32_t> items;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        items = all_custom_items_locked();
    }
    int granted = 0;
    for (auto &[id, qty] : items)
    {
        if (qty <= 0) continue;
        uint32_t cur = goblin::inventory::goods_count(id);
        for (uint32_t have = cur; have < static_cast<uint32_t>(qty); ++have)
            if (goblin::inventory::give_item(id, 1)) ++granted;
    }
    if (!items.empty())
        spdlog::info("[SIDECAR] reinjected {} unit(s) across {} custom item(s)", granted, items.size());
}

bool load_locked()
{
    if (g_mfg_path.empty()) return false;
    std::error_code ec;
    if (!fs::exists(g_mfg_path, ec))
    {
        // No sidecar yet for this save — a fresh, empty store bound to this path.
        g_flags.clear();
        g_kv.clear();
        g_guid = make_guid();
        g_loaded = true;
        g_dirty = false;
        spdlog::info("[SIDECAR] no existing sidecar at {} — fresh store", g_mfg_path.string());
        return true;
    }
    mINI::INIStructure ini;
    mINI::INIFile f(g_mfg_path.string());
    if (!f.read(ini))
    {
        spdlog::warn("[SIDECAR] read failed: {}", g_mfg_path.string());
        return false;
    }
    from_ini(ini);
    g_loaded = true;
    g_dirty = false;
    spdlog::info("[SIDECAR] loaded {} ({} flags, {} kv)", g_mfg_path.string(), g_flags.size(),
                 g_kv.size());
    return true;
}
}  // namespace

void reset_serialize_probe()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_ser_callers.clear();
    g_ser_calls.store(0);
    spdlog::info("[SERFN] probe reset — capturing fresh caller chains");
}

void install_save_hook()
{
    if (!config::sidecarSave) return;
    void *fn = modutils::scan<void>({.aob = goblin::sig::SAVE_FN});
    if (!fn) { spdlog::warn("[SAVEFN] AOB not found — observer skipped"); return; }
    try
    {
        modutils::hook(fn, reinterpret_cast<void *>(hk_save),
                       reinterpret_cast<void **>(&g_orig_save));
        spdlog::info("[SAVEFN] observer hooked @ {}", fn);
    }
    catch (const std::exception &e)
    {
        spdlog::error("[SAVEFN] hook failed: {}", e.what());
    }
    // Serialize observer (Phase 2 RE). Hook the CONFIRMED whole-slot game-data serialize
    // (FUN_14067dc00, RVA 0x67dc00) found in Ghidra via find_serialize.java — SERIALIZE_FN AOB
    // (unique in .text, version-independent). READ-ONLY: confirm it fires ONCE per real save
    // (grace-rest / quit) + note its thread before flipping kItemStripReinjectWired. Supersedes the
    // old hardcoded 0x1ede700 (a DLOutputStream primitive, not the orchestrator).
    void *sf = modutils::scan<void>({.aob = goblin::sig::SERIALIZE_FN});
    if (!sf) { spdlog::warn("[SERFN] SERIALIZE_FN AOB not found — observer skipped"); return; }
    try
    {
        modutils::hook(sf, reinterpret_cast<void *>(hk_serialize),
                       reinterpret_cast<void **>(&g_orig_ser));
        spdlog::info("[SERFN] observer hooked @ {}", sf);
    }
    catch (const std::exception &e)
    {
        spdlog::error("[SERFN] hook failed: {}", e.what());
    }
}

void note_save_file_opened(const wchar_t *path, bool for_write)
{
    if (!config::sidecarSave || !path) return;
    fs::path p;
    try { p = fs::path(path); } catch (...) { return; }
    if (!is_er_save(p)) return;

    bool saving = false;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        // First sighting (or the game switched save files): bind + load the matching sidecar.
        if (g_save_path != p)
        {
            g_save_path = p;
            g_mfg_path = p;
            g_mfg_path.replace_extension(".mfg");
            g_loaded = false;
            spdlog::info("[SIDECAR] save file {} -> sidecar {}", p.string(), g_mfg_path.string());
            load_locked();
        }
        // A write open = the game is saving → persist our sidecar alongside it.
        if (for_write && g_loaded)
        {
            save_locked();
            saving = true;
        }
    }
    // NB the item strip/reinject is NOT done here. The CreateFileW write-open fires too LATE — ER has
    // already serialized the inventory buffer by then (empirically the item survived at full count).
    // The atomic bracket lives in hk_serialize (SERIALIZE_FN) instead, where the inventory is written
    // INSIDE the call. This path only persists our sidecar .mfg alongside the game save (above).
    // Keep the one-shot save-path stack walk for diagnostics only.
    if (saving) probe_save_callstack();
}

std::string sidecar_path_utf8()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_mfg_path.string();
}

void add_custom_flag(uint32_t flag_id)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_flags.insert(flag_id).second) g_dirty = true;
}

void remove_custom_flag(uint32_t flag_id)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_flags.erase(flag_id)) g_dirty = true;
}

bool has_custom_flag(uint32_t flag_id)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_flags.count(flag_id) != 0;
}

std::vector<uint32_t> custom_flags()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return {g_flags.begin(), g_flags.end()};
}

void add_custom_item(uint32_t item_id, int32_t qty)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_items[item_id] += qty;
    if (g_items[item_id] <= 0) g_items.erase(item_id);
    g_dirty = true;
}

void remove_custom_item(uint32_t item_id)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_items.erase(item_id)) g_dirty = true;
}

std::vector<std::pair<uint32_t, int32_t>> custom_items()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return {g_items.begin(), g_items.end()};
}

void register_author_item(uint32_t item_id, int32_t qty)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (qty > 0) g_author_items[item_id] = qty;
    else g_author_items.erase(item_id);
    // NB not marked dirty and never written to the .mfg — the toml is the source of truth.
}

void set_kv(const std::string &key, const std::string &value)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_kv[key] = value;
    g_dirty = true;
}

std::string get_kv(const std::string &key)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_kv.find(key);
    return it == g_kv.end() ? std::string{} : it->second;
}

bool load()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return load_locked();
}

bool save()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return save_locked();
}

std::string status_line()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    std::string p = g_mfg_path.empty() ? "(none)" : g_mfg_path.string();
    return "path=" + p + " loaded=" + (g_loaded ? "1" : "0") +
           " flags=" + std::to_string(g_flags.size()) + " items=" + std::to_string(g_items.size()) +
           " kv=" + std::to_string(g_kv.size()) + " dirty=" + (g_dirty ? "1" : "0");
}

void tick(bool world_loaded)
{
    if (!config::sidecarSave) return;
    // Single-threaded state (poll thread only) — no lock for the edge tracker.
    if (world_loaded && !g_prev_world_loaded)
    {
        // Entered the world (the game has loaded its own save flags by now) → queue a
        // replay onto the present thread. Re-firing is harmless: SetEventFlag(f,1) is
        // idempotent, so a transient world_loaded blip that re-edges just re-applies.
        g_replay_pending.store(true, std::memory_order_relaxed);
        spdlog::info("[SIDECAR] world entered — flag replay queued");
    }
    else if (!world_loaded && g_prev_world_loaded)
    {
        // Left to the title / a load screen → persist dirty state (belt-and-suspenders
        // alongside the game-save write signal in note_save_file_opened).
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_dirty && !g_mfg_path.empty()) save_locked();
    }
    g_prev_world_loaded = world_loaded;
}

void pump_present()
{
    if (!config::sidecarSave) return;

    // World-enter replay (queued by tick()): re-apply custom flags AND re-grant custom items
    // (the load half of strip/reinject — the save is clean, so the sidecar restores them).
    if (g_replay_pending.exchange(false, std::memory_order_relaxed))
    {
        std::vector<uint32_t> flags;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            flags.assign(g_flags.begin(), g_flags.end());
        }
        int ok = 0;
        for (uint32_t f : flags)
            if (seh_set_flag(f)) ++ok;
        if (!flags.empty())
            spdlog::info("[SIDECAR] replayed {}/{} custom event flags into the session", ok,
                         flags.size());
        if (kItemStripReinjectWired) reinject_items();
    }
    // No after-save timer reinject here — the strip/reinject is now atomic inside hk_serialize
    // (SERIALIZE_FN), so the item is restored synchronously the instant the save buffer is written.
}
}  // namespace goblin::sidecar
