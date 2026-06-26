#include "goblin_worldmap_probe.hpp"
#include "goblin_bench.hpp"    // GOBLIN_BENCH_QUIET — make dev dump cost visible in the report
#include "goblin_inject.hpp"   // goblin::world_map_open() — gate the scan on map-open
#include "goblin_config.hpp"   // config::dumpConverters
#include "re_signatures.hpp"   // centralized image RVAs
#include "modutils.hpp"        // AOB scan (render re-apply fn resolve)

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace goblin::worldmap_probe
{
namespace
{
std::shared_ptr<spdlog::logger> g_log;

// Last active map cursor (the one with a valid WorldMapArea/zoom). Published by
// the probe loop, read by get_live_view() on the render thread for the overlay-
// markers prototype. 0 = no live cursor seen yet / map closed.
std::atomic<uintptr_t> g_active_cursor{0};

// The eldenring.exe image range [base, end). A STATIC/default cursor template
// lives in here with a constant junk view (zoom≈4.88, coord≈0); only the real
// map cursor's WorldMapArea is HEAP-allocated (outside the image). Rejecting
// cursors whose +0xF0 view points inside the image kills that false positive
// (it was thrashing g_active_cursor every tick → first-open lag / jumps).
bool exe_range_inited = false;
uintptr_t g_exe_base = 0, g_exe_end = 0;

void init_exe_range()
{
    if (exe_range_inited)
        return;
    exe_range_inited = true;
    g_exe_base = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!g_exe_base)
        return;
    // PE: e_lfanew @ base+0x3C; SizeOfImage @ base + e_lfanew + 0x50 (PE64).
    uint32_t e_lfanew = *reinterpret_cast<uint32_t *>(g_exe_base + 0x3C);
    uint32_t size_of_image = *reinterpret_cast<uint32_t *>(g_exe_base + e_lfanew + 0x50);
    g_exe_end = g_exe_base + size_of_image;
}

// True if `view` points inside the exe image = the static cursor template, not a
// live heap WorldMapArea.
bool view_is_static(uintptr_t view)
{
    init_exe_range();
    return g_exe_base && view >= g_exe_base && view < g_exe_end;
}

// Cheap sanity gate before dereferencing a read-from-memory pointer (the cursor's
// +0xF0 view can be mid-transition garbage on map/DLC load). SEH is the real net,
// but clang-cl SEH is fragile, so reject implausible pointers up front too.
bool plausible_ptr(uintptr_t p)
{
    return p >= 0x10000 && p < 0x7fffffffffffull && (p & 7) == 0;
}

// CS::WorldMapCursorControl vtable RVA (doc imagebase 0x140000000). The cursor
// object's first qword IS this vtable ptr, so scanning memory for this value
// finds cursor instances directly. Resolve-by-RVA: if a game patch shifted it,
// the scan finds nothing → re-derive from the cursor ctor AOB (doc Goal 2).
constexpr uintptr_t CURSOR_VTABLE_RVA = goblin::sig::CURSOR_VTABLE_RVA;
constexpr ptrdiff_t OFF_X = 0xFC, OFF_Z = 0x104, OFF_Y = 0x10C;
constexpr uintptr_t CURSOR_OFF_IN_MENU = 0x2DB0;
// CSMenuMan static slot (RVA, re_v? Ghidra c843cc3). The world-map cursor =
// WorldMapDialog + 0x2DB0, and the dialog ptr lives somewhere in the first few KB
// of CSMenuMan → a BOUNDED walk (vs the whole-RAM scan) resolves it in O(KB).
constexpr uintptr_t CSMENUMAN_SLOT_RVA = goblin::sig::CSMENUMAN_SLOT_RVA;
constexpr uintptr_t MENU_WALK_WINDOW = 0x10000; // dialog ptr "in the first few KB"; widen to be safe

// PROJECTION transform-scan (docs/world_map_projection_re_findings.md). cursor+0xF0
// points to the CS::WorldMapArea view object; the LIVE viewport is plain floats there:
//   +0x378 = pan (vec2, marker space)   +0x380 = zoom (f32)
//   +0x350..0x35c = STATIC full-map rect [0,0,10496,10496] (sanity: should NOT move)
// The make-or-break test: PAN should sweep +0x378; ZOOM should change +0x380; the
// +0x350 rect should stay put. All reads are SEH-guarded + read-only.
constexpr ptrdiff_t OFF_VIEW_PTR = 0xF0;     // cursor -> WorldMapArea
constexpr ptrdiff_t VIEW_PAN_X = 0x378, VIEW_PAN_Z = 0x37C, VIEW_ZOOM = 0x380;
constexpr ptrdiff_t VIEW_FULLRECT = 0x350; // static full-map rect [minX,minZ,maxX,maxZ] (clamp bound)
// Virtual UI canvas singleton: [DAT_1447ef360 + 0x128] -> {+0x110 originX, +0x114
// originY, +0x118 width, +0x11c height}. RVA, patch-fragile (debug only).
constexpr uintptr_t CANVAS_SINGLETON_RVA = goblin::sig::CANVAS_SINGLETON_RVA;

// Safe reads via ReadProcessMemory (NOT __try). clang-cl proved the plain load
// "can't fault" and ELIDED the __try/__except even with __declspec(noinline) — the
// guard was silently dropped and bad pointers faulted unhandled (0xC0000005 at the
// outlined `*out=*src` leaf, decoded from the crash RVA). RPM is an opaque kernel
// call the optimizer can't elide or prove safe: an invalid src returns false, never
// crashes. Slightly slower (a syscall) but bulletproof — fine once the O(KB) walk
// replaces the whole-RAM scan.
inline bool seh_read8(const void *src, uint64_t *out)
{
    SIZE_T n = 0;
    return ReadProcessMemory(GetCurrentProcess(), src, out, sizeof(*out), &n) && n == sizeof(*out);
}
inline bool seh_read4(const void *src, float *out)
{
    SIZE_T n = 0;
    return ReadProcessMemory(GetCurrentProcess(), src, out, sizeof(*out), &n) && n == sizeof(*out);
}
inline bool seh_read_i32(const void *src, int *out)
{
    SIZE_T n = 0;
    return ReadProcessMemory(GetCurrentProcess(), src, out, sizeof(*out), &n) && n == sizeof(*out);
}
// Safe writes via WriteProcessMemory — bad/read-only dst returns false, never crashes.
inline bool seh_write_i32(void *dst, int v)
{
    SIZE_T n = 0;
    return WriteProcessMemory(GetCurrentProcess(), dst, &v, sizeof(v), &n) && n == sizeof(v);
}
inline bool seh_write_f32(void *dst, float v)
{
    SIZE_T n = 0;
    return WriteProcessMemory(GetCurrentProcess(), dst, &v, sizeof(v), &n) && n == sizeof(v);
}
inline bool seh_write_u8(void *dst, uint8_t v)
{
    SIZE_T n = 0;
    return WriteProcessMemory(GetCurrentProcess(), dst, &v, 1, &n) && n == 1;
}

// (The whole-address-space vtable scan was removed — it was O(GB), crashed at
// startup, and is superseded by the O(KB) CSMenuMan walk below.)

// O(KB) resolve: CSMenuMan → (bounded walk) → WorldMapDialog → +0x2DB0 = cursor.
// Replaces the whole-address-space vtable scan (Ghidra c843cc3): the dialog pointer
// is in the first few KB of CSMenuMan, and *(dialog + 0x2DB0) == the cursor vtable is
// the ground-truth check. Returns the cursor address, 0 if not found / map closed.
// Logs the winning offset once → can be hardcoded to a true O(1) deref next patch.
uintptr_t resolve_cursor_via_menu(uintptr_t base, uintptr_t vtable_va)
{
    uint64_t mm = 0;
    bool mm_ok = seh_read8(reinterpret_cast<void *>(base + CSMENUMAN_SLOT_RVA), &mm);
    static int diag = 0;
    if (diag < 3)
    {
        ++diag;
        g_log->info("[MENU] walk: CSMenuMan slot {:#x} -> mm={:#x} (read_ok={}, plausible={})",
                    base + CSMENUMAN_SLOT_RVA, mm, mm_ok, mm ? plausible_ptr(mm) : false);
    }
    if (!mm_ok || !mm || !plausible_ptr(mm))
        return 0;
    auto vt_is_cursor = [&](uint64_t obj) {
        uint64_t vt = 0;
        return seh_read8(reinterpret_cast<void *>(obj + CURSOR_OFF_IN_MENU), &vt) && vt == vtable_va;
    };
    // O(1) RESOLVE — the structural chain CSMenuMan+0x80 → +0x10 → WorldMapDialog → +0x2DB0
    // = cursor. The RE (docs/re/windows_worldmap_cursor_o1_re_prompt.md) proved this offset
    // is NOT statically resolvable (CSMenuMan is a 471-ref hot global, the dialog is
    // factory-created) → it had to be captured at runtime; the offsets below were lifted
    // from the [MENU] log on this build. Seeded so the cursor resolves in two derefs from
    // the FIRST tick — no scan in normal operation, and reopens/grace-rests resolve O(1)
    // too (only the pointer values change; the field offsets are stable). When the map is
    // CLOSED these derefs fail (no dialog) → return 0 without scanning, offsets untouched.
    // off2==~0 → 1-level (mm+off1 = dialog).
    static uintptr_t c_off1 = 0x80, c_off2 = 0x10;
    {
        uint64_t p = 0;
        if (seh_read8(reinterpret_cast<void *>(mm + c_off1), &p) && plausible_ptr(p))
        {
            uint64_t dlg = p;
            if (c_off2 != ~uintptr_t(0))
            {
                uint64_t q = 0;
                dlg = (seh_read8(reinterpret_cast<void *>(p + c_off2), &q) && plausible_ptr(q))
                          ? q : 0;
            }
            if (dlg && vt_is_cursor(dlg))
                return dlg + CURSOR_OFF_IN_MENU;
        }
    }

    // O(1) missed. Map shut ⇒ there is no dialog ⇒ return 0 (NO scan). Only a miss while the
    // map is OPEN means the seeded offsets drifted (a game patch) — then re-discover by
    // scan and re-seed c_off1/c_off2 so O(1) resumes. The scan budget resets every time the
    // map opens (per-session, NOT per-lifetime — a lifetime cap once disabled the scan
    // forever and the map "died" permanently, commit f9c283b).
    const bool open = goblin::world_map_open();
    static bool s_was_open = false;
    static int s_scan_budget = 8;
    if (open && !s_was_open) s_scan_budget = 8;
    s_was_open = open;
    if (!open) return 0;
    if (s_scan_budget <= 0) return 0; // unfindable on this patch — avoid a per-tick mega-scan
    --s_scan_budget;

    // LEVEL 1: the dialog is a flat field of CSMenuMan.
    for (uintptr_t o1 = 0; o1 < MENU_WALK_WINDOW; o1 += 8)
    {
        uint64_t p = 0;
        if (!seh_read8(reinterpret_cast<void *>(mm + o1), &p) || !plausible_ptr(p))
            continue;
        if (vt_is_cursor(p))
        {
            c_off1 = o1; c_off2 = ~uintptr_t(0);
            g_log->info("[MENU] re-seed cursor via CSMenuMan+{:#x} (L1) → cursor {:#x}",
                        o1, p + CURSOR_OFF_IN_MENU);
            return p + CURSOR_OFF_IN_MENU;
        }
    }
    // LEVEL 2: the dialog is one deref deeper (mm → container → dialog).
    for (uintptr_t o1 = 0; o1 < MENU_WALK_WINDOW; o1 += 8)
    {
        uint64_t p = 0;
        if (!seh_read8(reinterpret_cast<void *>(mm + o1), &p) || !plausible_ptr(p))
            continue;
        for (uintptr_t o2 = 0; o2 < 0x800; o2 += 8)
        {
            uint64_t q = 0;
            if (!seh_read8(reinterpret_cast<void *>(p + o2), &q) || !plausible_ptr(q))
                continue;
            if (vt_is_cursor(q))
            {
                c_off1 = o1; c_off2 = o2;
                g_log->info("[MENU] re-seed cursor via CSMenuMan+{:#x} → +{:#x} (L2) → cursor {:#x} "
                            "(update the O(1) seed)", o1, o2, q + CURSOR_OFF_IN_MENU);
                return q + CURSOR_OFF_IN_MENU;
            }
        }
    }
    g_log->info("[MENU] re-discovery pass found no dialog (mm={:#x}, budget {}) — O(1) seed "
                "0x80/0x10 may have drifted; re-RE needed", mm, s_scan_budget);
    return 0;
}

// REGION-FIELD finder: the open-map region (overworld / underground / DLC) is NOT
// the sublayer flag (dead) nor fullRect (constant 10496). Delta-scan the WorldMapDialog
// (cursor−0x2DB0) + the WorldMapArea (view) for any int32 holding a SMALL value that
// CHANGES when the user switches maps → that field is the open-region id. Logs each
// change once; the user opens overworld→underground→DLC and reports which offset flips.
void region_diag(uintptr_t cursor, uintptr_t view)
{
    static std::unordered_map<uintptr_t, int> last;
    auto scan = [&](const char *tag, uintptr_t base_obj, int span) {
        for (int off = 0; off <= span; off += 4)
        {
            int val = 0;
            if (!seh_read_i32(reinterpret_cast<void *>(base_obj + off), &val)) continue;
            uintptr_t key = (base_obj == cursor - CURSOR_OFF_IN_MENU ? 0 : 0x80000000ull) + off;
            auto it = last.find(key);
            if (it != last.end() && it->second != val &&
                val >= 0 && val < 256 && it->second >= 0 && it->second < 256)
                g_log->info("[REGION-DIAG] {}+{:#x}: {} -> {}", tag, off, it->second, val);
            last[key] = val;
        }
    };
    scan("dialog", cursor - CURSOR_OFF_IN_MENU, 0x600);
    if (view) scan("view", view, 0x400);
}

// INPUT-DEVICE delta scan: which f32 fields move under MOUSE vs GAMEPAD-STICK?
// Symptom (2026-06-20): markers update on mouse motion but NOT gamepad stick — the
// reticle (+0x104/+0x108) tracks the mouse cursor but seemingly not the gamepad one.
// This logs EVERY f32 in a window on the cursor object AND the WorldMapArea (view) that
// changed since last tick, with its offset. Recipe: open the map, move ONLY the mouse →
// note the offsets logged; then move ONLY the gamepad stick → note the offsets. The field
// the projection must read is one that changes under BOTH (prime suspect: view pan +0x378).
// If NOTHING on this cursor/view changes under the stick, the gamepad drives a different
// object → bring back a bounded all-instance scan. Read-only + SEH-guarded.
void input_delta_scan(uintptr_t cursor, uintptr_t view)
{
    static std::unordered_map<uintptr_t, float> last;
    auto scan = [&](const char *tag, uintptr_t base_obj, int lo, int hi) {
        for (int off = lo; off <= hi; off += 4)
        {
            float val = 0.f;
            if (!seh_read4(reinterpret_cast<void *>(base_obj + off), &val)) continue;
            if (!std::isfinite(val)) continue;
            uintptr_t key = base_obj + off;
            auto it = last.find(key);
            if (it != last.end() && std::fabs(it->second - val) > 0.01f)
                g_log->info("[INPUT-DELTA] {}+{:#05x}: {:.3f} -> {:.3f}", tag, off, it->second, val);
            last[key] = val;
        }
    };
    // cursor coord cluster (reticle/snap fields) + the view's pan/zoom/rect cluster.
    scan("cursor", cursor, 0xE0, 0x160);
    if (view) scan("view", view, 0x340, 0x390);
}

// ── Enumerate ALL world-map cursors reachable from CSMenuMan (find the GAMEPAD one) ──
// Under gamepad the menu-walk (mouse) cursor's reticle is FROZEN while pan moves. If the
// gamepad drives a DIFFERENT WorldMapDialog/cursor than the menu-walk one, it is also
// reachable from CSMenuMan. This is a BOUNDED, SAFE enumeration — only the CSMenuMan
// window (L1 flat + L2 one-deref, exactly like resolve_cursor_via_menu), all via RPM, no
// raw deref. (Replaces an earlier whole-RAM scan that clang-cl miscompiled into an
// out-of-bounds read → crash.) Collects every dialog whose +0x2DB0 == cursor vtable.
std::vector<uintptr_t> g_all_cursors;

void enumerate_menu_cursors(uintptr_t base, uintptr_t vtable_va)
{
    g_all_cursors.clear();
    uint64_t mm = 0;
    if (!seh_read8(reinterpret_cast<void *>(base + CSMENUMAN_SLOT_RVA), &mm) || !plausible_ptr(mm))
        return;
    auto vt_is_cursor = [&](uint64_t obj) {
        uint64_t vt = 0;
        return seh_read8(reinterpret_cast<void *>(obj + CURSOR_OFF_IN_MENU), &vt) && vt == vtable_va;
    };
    auto add = [&](uint64_t dlg) {
        uintptr_t cur = dlg + CURSOR_OFF_IN_MENU;
        for (uintptr_t e : g_all_cursors)
            if (e == cur) return;
        g_all_cursors.push_back(cur);
    };
    for (uintptr_t o1 = 0; o1 < MENU_WALK_WINDOW; o1 += 8)
    {
        uint64_t p = 0;
        if (!seh_read8(reinterpret_cast<void *>(mm + o1), &p) || !plausible_ptr(p))
            continue;
        if (vt_is_cursor(p)) { add(p); continue; }       // L1: dialog is a flat field
        for (uintptr_t o2 = 0; o2 < 0x800; o2 += 8)        // L2: one deref deeper
        {
            uint64_t q = 0;
            if (!seh_read8(reinterpret_cast<void *>(p + o2), &q) || !plausible_ptr(q))
                continue;
            if (vt_is_cursor(q)) add(q);
        }
    }
    g_log->info("[ALLCURSOR] found {} world-map cursor(s) in CSMenuMan window. Move the GAMEPAD "
                "stick — the one whose +0xFC/+0x104 changes (not tagged MOUSE) is the gamepad cursor.",
                g_all_cursors.size());
    for (uintptr_t c : g_all_cursors)
        g_log->info("[ALLCURSOR]   cursor @{:#x} (dialog @{:#x})", c, c - CURSOR_OFF_IN_MENU);
}

// ── Live world→map-space projection (call the engine's own fn) ──────────────────────
// FUN_1408877d0(VM, float out[2], u32* packedId, float world[3]) -> char (1 on match).
// Loops the VM converters, folds WorldMapLegacyConvParam, applies the per-page affine —
// i.e. projects ANY (area,grid,pos) exactly like the native map (RE findings §4). This is
// the replacement for our baked LEGACY_CONV + -7040/+16512 affine + DLC eyeball.
using ProjLoopFn = char (*)(void *, float *, uint32_t *, float *);
ProjLoopFn resolve_proj_loop()
{
    static ProjLoopFn fn = nullptr;
    static bool tried = false;
    if (!tried)
    {
        tried = true;
        try
        {
            fn = modutils::scan<char(void *, float *, uint32_t *, float *)>(
                {.aob = goblin::sig::WORLDMAP_PROJ_LOOP});
        }
        catch (...) { fn = nullptr; }
        g_log->info("[PROJ] FUN_1408877d0 (loop wrapper) resolve -> {:p}", (void *)fn);
    }
    return fn;
}
// Call the engine projection. packedId = (area<<24)|(gridX<<16)|(gridZ<<8); world =
// {posX, posY, posZ} AREA-LOCAL (NOT gridX*256+pos — the fn reconstructs the tile). Returns
// false on no-match (e.g. a no-conv area like m19 Chapel the game doesn't place).
bool project_live(void *vm, int area, int gx, int gz, float px, float pz, float &u, float &v)
{
    ProjLoopFn fn = resolve_proj_loop();
    if (!fn || !vm)
        return false;
    uint32_t packed = ((uint32_t)(area & 0xff) << 24) | ((uint32_t)(gx & 0xff) << 16) |
                      ((uint32_t)(gz & 0xff) << 8);
    float world[3] = {px, 0.0f, pz};
    float out[2] = {0.0f, 0.0f};
    char ok = fn(vm, out, &packed, world);
    if (!ok)
        return false;
    u = out[0];
    v = out[1];
    return true;
}

// FUN_140876140(converter, float out[2], u32* packedId, float world[3]) -> char(1 on match).
// Projects ONE point through ONE converter; folds LegacyConv (mutates *packedId to the dst
// area) then checks area match. We loop it per slot to find the matched slot → page.
using ProjPointFn = char (*)(void *, float *, uint32_t *, float *);
ProjPointFn resolve_proj_point()
{
    static ProjPointFn fn = nullptr;
    static bool tried = false;
    if (!tried)
    {
        tried = true;
        try
        {
            fn = modutils::scan<char(void *, float *, uint32_t *, float *)>(
                {.aob = goblin::sig::WORLDMAP_PROJ_POINT});
        }
        catch (...) { fn = nullptr; }
        g_log->info("[PAGE] FUN_140876140 (per-converter) resolve -> {:p}", (void *)fn);
    }
    return fn;
}

// Live PAGE for a point: loop the VM converters with FUN_140876140; the first slot that
// matches → page = page-table[slot] (base+0x2ad82f8 = [00 01 0a] = {overworld,underground,
// DLC}), with the area==12 ⇒ underground override (findings §5). out_* report the matched
// slot + post-fold area for the A/B. Returns the raw page id (0/1/10), -1 = not placed.
int project_page(uintptr_t vm, int area, int gx, int gz, float px, float pz, int &out_slot,
                 int &out_folded_area)
{
    out_slot = -1;
    out_folded_area = -1;
    ProjPointFn fn = resolve_proj_point();
    if (!fn || !vm)
        return -2;
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    uint64_t count = 0;
    seh_read8(reinterpret_cast<void *>(vm + 0x280), &count);
    uint64_t n = count > 8 ? 8 : count;
    int page_tab = 0;
    seh_read_i32(reinterpret_cast<void *>(base + 0x2ad82f8), &page_tab); // bytes [00 01 0a ..]
    for (uint64_t i = 0; i < n; ++i)
    {
        uintptr_t conv = vm + 0xF8 + i * 0x30;
        uint32_t packed = ((uint32_t)(area & 0xff) << 24) | ((uint32_t)(gx & 0xff) << 16) |
                          ((uint32_t)(gz & 0xff) << 8);
        float world[3] = {px, 0.0f, pz};
        float out[2] = {0.0f, 0.0f};
        if (fn(reinterpret_cast<void *>(conv), out, &packed, world))
        {
            out_slot = (int)i;
            out_folded_area = (int)((packed >> 24) & 0xff);
            int pg = (i < 3) ? ((page_tab >> (i * 8)) & 0xff) : -1;
            if (pg == 0 && area == 12) // base-underground override (shares the OW converter)
                pg = 1;
            return pg;
        }
    }
    return -1; // no converter accepts it (e.g. m19 Chapel) → game doesn't place it
}

// Find the live CS::WorldMapViewModel (cached). Resolves from the active map cursor →
// WorldMapDialog (cursor-0x2DB0) → scan its pointer fields for an object whose first qword
// is the VM vtable. Re-validates the cached ptr cheaply; re-scans only when it goes stale
// (map reopened / freed). Returns 0 if the map is closed / not found.
uintptr_t find_view_model()
{
    static uintptr_t s_vm = 0;
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!base)
        return 0;
    const uintptr_t vm_vtable = base + goblin::sig::WORLDMAP_VIEWMODEL_VTABLE_RVA;
    if (s_vm)
    {
        uint64_t vt = 0;
        if (seh_read8(reinterpret_cast<void *>(s_vm), &vt) && vt == vm_vtable)
            return s_vm;
        s_vm = 0;
    }
    uintptr_t cursor = g_active_cursor.load(std::memory_order_relaxed);
    if (!cursor)
        return 0;
    uintptr_t dialog = cursor - CURSOR_OFF_IN_MENU;
    for (uintptr_t o = 0; o < 0x8000; o += 8)
    {
        uint64_t p = 0;
        if (!seh_read8(reinterpret_cast<void *>(dialog + o), &p) || !plausible_ptr(p))
            continue;
        uint64_t vt = 0;
        if (seh_read8(reinterpret_cast<void *>(p), &vt) && vt == vm_vtable) { s_vm = p; return p; }
    }
    return 0;
}

// Dev one-shot (config dump_converters): find the live CS::WorldMapViewModel and dump its
// converter array — the engine's own world->map-space projection table (RE:
// docs/re/windows_world_to_mapspace_projection_re_findings.md §1/§2). The VM is found by
// scanning the WorldMapDialog's pointer fields for an object whose first qword == the VM
// vtable (RVA WORLDMAP_VIEWMODEL_VTABLE_RVA). Logs [CONV] once per DISTINCT array content,
// so opening overworld → base-UG (m12) → DLC each logs its page's converters (incl. the
// never-solved DLC slot). STRICTLY READ-ONLY (ReadProcessMemory). cursor = dialog+0x2DB0.
void dump_converters(uintptr_t base, uintptr_t cursor)
{
    using clock = std::chrono::steady_clock;
    static clock::time_point s_last{};
    auto now = clock::now();
    if (now != clock::time_point{} && now - s_last < std::chrono::milliseconds(1500))
        return;
    s_last = now;

    const uintptr_t vm_vtable = base + goblin::sig::WORLDMAP_VIEWMODEL_VTABLE_RVA;
    const uintptr_t dialog = cursor - CURSOR_OFF_IN_MENU;
    uintptr_t vm = 0;
    for (uintptr_t o = 0; o < 0x8000; o += 8)
    {
        uint64_t p = 0;
        if (!seh_read8(reinterpret_cast<void *>(dialog + o), &p) || !plausible_ptr(p))
            continue;
        uint64_t vt = 0;
        if (seh_read8(reinterpret_cast<void *>(p), &vt) && vt == vm_vtable) { vm = p; break; }
    }
    static int s_misses = 0;
    if (!vm)
    {
        if (s_misses++ < 6)
            g_log->info("[CONV] WorldMapViewModel (vtable {:#x}) NOT in WorldMapDialog {:#x} "
                        "+0..0x8000 — VM lives elsewhere; try a wider/other root", vm_vtable, dialog);
        return;
    }

    uint64_t count = 0;
    seh_read8(reinterpret_cast<void *>(vm + 0x280), &count);
    uint64_t n = count > 8 ? 8 : count;

    g_log->info("[CONV] VM={:#x} count={} (dialog={:#x}, vtable RVA {:#x})",
                vm, count, dialog, goblin::sig::WORLDMAP_VIEWMODEL_VTABLE_RVA);
    for (uint64_t i = 0; i < n; ++i)
    {
        uintptr_t c = vm + 0xF8 + i * 0x30;
        int key = 0;
        float ox = 0, oz = 0, bx = 0, bz = 0, sc = 0;
        uint64_t node = 0;
        seh_read_i32(reinterpret_cast<void *>(c + 0x08), &key);
        seh_read4(reinterpret_cast<void *>(c + 0x0C), &ox);
        seh_read4(reinterpret_cast<void *>(c + 0x14), &oz);
        seh_read4(reinterpret_cast<void *>(c + 0x18), &bx);
        seh_read4(reinterpret_cast<void *>(c + 0x1C), &bz);
        seh_read4(reinterpret_cast<void *>(c + 0x20), &sc);
        seh_read8(reinterpret_cast<void *>(c + 0x28), &node);
        unsigned area = ((unsigned)key >> 24) & 0xff, gxb = ((unsigned)key >> 16) & 0xff,
                 gzb = ((unsigned)key >> 8) & 0xff;
        g_log->info("[CONV]   slot{} area={} gridXbase={} gridZbase={} origin=({:.1f},{:.1f}) "
                    "bias=({:.1f},{:.1f}) scale={:.4f} legacyConv={:#x}{}",
                    i, area, gxb, gzb, ox, oz, bx, bz, sc, node, node ? " [folds legacy]" : "");
    }
    // Sanity vs the baked affine: overworld slot should give originX-biasX=7040, originZ+biasZ=16512.
    g_log->info("[CONV] (expect an overworld slot: scale 1.0, origin 7168/16384, bias 128/128 "
                "=> our baked -7040/+16512)");

    // PROJ A/B: project a few known points LIVE (FUN_1408877d0) vs the baked overworld affine,
    // to validate the callable path (correct ABI + matches) BEFORE wiring it everywhere. A
    // dungeon/UG sample (legacy-fold) won't match baked — that's the point: the live fold is
    // the proper projection our baked LEGACY_CONV only approximates.
    struct Sample { int a, gx, gz; float px, pz; const char *what; } samples[] = {
        {60, 40, 40, 100.0f, 100.0f, "overworld (should == baked)"},
        {60, 28, 64, 0.0f, 0.0f, "overworld origin tile"},
        {61, 40, 40, 100.0f, 100.0f, "DLC overworld"},
        {12, 1, 0, 50.0f, 50.0f, "underground m12 (legacy-fold)"},
    };
    for (const Sample &s : samples)
    {
        float lu = 0, lv = 0;
        bool ok = project_live(reinterpret_cast<void *>(vm), s.a, s.gx, s.gz, s.px, s.pz, lu, lv);
        float wx = s.gx * 256.0f + s.px, wz = s.gz * 256.0f + s.pz;
        float bu = wx - 7040.0f, bv = -wz + 16512.0f;
        g_log->info("[PROJTEST] {} | area{} grid({},{}) pos({:.0f},{:.0f}) -> live=({:.1f},{:.1f}) "
                    "ok={} | baked-OW=({:.1f},{:.1f}) dU={:.1f} dV={:.1f}",
                    s.what, s.a, s.gx, s.gz, s.px, s.pz, lu, lv, ok, bu, bv, lu - bu, lv - bv);
    }

    // PAGE A/B: live page (loop FUN_140876140 → page-table) vs the baked marker_group_from
    // (which needs the fold's projected area). Goal: confirm the live page gives enough to
    // drop marker_group_from. Samples cover OW(60) / base-UG(12) / DLC-OW(61) / DLC-UG(40-43).
    struct PSample { int a, gx, gz; float px, pz; const char *what; } psamples[] = {
        {60, 40, 40, 100.0f, 100.0f, "overworld"},
        {12, 1, 0, 50.0f, 50.0f, "base underground"},
        {61, 40, 40, 100.0f, 100.0f, "DLC overworld"},
        {40, 1, 0, 50.0f, 50.0f, "DLC underground m40"},
        {19, 0, 0, 0.0f, 0.0f, "m19 Chapel (no-conv)"},
    };
    for (const PSample &s : psamples)
    {
        int slot = -1, folded = -1;
        int live_pg = project_page(vm, s.a, s.gx, s.gz, s.px, s.pz, slot, folded);
        int ga = 0; float wx = 0, wz = 0;
        goblin::marker_world_pos((uint8_t)s.a, (uint8_t)s.gx, (uint8_t)s.gz, s.px, s.pz, ga, wx, wz,
                                 /*conv_underground=*/true);
        int baked_grp = goblin::marker_group_from((uint8_t)s.a, ga);
        g_log->info("[PAGETEST] {} | area{} -> LIVE page={} slot={} foldedArea={} | "
                    "BAKED group={} (projArea={})",
                    s.what, s.a, live_pg, slot, folded, baked_grp, ga);
    }
}

// Dev one-shot (config dump_native_pins): walk the native-pin icon manager's std::map and
// log what pins the engine built — RE windows_suppress_native_pins_runtime_re_findings.md.
// mgr = [er+ICON_MGR_SLOT_RVA]; MSVC std::map _Tree @ mgr+0x390: _Myhead@+0x398 (sentinel
// ptr, non-null even empty), _Mysize@+0x3A0 (count). Node: Left@+0, Parent@+8, Right@+0x10,
// isNil@+0x19, key(int)@+0x20, value(CSWorldMapPointIns*)@+0x28. We DFS via Left/Right and
// stop at nil nodes. STRICTLY READ-ONLY (ReadProcessMemory via seh_read*). Throttled +
// change-detected (logs once per distinct size) so it doesn't spam per tick.
void dump_native_pin_map(uintptr_t base, uintptr_t mgr_slot, const char *tag)
{
    uint64_t mgr = 0;
    if (!seh_read8(reinterpret_cast<void *>(base + mgr_slot), &mgr) || !plausible_ptr(mgr))
    {
        g_log->info("[PINS] {} mgr slot {:#x} -> {:#x} (not resolved)", tag, base + mgr_slot, mgr);
        return;
    }
    uint64_t size = 0, head = 0;
    seh_read8(reinterpret_cast<void *>(mgr + 0x3A0), &size); // _Mysize
    seh_read8(reinterpret_cast<void *>(mgr + 0x398), &head); // _Myhead (sentinel)
    g_log->info("[PINS] {} mgr={:#x} size={} head={:#x}", tag, mgr, size, head);
    if (!plausible_ptr(head) || size == 0 || size > 100000)
        return;

    uint64_t root = 0;
    seh_read8(reinterpret_cast<void *>(head + 0x8), &root); // _Myhead->Parent = root
    if (!plausible_ptr(root))
        return;

    // Bounded DFS (cap nodes hard; log first LOG_CAP).
    constexpr int NODE_CAP = 8192;
    constexpr int LOG_CAP = 120;
    std::vector<uint64_t> stack;
    stack.push_back(root);
    int visited = 0, logged = 0;
    bool dumped_window = false;
    while (!stack.empty() && visited < NODE_CAP)
    {
        uint64_t node = stack.back();
        stack.pop_back();
        if (!plausible_ptr(node) || node == head)
            continue;
        uint8_t nil = 1;
        { uint64_t b = 0; if (seh_read8(reinterpret_cast<void *>(node + 0x18), &b)) nil = (b >> 8) & 0xff; }
        if (nil) // isNil byte @ +0x19 set → sentinel
            continue;
        ++visited;

        int key = 0;
        uint64_t ins = 0;
        seh_read_i32(reinterpret_cast<void *>(node + 0x20), &key);
        seh_read8(reinterpret_cast<void *>(node + 0x28), &ins);
        int ins_id = -1;
        if (plausible_ptr(ins))
            seh_read_i32(reinterpret_cast<void *>(ins + 0x30), &ins_id); // ins+0x30 = id (findings §2)
        if (logged < LOG_CAP)
        {
            g_log->info("[PINS]   {} key={} ins={:#x} ins+0x30={}", tag, key, ins, ins_id);
            ++logged;
        }
        // First valid ins → dump a field window to help locate iconId/type for a future
        // SELECTIVE filter (grace vs category). One-shot.
        if (!dumped_window && plausible_ptr(ins))
        {
            std::string hex;
            for (int o = 0x28; o <= 0x80; o += 4)
            {
                int v = 0;
                seh_read_i32(reinterpret_cast<void *>(ins + o), &v);
                char buf[32];
                std::snprintf(buf, sizeof(buf), "+%02x=%d ", o, v);
                hex += buf;
            }
            g_log->info("[PINS]   {} ins {:#x} window: {}", tag, ins, hex);
            dumped_window = true;
        }

        uint64_t l = 0, r = 0;
        seh_read8(reinterpret_cast<void *>(node + 0x00), &l); // Left
        seh_read8(reinterpret_cast<void *>(node + 0x10), &r); // Right
        if (plausible_ptr(l) && l != head) stack.push_back(l);
        if (plausible_ptr(r) && r != head) stack.push_back(r);
    }
    g_log->info("[PINS] {} walked {} nodes (logged {}{})", tag, visited, logged,
                visited >= NODE_CAP ? ", CAPPED" : "");
}

void dump_native_pins(uintptr_t base)
{
    using clock = std::chrono::steady_clock;
    static clock::time_point s_last{};
    static uint64_t s_last_size = ~0ull;
    static bool s_saw_nonempty = false; // once we log a non-empty PRIMARY, switch to change-detect
    static int s_force_attempts = 0;    // cap the "dump until non-empty" spam (~6s of map-open)
    auto now = clock::now();
    // Sample often (a map session can be brief + pins build a few hundred ms after open).
    if (now != clock::time_point{} && now - s_last < std::chrono::milliseconds(400))
        return;
    s_last = now;

    // Read the primary manager size. Until we've seen it NON-empty at least once, dump every
    // tick (the first sample is often before the pins build → an early 0 must not lock us out).
    // After the first non-empty dump, change-detect on size so we don't spam an idle map.
    uint64_t mgr = 0, size = 0;
    if (seh_read8(reinterpret_cast<void *>(base + goblin::sig::ICON_MGR_SLOT_RVA), &mgr) &&
        plausible_ptr(mgr))
        seh_read8(reinterpret_cast<void *>(mgr + 0x3A0), &size);
    bool force = !s_saw_nonempty && s_force_attempts < 15; // keep probing early-empty
    if (!force && size == s_last_size)
        return;
    if (force) ++s_force_attempts;
    s_last_size = size;
    if (size > 0 && size < 100000)
        s_saw_nonempty = true;

    dump_native_pin_map(base, goblin::sig::ICON_MGR_SLOT_RVA, "PRIMARY[er+0x3D6E9B0]");
    dump_native_pin_map(base, goblin::sig::ICON_MGR_SIBLING_SLOT_RVA, "SIBLING[er+0x3D6F558]");
}

void probe_loop()
{
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!base)
    {
        g_log->error("eldenring.exe base not found — probe aborted");
        return;
    }
    uintptr_t vtable_va = base + CURSOR_VTABLE_RVA;
    g_log->info("probe start: eldenring.exe base={:#x}, cursor vtable={:#x} (RVA {:#x})",
                base, vtable_va, CURSOR_VTABLE_RVA);

    // Several objects carry the WorldMapCursorControl vtable; only the ACTIVE map
    // cursor's coords change as you move. So track ALL instances and log each one's
    // coords on change (with its address) — the changing one is the real cursor.
    using clock = std::chrono::steady_clock;
    std::vector<uintptr_t> candidates;
    std::unordered_map<uintptr_t, std::array<float, 3>> last;
    std::unordered_map<uintptr_t, std::array<float, 3>> last_view; // panx/panz/zoom per cursor
    std::unordered_map<uintptr_t, char> bounds_dumped; // one-shot bounds dump per active cursor
    clock::time_point last_scan{};
    int empty_logs = 0;

    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto now = clock::now();

        // Proactive icon harvest, relocated off the game-thread find hook (docs/rpm_walk_audit.md).
        // Self-throttled + read-only RPM on our own process → safe on this background thread; keeps
        // Wine's per-RPM walk cost off the engine cadence. No-op unless an icon config is on.
        goblin::background_harvest_tick();

        // Drop a dead active cursor so the sticky publish can re-lock a fresh one.
        if (uintptr_t ac = g_active_cursor.load(std::memory_order_relaxed))
        {
            uint64_t vt;
            if (!seh_read8(reinterpret_cast<void *>(ac), &vt) || vt != vtable_va)
                g_active_cursor.store(0, std::memory_order_relaxed);
        }

        // PRIMARY: the menu walk (CSMenuMan → WorldMapDialog → +0x2DB0 = cursor) is cheap
        // (O(KB)) AND is itself the map-open detector — the dialog only exists while the
        // map is up. Run it EVERY tick, independent of the 0xCD gate (which oscillates
        // 3/7/0 and starved the probe). If it finds the canonical cursor, the map is open.
        uintptr_t menu_cursor = resolve_cursor_via_menu(base, vtable_va);
        if (menu_cursor)
        {
            if (candidates.size() != 1 || candidates[0] != menu_cursor)
            {
                candidates.clear();
                candidates.push_back(menu_cursor);
                g_log->info("resolve: cursor {:#x} (menu walk)", menu_cursor);
            }
            // RE check: dump the live WorldMapViewModel converter array (world->map-space
            // projection) when requested. Read-only; throttled + change-detected internally.
            // Dev dumps run on THIS 100ms probe thread and are RPM-heavy (std::map /
            // converter-array walks) → under Wine each RPM is a wineserver round-trip,
            // and the single-threaded wineserver means a storm here stalls the render
            // thread too (= phantom map lag with no [BENCH] line). Wrap them so a stray
            // dump_* flag left ON shows up as debug.dump_* in the session report.
            if (goblin::config::dumpConverters)
            {
                GOBLIN_BENCH_QUIET("debug.dump_converters");
                dump_converters(base, menu_cursor);
            }
            // RE check: walk the native-pin icon manager (CSWorldMapPointMan +0x398) to
            // see what pins the engine builds → decide native-pin suppression. Read-only.
            if (goblin::config::dumpNativePins)
            {
                GOBLIN_BENCH_QUIET("debug.dump_native_pins");
                dump_native_pins(base);
            }
            // Path A verify: the map is open here → re-read the registered icon images'
            // lazily-bound GPU textures (img+0x10 → GXTexture2D → ID3D12Resource). Once.
            if (goblin::config::dumpIconTextures)
            {
                GOBLIN_BENCH_QUIET("debug.dump_icon_textures");
                goblin::dump_icon_textures_live();
            }
            // DISABLED (gamepad-cursor WIP): the all-instance enumerate_menu_cursors scan
            // (L1 0x10000 × L2 0x800 RPM reads) ran once per map-open and slowed the map
            // load noticeably — and it never reliably found the gamepad cursor. Removed; the
            // single menu-walk cursor is used. Re-enable only behind a debug flag if needed.
        }
        else
        {
            // Menu walk found no cursor → the map is closed (no WorldMapDialog) or the
            // dialog offset is outside the walk window. NO whole-RAM scan anymore (it
            // crashed at startup + is O(GB)). Drop the active cursor; the walk re-resolves
            // the instant the map opens.
            if (g_active_cursor.load(std::memory_order_relaxed))
                g_active_cursor.store(0, std::memory_order_relaxed);
            candidates.clear();
            last.clear();
            last_view.clear();
            g_all_cursors.clear(); // re-enumerate next map-open
            continue;
        }

        for (uintptr_t a : candidates)
        {
            uint64_t v;
            if (!seh_read8(reinterpret_cast<void *>(a), &v) || v != vtable_va)
                continue; // instance died (menu closed)
            float x, z, y;
            if (!seh_read4(reinterpret_cast<void *>(a + OFF_X), &x) ||
                !seh_read4(reinterpret_cast<void *>(a + OFF_Z), &z) ||
                !seh_read4(reinterpret_cast<void *>(a + OFF_Y), &y))
                continue;
            // Many objects carry the vtable but are stale/uninitialised (junk:
            // 8e34, NaN, all-zero). The real map cursor has finite, plausible,
            // nonzero coords (the confirmed run was ~3888..6762). Filter to those.
            auto sane = [](float v) { return std::isfinite(v) && std::fabs(v) < 1e5f; };
            if (!sane(x) || !sane(z) || !sane(y) || (x == 0.f && z == 0.f))
                continue;
            auto &l = last[a];
            bool coord_moved = (x != l[0] || z != l[1] || y != l[2]);
            if (coord_moved) // reticle moved → this is (likely) the active cursor
            {
                g_log->info("cursor @{:#x} (menu @{:#x}): +0xFC={:.2f}  +0x104={:.2f}  +0x10C={:.2f}",
                            a, a - CURSOR_OFF_IN_MENU, x, z, y);
                l = {x, z, y};
            }

            // PROJECTION: follow cursor+0xF0 → WorldMapArea and read the LIVE viewport
            // (pan +0x378, zoom +0x380) + the static full-map rect EVERY tick. Log when
            // the reticle moved OR pan/zoom changed — ZOOM centers on the reticle (coord
            // unchanged) and edge-scroll PAN clamps the reticle at the edge (coord frozen
            // while pan sweeps), so gating on coord-move alone misses both. (doc §6)
            // PAN → pan sweeps; ZOOM → zoom changes; +0x350 rect stays [0,0,10496,10496].
            uint64_t view = 0;
            if (seh_read8(reinterpret_cast<void *>(a + OFF_VIEW_PTR), &view) && view &&
                plausible_ptr(view) && !view_is_static(view))
            {
                float panx, panz, zoom, r0, r1, r2, r3;
                if (seh_read4(reinterpret_cast<void *>(view + VIEW_PAN_X), &panx) &&
                    seh_read4(reinterpret_cast<void *>(view + VIEW_PAN_Z), &panz) &&
                    seh_read4(reinterpret_cast<void *>(view + VIEW_ZOOM), &zoom) &&
                    seh_read4(reinterpret_cast<void *>(view + 0x350), &r0) &&
                    seh_read4(reinterpret_cast<void *>(view + 0x354), &r1) &&
                    seh_read4(reinterpret_cast<void *>(view + 0x358), &r2) &&
                    seh_read4(reinterpret_cast<void *>(view + 0x35c), &r3))
                {
                    // Publish this cursor as the active one (valid view + zoom) for
                    // the overlay-markers prototype's get_live_view(). DIAG: log the
                    // 0→addr (and address-change) transition so the first-open latency
                    // can be measured against the map-open moment.
                    // STICKY: several mirror cursor instances carry a valid view+zoom;
                    // publishing "the last valid candidate" each scan made the active
                    // cursor SWITCH mid-session → its view centre jumped → the markers
                    // TELEPORTED (and the eyeball pan shifted per session). Lock onto the
                    // first valid one and keep it until it dies (active_dead clears it).
                    if (zoom != 0.f && !view_is_static(view))
                    {
                        uintptr_t cur = g_active_cursor.load(std::memory_order_relaxed);
                        if (cur == 0 || cur == a)
                        {
                            if (cur != a)
                                g_log->info("[PUBLISH] active cursor @{:#x} (view @{:#x}) zoom={:.4f} "
                                            "coord=({:.1f},{:.1f})", a, view, zoom, x, z);
                            g_active_cursor.store(a, std::memory_order_relaxed);
                        }
                    }

                    auto &lv = last_view[a];
                    bool view_changed = (panx != lv[0] || panz != lv[1] || zoom != lv[2]);
                    if (coord_moved || view_changed)
                    {
                        g_log->info("  VIEW @{:#x}: pan(+0x378)=({:.2f},{:.2f})  zoom(+0x380)={:.4f}"
                                    "  fullRect(+0x350)=[{:.1f},{:.1f},{:.1f},{:.1f}]",
                                    view, panx, panz, zoom, r0, r1, r2, r3);
                        lv = {panx, panz, zoom};
                    }
                    region_diag(a, view); // find the open-map region field (switch maps → flips)
                    input_delta_scan(a, view); // which f32 moves under mouse vs gamepad stick?
                }

                // One-shot per active cursor: the full float window cursor+0xE0..+0x388
                // (covers both rects + pan + zoom) + the virtual canvas (1920×1080?).
                if ((x != 0.f || z != 0.f) && !bounds_dumped.count(a))
                {
                    bounds_dumped[a] = 1;
                    g_log->info("--- window dump @{:#x} (rects, pan@+0x378, zoom@+0x380) ---", a);
                    for (ptrdiff_t off = 0xE0; off <= 0x388; off += 4)
                    {
                        float fv;
                        if (seh_read4(reinterpret_cast<void *>(a + off), &fv))
                            g_log->info("  +{:#05x} = {:.3f}", off, fv);
                    }
                    if (view)
                        for (ptrdiff_t off = 0x340; off <= 0x388; off += 4)
                        {
                            float fv;
                            if (seh_read4(reinterpret_cast<void *>(view + off), &fv))
                                g_log->info("  VIEW+{:#05x} = {:.3f}", off, fv);
                        }
                    // Virtual UI canvas: expect width≈1920, height≈1080.
                    uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
                    uint64_t canvas = 0;
                    if (exe && seh_read8(reinterpret_cast<void *>(exe + CANVAS_SINGLETON_RVA), &canvas) &&
                        canvas)
                    {
                        uint64_t c2 = 0;
                        if (seh_read8(reinterpret_cast<void *>(canvas + 0x128), &c2) && c2)
                        {
                            float ox, oy, w, h;
                            if (seh_read4(reinterpret_cast<void *>(c2 + 0x110), &ox) &&
                                seh_read4(reinterpret_cast<void *>(c2 + 0x114), &oy) &&
                                seh_read4(reinterpret_cast<void *>(c2 + 0x118), &w) &&
                                seh_read4(reinterpret_cast<void *>(c2 + 0x11c), &h))
                                g_log->info("  CANVAS: origin=({:.1f},{:.1f})  size=({:.1f},{:.1f})", ox, oy, w, h);
                        }
                    }
                }
            }
        }
    }
}
} // namespace

uintptr_t debug_active_cursor() { return g_active_cursor.load(std::memory_order_relaxed); }

bool project(int area, int gridX, int gridZ, float posX, float posZ, float &mapU, float &mapV,
             int &page)
{
    page = -1;
    uintptr_t vm = find_view_model();
    if (!vm)
        return false;
    ProjPointFn fn = resolve_proj_point();
    if (!fn)
        return false;
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    uint64_t count = 0;
    seh_read8(reinterpret_cast<void *>(vm + 0x280), &count);
    uint64_t n = count > 8 ? 8 : count;
    int page_tab = 0;
    seh_read_i32(reinterpret_cast<void *>(base + 0x2ad82f8), &page_tab); // bytes [00 01 0a ..]
    // Loop the converters with FUN_140876140 (per-converter): one pass yields BOTH the UV
    // (out[]) and the matched slot → page. First match wins.
    for (uint64_t i = 0; i < n; ++i)
    {
        uintptr_t conv = vm + 0xF8 + i * 0x30;
        uint32_t packed = ((uint32_t)(area & 0xff) << 24) | ((uint32_t)(gridX & 0xff) << 16) |
                          ((uint32_t)(gridZ & 0xff) << 8);
        float world[3] = {posX, 0.0f, posZ};
        float out[2] = {0.0f, 0.0f};
        if (fn(reinterpret_cast<void *>(conv), out, &packed, world))
        {
            mapU = out[0];
            mapV = out[1];
            int pg = (i < 3) ? ((page_tab >> (i * 8)) & 0xff) : 0;
            if (pg == 0 && area == 12) // base-underground shares the overworld converter
                pg = 1;
            page = pg;
            return true;
        }
    }
    return false; // no converter accepts it
}

bool get_live_view(LiveView &out)
{
    uintptr_t a = g_active_cursor.load(std::memory_order_relaxed);
    if (!a)
        return false;
    static uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!base)
        return false;
    uint64_t vt = 0, view = 0;
    // Confirm the cached address is still a live cursor (menu may have closed).
    if (!seh_read8(reinterpret_cast<void *>(a), &vt) || vt != base + CURSOR_VTABLE_RVA)
        return false;
    if (!seh_read8(reinterpret_cast<void *>(a + OFF_VIEW_PTR), &view) || !view ||
        !plausible_ptr(view) || view_is_static(view))
        return false;
    if (!seh_read4(reinterpret_cast<void *>(a + OFF_X), &out.cursorX) ||
        !seh_read4(reinterpret_cast<void *>(a + OFF_Z), &out.cursorZ) ||
        !seh_read4(reinterpret_cast<void *>(view + VIEW_PAN_X), &out.panX) ||
        !seh_read4(reinterpret_cast<void *>(view + VIEW_PAN_Z), &out.panZ) ||
        !seh_read4(reinterpret_cast<void *>(view + VIEW_ZOOM), &out.zoom) || out.zoom == 0.f)
        return false;
    // Cursor/snap bounds rect on the view (view+0x340..0x34c) → cursor-independent centre.
    if (!seh_read4(reinterpret_cast<void *>(view + 0x340), &out.snapMinX) ||
        !seh_read4(reinterpret_cast<void *>(view + 0x344), &out.snapMinZ) ||
        !seh_read4(reinterpret_cast<void *>(view + 0x348), &out.snapMaxX) ||
        !seh_read4(reinterpret_cast<void *>(view + 0x34c), &out.snapMaxZ))
    {
        out.snapMinX = out.snapMinZ = out.snapMaxX = out.snapMaxZ = 0.f;
    }
    // diag window cursor+0xFC..+0x118 (find which offset drives the vertical axis)
    for (int i = 0; i < 8; ++i)
        if (!seh_read4(reinterpret_cast<void *>(a + 0xFC + i * 4), &out.raw[i]))
            out.raw[i] = 0.f;
    // Snap-rect midpoint (view +0x340 minX / +0x344 minZ / +0x348 maxX / +0x34c maxZ) →
    // device-independent view centre = (pan + snapMid)/zoom (NOT the reticle).
    out.snapMidX = out.snapMidZ = 0.f;
    {
        float sminx, sminz, smaxx, smaxz;
        if (seh_read4(reinterpret_cast<void *>(view + 0x340), &sminx) &&
            seh_read4(reinterpret_cast<void *>(view + 0x344), &sminz) &&
            seh_read4(reinterpret_cast<void *>(view + 0x348), &smaxx) &&
            seh_read4(reinterpret_cast<void *>(view + 0x34c), &smaxz))
        {
            out.snapMidX = (sminx + smaxx) * 0.5f;
            out.snapMidZ = (sminz + smaxz) * 0.5f;
        }
    }
    // WorldMapArea+0x6e (i32) = areaNo of the open page (doc §3) → page filter.
    out.viewArea = -1;
    seh_read_i32(reinterpret_cast<void *>(view + 0x6e), &out.viewArea);
    // Open-map region — SOLVED RE (commit 3f4ba42, docs/windows_open_map_region_re_prompt.md).
    // The dead DAT_143d6cfc3 flag (render-only/transient) is replaced by two O(1) reads off
    // the WorldMapDialog (= cursor − 0x2DB0):
    //   page  = *(int*)(dialog + 0xA88)                       0 = base, 10 = DLC
    //   layer = *(uint8_t*)( *(void**)(dialog + 0x2B68) + 0xB8 )  0 = surface, 1 = underground
    // (underground is applied internally as page+1, not stored in `page`.) Gate:
    //   page==10 → DLC ; else layer==0 → overworld ; else underground.
    out.underground = 0;
    out.openDlc = 0;
    uintptr_t dialog = a - CURSOR_OFF_IN_MENU;
    int page = 0;
    if (seh_read_i32(reinterpret_cast<void *>(dialog + 0xA88), &page))
        out.openDlc = (page == 10) ? 1 : 0;
    uint64_t layer_obj = 0;
    if (seh_read8(reinterpret_cast<void *>(dialog + 0x2B68), &layer_obj) && layer_obj)
    {
        int layer = 0;
        if (seh_read_i32(reinterpret_cast<void *>(layer_obj + 0xB8), &layer))
            out.underground = layer & 0xFF; // the layer state is a byte
    }
    // Page-transition state (RE §2/§5, runtime-CONFIRMED). fadeTimer (dialog+0xE00) resets to 0.2
    // at a swap and ramps to 0 over ~0.2s (sign=layer); the doc's pan/zoom-target offsets read
    // garbage and aren't needed (marker positions ride the live view, §4). swapEdge = the 1-frame
    // swap pulse.
    out.swapEdge = 0; out.fadeTimer = 0.f;
    {
        int edge = 0;
        if (seh_read_i32(reinterpret_cast<void *>(dialog + 0xA44), &edge))
            out.swapEdge = edge & 0xFF; // u8 (read as i32, mask)
        seh_read4(reinterpret_cast<void *>(dialog + 0xE00), &out.fadeTimer);
    }
    return true;
}

// Pan the live world map so its view CENTRES on a marker-space point (mU, mV) — the same space as
// project_marker's gU/gV and WorldMapPointParam posX/posZ. Inverts the engine pan setter
// (FUN_1409cd100): pan = viewCentre·zoom − snapMid (identical to goblin_projection ViewDelay's
// centre→pan). Writes WorldMapArea +0x378/+0x37C via WriteProcessMemory (bad/RO dst → false, never
// crashes). Only valid for a point on the CURRENTLY-OPEN page (cross-page needs a page switch first).
// Returns false if the map is closed / no live cursor / a read or write faulted.
static LocateDebug g_locate_dbg;
const LocateDebug &last_locate_debug() { return g_locate_dbg; }

bool set_view_center(float mU, float mV, float minZoom)
{
    // Reset the diagnostic snapshot for this call (the F1 "Locate debug" overlay reads it).
    g_locate_dbg = LocateDebug{};
    g_locate_dbg.ran = true;
    g_locate_dbg.reqU = mU;
    g_locate_dbg.reqV = mV;
    uintptr_t a = g_active_cursor.load(std::memory_order_relaxed);
    if (!a)
        return false;
    static uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!base)
        return false;
    uint64_t vt = 0, view = 0;
    if (!seh_read8(reinterpret_cast<void *>(a), &vt) || vt != base + CURSOR_VTABLE_RVA)
        return false;
    if (!seh_read8(reinterpret_cast<void *>(a + OFF_VIEW_PTR), &view) || !view ||
        !plausible_ptr(view) || view_is_static(view))
        return false;
    g_locate_dbg.cursorOk = true;
    float zoom = 0.f, sminx = 0, sminz = 0, smaxx = 0, smaxz = 0;
    if (!seh_read4(reinterpret_cast<void *>(view + VIEW_ZOOM), &zoom) || zoom == 0.f ||
        !seh_read4(reinterpret_cast<void *>(view + 0x340), &sminx) ||
        !seh_read4(reinterpret_cast<void *>(view + 0x344), &sminz) ||
        !seh_read4(reinterpret_cast<void *>(view + 0x348), &smaxx) ||
        !seh_read4(reinterpret_cast<void *>(view + 0x34c), &smaxz))
        return false;
    // Zoom IN if the live view is more zoomed-OUT than requested (a far-out view would leave the
    // located marker a tiny speck). Write the live zoom (+0x380) directly so it snaps; we never zoom
    // OUT (a user who zoomed in further keeps it). snapMid is the map-extent midpoint (zoom-independent),
    // so the pan below uses the post-zoom value and the centre stays exact even while the engine eases
    // — and the per-frame locate hold re-asserts it, self-correcting any one-frame ease lag.
    g_locate_dbg.zoomBefore = zoom;
    if (minZoom > 0.f && zoom < minZoom)
    {
        seh_write_f32(reinterpret_cast<void *>(view + VIEW_ZOOM), minZoom);
        zoom = minZoom;
    }
    g_locate_dbg.zoomUsed = zoom;
    const float snapMidX = (sminx + smaxx) * 0.5f;
    const float snapMidZ = (sminz + smaxz) * 0.5f;
    g_locate_dbg.snapMidX = snapMidX;
    g_locate_dbg.snapMidZ = snapMidZ;
    // Clamp the target view-CENTRE so the written pan keeps the view INSIDE the world. The engine
    // REVERTS a pan that would show void past the map edge, so a marker near the border (e.g. Godrick
    // at Stormveil — top-left of the map) otherwise never centres at all: the OOB pan is rejected and
    // the map doesn't move. Bound = the static full-map rect (+0x350 = [minX,minZ,maxX,maxZ]). The
    // visible half-extent in marker space is resolution-INDEPENDENT — the map renders in a fixed
    // 1920×1080 virtual canvas so the realW/1920 factor cancels → half = 960/zoom (X), 540/zoom (Z).
    // Inclusive (<=): when an axis has exactly one valid centre we clamp to it instead of snapping to
    // the midpoint; when the whole map fits (lo>hi, zoomed out) we centre on the map midpoint.
    float cU = mU, cV = mV;
    float wMinX = 0, wMinZ = 0, wMaxX = 0, wMaxZ = 0;
    if (seh_read4(reinterpret_cast<void *>(view + VIEW_FULLRECT + 0), &wMinX) &&
        seh_read4(reinterpret_cast<void *>(view + VIEW_FULLRECT + 4), &wMinZ) &&
        seh_read4(reinterpret_cast<void *>(view + VIEW_FULLRECT + 8), &wMaxX) &&
        seh_read4(reinterpret_cast<void *>(view + VIEW_FULLRECT + 12), &wMaxZ) &&
        wMaxX > wMinX && wMaxZ > wMinZ)
    {
        const float halfU = 960.f / zoom, halfV = 540.f / zoom;
        const float loU = wMinX + halfU, hiU = wMaxX - halfU;
        const float loV = wMinZ + halfV, hiV = wMaxZ - halfV;
        cU = (loU <= hiU) ? (cU < loU ? loU : (cU > hiU ? hiU : cU)) : (wMinX + wMaxX) * 0.5f;
        cV = (loV <= hiV) ? (cV < loV ? loV : (cV > hiV ? hiV : cV)) : (wMinZ + wMaxZ) * 0.5f;
        g_locate_dbg.rectOk = true;
        g_locate_dbg.wMinX = wMinX; g_locate_dbg.wMinZ = wMinZ;
        g_locate_dbg.wMaxX = wMaxX; g_locate_dbg.wMaxZ = wMaxZ;
    }
    g_locate_dbg.clampU = cU;
    g_locate_dbg.clampV = cV;
    g_locate_dbg.clamped = (cU != mU || cV != mV);
    // The "intended" pan = where the view SHOULD end up. We DON'T write it: the engine re-derives the
    // pan from the cursor every frame (runtime-confirmed via the Locate debug overlay — a raw pan write,
    // and a reticle +0xFC write, are both reverted). The actual centring is DRIVEN overlay-side by
    // feeding GetCursorPos the marker's projected screen offset (the engine's own pan, converges + works
    // at edges). This panX/panZ is kept only as the debug "target" the live pan should converge to.
    const float panX = cU * zoom - snapMidX;
    const float panZ = cV * zoom - snapMidZ;
    g_locate_dbg.panWroteX = panX;
    g_locate_dbg.panWroteZ = panZ;
    g_locate_dbg.wrote = true; // zoom written + target prepared (no pan write by design)
    return true;
}

// ── Page-switch instrumentation (config debug_page_switch) ───────────────────────────────────────
// Hook each world-map page/layer switch handler and log [PAGESW] when it fires: which fn, its 4
// register args (rcx/rdx/r8/r9 = dialog + up to 3 ints), and the page state (openDlc / layer)
// BEFORE→AFTER. Run it once (overworld→underground→overworld→DLC→overworld) to (a) pin which of the 6
// base↔DLC siblings does each direction, (b) confirm the layer/force arg values, and (c) feed the
// safe-trigger design (docs/re/windows_worldmap_page_switch_re_prompt.md). Read-only besides the hooks
// (calls the original, never alters args) — purely diagnostic, off by default.
namespace
{
struct PsHandler { uint32_t rva; const char *name; };
// RVAs from docs/re/windows_worldmap_page_transition_re_findings.md §6 (imagebase 0x140000000).
constexpr PsHandler PS_HANDLERS[] = {
    {0x9c40f0, "c40f0(layer surface<->UG)"},
    {0x9c5d20, "c5d20(map?)"}, {0x9c7900, "c7900(map?)"}, {0x9c1fc0, "c1fc0(map?)"},
    {0x9c23d0, "c23d0(map?)"}, {0x9c2c00, "c2c00(map?)"}, {0x9c3280, "c3280(map?)"},
    {0x9c8120, "c8120(page-apply)"},
};
constexpr int PS_COUNT = (int)(sizeof(PS_HANDLERS) / sizeof(PS_HANDLERS[0]));
using PsFn = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
PsFn g_ps_orig[PS_COUNT] = {};

// dialog = the handler's 1st arg (rcx). page id @ +0xA88, layer byte @ [+0x2B68]+0xB8.
inline void ps_read_page(uintptr_t dialog, int &openDlc, int &layer)
{
    openDlc = layer = -1;
    if (!dialog) return;
    seh_read_i32(reinterpret_cast<void *>(dialog + 0xA88), &openDlc);
    uint64_t sub = 0;
    if (seh_read8(reinterpret_cast<void *>(dialog + 0x2B68), &sub) && sub)
    {
        int b = -1;
        if (seh_read_i32(reinterpret_cast<void *>(sub + 0xB8), &b)) layer = b & 0xFF;
    }
}

template <int N>
uintptr_t ps_detour(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4)
{
    int bd = -1, bl = -1;
    ps_read_page(a1, bd, bl);
    uintptr_t r = g_ps_orig[N] ? g_ps_orig[N](a1, a2, a3, a4) : 0;
    int ad = -1, al = -1;
    ps_read_page(a1, ad, al);
    if (g_log)
        g_log->info("[PAGESW] {} dialog={:#x} a2={:#x} a3={:#x} a4={:#x} | page {}->{} layer {}->{}",
                    PS_HANDLERS[N].name, a1, a2, a3, a4, bd, ad, bl, al);
    return r;
}

template <int N>
int ps_try_hook(uintptr_t base)
{
    try
    {
        modutils::hook(reinterpret_cast<void *>(base + PS_HANDLERS[N].rva),
                       reinterpret_cast<void *>(&ps_detour<N>),
                       reinterpret_cast<void **>(&g_ps_orig[N]));
        return 1;
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[PAGESW] hook {} (+{:#x}) failed: {}", PS_HANDLERS[N].name,
                     PS_HANDLERS[N].rva, e.what());
        return 0;
    }
}

template <int... Is>
int ps_install_seq(uintptr_t base, std::integer_sequence<int, Is...>)
{
    int n = 0;
    (void)std::initializer_list<int>{(n += ps_try_hook<Is>(base), 0)...};
    return n;
}

void install_page_switch_probe()
{
    if (!goblin::config::debugPageSwitch) return;
    uintptr_t base = g_exe_base ? g_exe_base
                                : reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!base) { spdlog::warn("[PAGESW] no eldenring.exe base — probe not installed"); return; }
    int n = ps_install_seq(base, std::make_integer_sequence<int, PS_COUNT>{});
    try { modutils::enable_hooks(); } catch (const std::exception &) {}
    spdlog::info("[PAGESW] page-switch probe: {}/{} hooks installed (switch pages on the open map)", n,
                 PS_COUNT);
}

// ── Auto page-switch marshal ─────────────────────────────────────────────────────────────────────
// Switch the map page from the GAME thread, never the render thread. We hook the per-frame map step
// FUN_1409c32f0 (runs on the UI thread, receives the WorldMapDialog) and, inside it, drain a pending
// "switch to group G" request by calling the pinned switch handlers — exactly where the engine itself
// calls them. (Instrumentation pinned: c1fc0 = base↔DLC page arg2∈{0,10}; c7900 = surface↔UG layer
// arg2∈{0,1}; the observed r8/r9 = {0x40,1}. See windows_worldmap_page_switch_re_findings.md.)
// Page (c1fc0) first, then layer (c7900) — one axis per step so the engine settles between.
constexpr uint32_t RVA_C1FC0 = 0x9c1fc0;   // base↔DLC: (dialog, page 0/10, _, force)
constexpr uint32_t RVA_C7900 = 0x9c7900;   // surface↔UG: (dialog, layer 0/1, 0x40, force)
constexpr uint32_t RVA_C32F0 = 0x9c32f0;   // per-frame map step (game thread)
using SwitchFn = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
// FUN_1409c32f0(dialog, float dt, …): dt is a FLOAT (xmm1), so the detour MUST type it as float to
// preserve it across the forward call — a uintptr_t there would drop dt and break the UI step.
using StepFn = uintptr_t (*)(uintptr_t, float, uintptr_t, uintptr_t);
SwitchFn g_c1fc0 = nullptr, g_c7900 = nullptr;
StepFn g_c32f0_orig = nullptr;
std::atomic<int> g_pending_group{-1};  // target group (bit1=DLC, bit0=UG); -1 = none
// LOCATE DRIVE (structural fix): the per-frame map step c32f0 eases the pan toward the cursor RETICLE
// target (= cursor+0xFC, RE §5b). We write that reticle to the located marker INSIDE the c32f0 detour
// (game thread, just before the original's easer reads it) so the engine centres on it and our write
// isn't reverted by the cursor tick. Set by the overlay locate hold; cleared when it ends.
std::atomic<bool> g_locate_active{false};
std::atomic<float> g_locate_u{0.f}, g_locate_v{0.f}; // target marker-space centre
// Frames to keep "busy" after the last switch: a page swap SNAPS the view to the new page's default
// (page-transition findings §7b), which would CLOBBER an item-locate pan issued too early. The locate
// waits until this settles (page_switch_busy) so its set_view_center is the LAST write and sticks.
std::atomic<int> g_switch_settle{0};
constexpr int SWITCH_SETTLE_FRAMES = 20;  // ~0.33s @60fps > the ~0.2s page cross-fade/snap

// Switch both axes toward the target group: PAGE (overworld<->DLC, c1fc0) then LAYER (surface<->UG,
// c7900). Availability gating lives in the CALLER (the overlay only requests a group whose pages the
// player has visited — grace-based, robust); the marshal just executes, so e.g. an overworld item
// found while underground switches the layer back to surface (the "Godrick from underground" fix).
// TODO(page_og_underground_available): the per-page DIALOG availability flag is only half-solved — DLC
// = dialog+0x27c8 (read UNRELIABLY at runtime: greyed even on a discovered save), underground = never
// found (contextual). Abandoned in favour of grace-discovery in the overlay; revisit if a clean dialog
// flag is wanted. See docs/re/windows_worldmap_page_switch_re_findings.md §6.
uintptr_t hk_c32f0(uintptr_t dialog, float dt, uintptr_t a3, uintptr_t a4)
{
    // LOCATE DRIVE: write the cursor RETICLE target = the located marker BEFORE the original step, so
    // its easer pans the view onto our marker (RE §5b: c32f0 eases pan toward dialog+0x2EAC = cursor+0xFC).
    // The cursor tick rewrites the reticle from input each frame, but here — game thread, last write
    // before the easer — ours wins. All three reticle pairs (snap-clamped +0xFC, full-clamped +0x104,
    // snap-lerp +0x10C) are set so whichever the easer reads centres on the target. Re-asserted every
    // frame for the locate hold's duration (the nav jitter keeps this step running with F1 open).
    if (g_locate_active.load(std::memory_order_relaxed) && dialog)
    {
        const float u = g_locate_u.load(std::memory_order_relaxed);
        const float v = g_locate_v.load(std::memory_order_relaxed);
        const uintptr_t cur = dialog + CURSOR_OFF_IN_MENU;
        seh_write_f32(reinterpret_cast<void *>(cur + 0xFC), u);
        seh_write_f32(reinterpret_cast<void *>(cur + 0x100), v);
        seh_write_f32(reinterpret_cast<void *>(cur + 0x104), u);
        seh_write_f32(reinterpret_cast<void *>(cur + 0x108), v);
        seh_write_f32(reinterpret_cast<void *>(cur + 0x10C), u);
        seh_write_f32(reinterpret_cast<void *>(cur + 0x110), v);
    }
    uintptr_t r = g_c32f0_orig ? g_c32f0_orig(dialog, dt, a3, a4) : 0;
    const int want = g_pending_group.load(std::memory_order_relaxed);
    // GIVE-UP guard: if a requested switch never reaches its target (e.g. a handler that doesn't take
    // the expected direction), DON'T loop forever — that would pin page_switch_busy() true and block
    // EVERY locate pan. Bound the attempts; on timeout, drop the request so pans unblock.
    static int s_last_want = -2, s_age = 0;
    if (want != s_last_want) { s_last_want = want; s_age = 0; }
    if (want >= 0 && dialog)
    {
        int curPage = -1, curLayer = -1;
        seh_read_i32(reinterpret_cast<void *>(dialog + 0xA88), &curPage);
        uint64_t sub = 0;
        if (seh_read8(reinterpret_cast<void *>(dialog + 0x2B68), &sub) && sub)
        {
            int b = -1;
            if (seh_read_i32(reinterpret_cast<void *>(sub + 0xB8), &b)) curLayer = b & 0xFF;
        }
        const int wantPage = (want & 2) ? 10 : 0;
        const int wantLayer = (want & 1) ? 1 : 0;
        if (++s_age > 120)  // ~2s: the switch isn't landing → give up so we never block pans
        {
            g_pending_group.store(-1, std::memory_order_relaxed);
            g_switch_settle.store(0, std::memory_order_relaxed);
        }
        else if (curPage != wantPage && g_c1fc0)
        {
            g_c1fc0(dialog, (uintptr_t)wantPage, 0, 1);            // switch page (c1fc0 is reliable both ways)
            g_switch_settle.store(SWITCH_SETTLE_FRAMES, std::memory_order_relaxed);
        }
        else if (curLayer != wantLayer && sub)
        {
            // LAYER: there's no reliable handler for UG→surface (c7900 only does surface→UG; the
            // reverse goes through an unhooked path), so WRITE the layer byte directly — the per-frame
            // c32f0 step (which we just called) reads it and cross-fades. (quentin's idea: we already
            // have the flag, just use it.) [dialog+0x2B68]+0xB8 = 0 surface / 1 underground.
            seh_write_u8(reinterpret_cast<void *>(sub + 0xB8), (uint8_t)wantLayer);
            g_switch_settle.store(SWITCH_SETTLE_FRAMES, std::memory_order_relaxed);
        }
        else
        {
            g_pending_group.store(-1, std::memory_order_relaxed);  // reached target → settle, then pan
            g_switch_settle.store(SWITCH_SETTLE_FRAMES, std::memory_order_relaxed);
        }
    }
    int s = g_switch_settle.load(std::memory_order_relaxed);
    if (s > 0) g_switch_settle.store(s - 1, std::memory_order_relaxed);
    return r;
}

void install_auto_page_switch()
{
    uintptr_t base = g_exe_base ? g_exe_base
                                : reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!base) return;
    g_c1fc0 = reinterpret_cast<SwitchFn>(base + RVA_C1FC0);
    g_c7900 = reinterpret_cast<SwitchFn>(base + RVA_C7900);
    try
    {
        modutils::hook(reinterpret_cast<void *>(base + RVA_C32F0),
                       reinterpret_cast<void *>(&hk_c32f0),
                       reinterpret_cast<void **>(&g_c32f0_orig));
        modutils::enable_hooks();
        spdlog::info("[PAGESW] auto page-switch marshal installed (c32f0 game-thread hook)");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[PAGESW] auto page-switch install failed: {}", e.what());
    }
}
} // namespace

void request_switch_to_page(int group)
{
    g_pending_group.store(group, std::memory_order_relaxed);
}

// Drive the live map to CENTRE on a marker-space point (the item-search locate). The c32f0 detour
// writes this to the cursor reticle each game frame so the engine eases the pan onto it (works at
// world edges, not reverted — unlike a raw pan write). Call every frame during the locate hold;
// clear_locate_target() when done (else the map stays pinned to the marker).
void set_locate_target(float u, float v)
{
    g_locate_u.store(u, std::memory_order_relaxed);
    g_locate_v.store(v, std::memory_order_relaxed);
    g_locate_active.store(true, std::memory_order_relaxed);
}
void clear_locate_target() { g_locate_active.store(false, std::memory_order_relaxed); }

bool page_switch_busy()
{
    return g_pending_group.load(std::memory_order_relaxed) >= 0 ||
           g_switch_settle.load(std::memory_order_relaxed) > 0;
}

void initialize(const std::filesystem::path &log_path)
{
    try
    {
        g_log = spdlog::basic_logger_mt("mfg-wmprobe", log_path.string(), true);  // truncate; prev sessions archived
        g_log->set_pattern("[%H:%M:%S.%e] %v");
        g_log->flush_on(spdlog::level::info);
    }
    catch (const std::exception &e)
    {
        spdlog::error("[WM-PROBE] could not open log {}: {}", log_path.string(), e.what());
        return;
    }
    install_page_switch_probe();  // gated on config::debugPageSwitch
    install_auto_page_switch();   // game-thread marshal for request_switch_to_page (item-search locate)
    std::thread(probe_loop).detach();
    spdlog::info("[WM-PROBE] world-map cursor probe active → {}", log_path.string());
}

// Mid-session resolution diagnostic (RE docs/re/windows_midsession_resolution_swapchain_re_findings.md).
// Walks the render-output list at DAT_1447ef360+0x128..+0x130 (stride 0x170) and logs each
// entry's active float dims (+0x118/+0x11c) + dirty bit (+0x140) against the live backbuffer.
// Self-contained + RPM-guarded (no probe init needed); logs to the main MapForGoblins.log.
void dump_render_dims(float bbW, float bbH)
{
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!base) { spdlog::info("[RENDIMS] no eldenring.exe base"); return; }
    constexpr uintptr_t RENDER_MGR_SLOT_RVA = 0x47ef360; // DAT_1447ef360 (RE 3ce2b18)
    uint64_t mgr = 0;
    if (!seh_read8(reinterpret_cast<void *>(base + RENDER_MGR_SLOT_RVA), &mgr) || mgr < 0x10000)
    {
        spdlog::info("[RENDIMS] render-mgr slot (rva {:#x}) unread/null (mgr={:#x})",
                     RENDER_MGR_SLOT_RVA, mgr);
        return;
    }
    uint64_t begin = 0, end = 0;
    seh_read8(reinterpret_cast<void *>(mgr + 0x128), &begin);
    seh_read8(reinterpret_cast<void *>(mgr + 0x130), &end);
    int mgrDirty = 0;
    seh_read_i32(reinterpret_cast<void *>(mgr + 0xeb8), &mgrDirty);
    spdlog::info("[RENDIMS] backbuffer={}x{} mgr={:#x} list=[{:#x}..{:#x}] mgrDirty(+0xeb8)={}",
                 bbW, bbH, mgr, begin, end, mgrDirty & 0xff);
    if (begin < 0x10000 || end < begin || (end - begin) > 0x4000)
    {
        spdlog::info("[RENDIMS] output-list ptrs implausible — abort walk");
        return;
    }
    int i = 0;
    for (uint64_t e = begin; e < end && i < 16; e += 0x170, ++i)
    {
        float w = 0, h = 0;
        int outIdx = 0, dirty = 0;
        seh_read4(reinterpret_cast<void *>(e + 0x118), &w);
        seh_read4(reinterpret_cast<void *>(e + 0x11c), &h);
        seh_read_i32(reinterpret_cast<void *>(e + 0x128), &outIdx);
        seh_read_i32(reinterpret_cast<void *>(e + 0x140), &dirty);
        // Source dims + render-scale (mgr + outIdx*0x30 + 0x3c8/0x3cc/0x3ec): FUN_1419ebb40
        // recomputes the active dims FROM these. If source is fresh → fix(1) recompute is
        // enough; if source is also stale → fix(2) full re-apply (updates source first).
        int srcW = 0, srcH = 0;
        float rscale = 0;
        uint64_t srcBase = mgr + static_cast<uint64_t>(static_cast<uint32_t>(outIdx)) * 0x30;
        seh_read_i32(reinterpret_cast<void *>(srcBase + 0x3c8), &srcW);
        seh_read_i32(reinterpret_cast<void *>(srcBase + 0x3cc), &srcH);
        seh_read4(reinterpret_cast<void *>(srcBase + 0x3ec), &rscale);
        bool stale = (w != bbW || h != bbH);
        spdlog::info("[RENDIMS]  entry#{} outIdx={} activeWH={}x{} srcWH={}x{} rscale={} "
                     "dirty(+0x140)={} {}",
                     i, outIdx, w, h, srcW, srcH, rscale, dirty & 0xff,
                     stale ? "<-- STALE vs backbuffer" : "ok");
    }
}

// No-restart fix for mid-session resolution / display-mode changes. The earlier raw-poke
// (fix_render_dims, removed) only rewrote scalar dims — it fixed borderless same-aspect by
// luck but left windowed zoomed and fullscreen doubled, because the real corruption is
// stale GPU RESOURCES: the swapchain back-buffers + each output's render-target array are
// recreated at (or never resized from) the old resolution on a same-mode change (the
// engine's own apply gates ResizeBuffers on a fullscreen-state transition). The RE
// follow-up (defda96d / windows_midsession_resolution_swapchain_re_followup_findings.md)
// found FUN_1419ed440(renderMgr, W, H) = the COMPLETE re-apply: bump generation, release
// all per-output targets, UNCONDITIONAL ResizeBuffers on every output, refresh the source
// dims from GetDesc, then recompute+recreate every output's targets. One call fixes
// windowed / fullscreen / borderless. It must run on the render (Present) thread — never
// from hk_resize_buffers, since it calls ResizeBuffers internally (re-entrant).
using ReApplyResFn = void(__fastcall *)(void *mgr, uint32_t w, uint32_t h);
static ReApplyResFn g_reapply_fn = nullptr;
static bool g_reapply_tried = false;
static std::atomic<bool> g_reapply_in_progress{false}; // suppress the nested-resize loop
static std::atomic<bool> g_resize_pending{false};      // set by hk_resize_buffers

static void resolve_reapply_fn() // C++ EH (scan can throw) — kept out of the SEH frame
{
    g_reapply_tried = true;
    try
    {
        g_reapply_fn = modutils::scan<void(void *, uint32_t, uint32_t)>(
            {.aob = goblin::sig::RENDER_REAPPLY_RES});
    }
    catch (...)
    {
        g_reapply_fn = nullptr;
    }
    spdlog::info("[RENDIMS] render re-apply fn (FUN_1419ed440) @ {:p}", (void *)g_reapply_fn);
}

// SEH wrapper around the engine call — POD-only (no C++ object unwinding) so __try is
// legal here (MSVC/clang-cl forbids mixing __try with C++ EH in one function).
static void call_reapply_seh(void *mgr, uint32_t w, uint32_t h)
{
    __try
    {
        g_reapply_fn(mgr, w, h);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// Raw-poke BACKSTOP: force the render-output scalar dims (source mgr+0x3c8/0x3cc + active
// entry+0x108/0x10c int, +0x118/0x11c float) to W/H — the fields the map-fit + 3D viewport
// read. WPM-guarded. Kept alongside the engine re-apply (belt-and-suspenders): covers a
// partial re-apply or an unresolved engine fn. Returns # output entries written.
static int poke_render_dims(uint64_t mgr, uint64_t begin, uint64_t end, int w, int h)
{
    if (begin < 0x10000 || end < begin || (end - begin) > 0x4000) return 0;
    const float fw = static_cast<float>(w), fh = static_cast<float>(h);
    int n = 0;
    for (uint64_t e = begin; e < end && n < 16; e += 0x170)
    {
        int outIdx = 0;
        seh_read_i32(reinterpret_cast<void *>(e + 0x128), &outIdx);
        uint64_t src = mgr + static_cast<uint64_t>(static_cast<uint32_t>(outIdx)) * 0x30;
        seh_write_i32(reinterpret_cast<void *>(src + 0x3c8), w);
        seh_write_i32(reinterpret_cast<void *>(src + 0x3cc), h);
        seh_write_i32(reinterpret_cast<void *>(e + 0x108), w);
        seh_write_i32(reinterpret_cast<void *>(e + 0x10c), h);
        seh_write_f32(reinterpret_cast<void *>(e + 0x110), 0.f);
        seh_write_f32(reinterpret_cast<void *>(e + 0x114), 0.f);
        seh_write_f32(reinterpret_cast<void *>(e + 0x118), fw);
        seh_write_f32(reinterpret_cast<void *>(e + 0x11c), fh);
        ++n;
    }
    return n;
}

// Note a swapchain resize so hk_present fires the re-apply even when the dims read
// CONSISTENT — the fullscreen-doubling case is stale GPU resources with UNCHANGED dims, so
// a dims edge-trigger alone never catches it. Suppressed while our own re-apply runs (its
// nested ResizeBuffers re-enters hk_resize_buffers → would loop).
void note_resize_event()
{
    if (g_reapply_in_progress.load(std::memory_order_relaxed)) return;
    g_resize_pending.store(true, std::memory_order_relaxed);
}

// Mid-session resolution/mode enforcer, called every frame from hk_present. Fires the
// engine re-apply + a scalar poke when EITHER a resize event is pending (catches the
// consistent-dims fullscreen case) OR the active dims disagree with the live backbuffer
// (catches windowed stale dims + hook-missed paths). Gated by fix_midsession_resolution.
// Returns 1 if anything fired this call.
//
// TODO (residual — likely an ENGINE bug, not really ours to fix; revisit only if cheap):
//   Two windowed cases the re-apply + poke still don't fully clear (user 2026-06-21):
//   (1) windowed 720p -> 1920x1080 : the zoom still appears (the up-size path leaves a
//       stale view somewhere the re-apply doesn't reach).
//   (2) windowed 1920x1080 -> 720p : DOUBLE buffer — a stale 1080p-sized buffer lingers
//       BEHIND the real 720p game (old back-buffer not released/recreated on this path).
//   Both are window-resize paths where ER's own swapchain management leaves stale GPU
//   state our user-side hook can't cleanly own. Borderless + fullscreen-same-mode are the
//   fixed/intended cases; these windowed transitions are best-effort. If revisited: a full
//   swapchain RECREATE (vs ResizeBuffers) or a forced SetFullscreenState toggle may be the
//   only complete cure — both are heavy/risky and out of scope for the map overlay.
int reapply_render_res(int w, int h)
{
    if (w <= 0 || h <= 0) return 0;
    if (!g_reapply_tried) resolve_reapply_fn();

    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!base) return 0;
    uint64_t mgr = 0;
    if (!seh_read8(reinterpret_cast<void *>(base + 0x47ef360), &mgr) || mgr < 0x10000) return 0;
    uint64_t begin = 0, end = 0;
    if (!seh_read8(reinterpret_cast<void *>(mgr + 0x128), &begin) || begin < 0x10000) return 0;
    seh_read8(reinterpret_cast<void *>(mgr + 0x130), &end);

    // Trigger: pending resize event OR active dims (entry+0x118/+0x11c) ≠ live backbuffer.
    const bool pending = g_resize_pending.exchange(false, std::memory_order_relaxed);
    float aw = 0, ah = 0;
    seh_read4(reinterpret_cast<void *>(begin + 0x118), &aw);
    seh_read4(reinterpret_cast<void *>(begin + 0x11c), &ah);
    const bool mismatch = (static_cast<int>(aw) != w || static_cast<int>(ah) != h);
    if (!pending && !mismatch) return 0;

    // (1) Engine re-apply = the real GPU-resource recreation (release + ResizeBuffers +
    //     recreate targets). Render-thread + re-entrancy guarded. Skipped if the AOB didn't
    //     resolve → only the poke runs (degrades to the old partial fix).
    bool fired = false;
    if (g_reapply_fn && !g_reapply_in_progress.load(std::memory_order_relaxed))
    {
        bool thread_ok = true;
        uint64_t rt = 0;
        if (seh_read8(reinterpret_cast<void *>(mgr + 0x6d0), &rt) && rt > 0x10000)
        {
            int rtid = 0;
            if (seh_read_i32(reinterpret_cast<void *>(rt + 0x14), &rtid) && rtid &&
                static_cast<uint32_t>(rtid) != GetCurrentThreadId())
                thread_ok = false;
        }
        if (thread_ok)
        {
            g_reapply_in_progress.store(true, std::memory_order_relaxed);
            call_reapply_seh(reinterpret_cast<void *>(mgr), static_cast<uint32_t>(w),
                             static_cast<uint32_t>(h));
            g_reapply_in_progress.store(false, std::memory_order_relaxed);
            fired = true;
        }
    }

    // (2) Scalar-dims poke backstop (covers a partial re-apply / unresolved engine fn).
    const int n = poke_render_dims(mgr, begin, end, w, h);

    spdlog::info("[RENDIMS] reapply_render_res({}x{}) engine={} poked={} (pending={} mismatch={})",
                 w, h, fired, n, pending, mismatch);
    return (fired || n) ? 1 : 0;
}

} // namespace goblin::worldmap_probe
