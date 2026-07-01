#include "goblin_inject.hpp"
#include "goblin_inject_shared.hpp"
#include "goblin_collected.hpp"
#include "goblin_kindling.hpp"
#include "goblin_config.hpp"
#include "goblin_messages.hpp"
#include "modutils.hpp"
#include "re_signatures.hpp"
#include "goblin_map_data.hpp"
#include "goblin_category_exceptions.hpp"
#include "goblin_region_anchors.hpp"
#include "goblin_major_regions.hpp"
#include "goblin_name_regions.hpp"
#include "goblin_tile_tabs.hpp"
#include "goblin_legacy_conv.hpp"
#include "goblin_legacy_fold.hpp"
#include "goblin_logic.hpp"
#include "goblin_markers.hpp"
#include "goblin_bench.hpp"
#include "from/params.hpp"
#include "from/paramdef/WORLD_MAP_POINT_PARAM_ST.hpp"
#include "from/paramdef/WORLD_MAP_PIECE_PARAM_ST.hpp"
#include "from/paramdef/BONFIRE_WARP_PARAM_ST.hpp"
#include "from/paramdef/BONFIRE_WARP_SUB_CATEGORY_PARAM_ST.hpp"
#include "goblin_quest_steps.hpp"
#include "goblin_logic.hpp"
#include "worldmap/loot_disk.hpp"  // read_game_file_decompressed (no-bake item-icon layout source)

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>
#include <deque>
#include <cctype>
#include <chrono>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <spdlog/spdlog.h>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <Xinput.h>
#pragma comment(lib, "Xinput.lib")

using ParamRowInfo = from::params::ParamRowInfo;
using ParamTable = from::params::ParamTable;
using ParamResCap = from::params::ParamResCap;
using Category = goblin::generated::Category;

static void *allocation = nullptr;



// Data pointers of MFG-injected WorldMapPointParam rows in the expanded table.
// Used by sanitize_injected_textids() (run after the FMG is built) to strip
// textIds that don't resolve to a real string.
static std::vector<uint8_t *> g_injected_row_ptrs;

static std::set<uint8_t *> g_lot_backed_set;




// EXPERIMENT: toast-method cycler. F10 fires a toast with the current method;
// F11 cycles the method. Lets us A/B every text-injectable notification path
// in-game. Remove once the final style is chosen.
// Default = method 1 (trampoline). User confirmed this is the codex-style
// upper-left plaque we want. Methods 0/2/3 retained for A/B testing via F11.
static std::atomic<int> g_toast_method{1};
static const char *const TOAST_METHOD_NAMES[] = {
    "0 Summon p=1 fp=1 u=1 (narrow plaque just below center, ~5s)",
    "1 ShowTutorialPopup trampoline (AOB-resolved; codex upper-left, default)",
};
// TutorialParam row ids exposed in goblin_inject.hpp. These are NEW rows
// injected by inject_tutorial_popup_rows() with textId pointing at
// TutorialBody.fmg entries injected by goblin_messages — so the upper-left
// codex toast renders our text without modifying any vanilla/ERR data.
static constexpr int TOAST_METHOD_COUNT =
    (int)(sizeof(TOAST_METHOD_NAMES) / sizeof(TOAST_METHOD_NAMES[0]));






// (The old Summon-message path (post_summon) was removed: it depended on five
// hardcoded RVAs (0x763360/0x11A3E0/0x843860/0x844060/0x843910) that a game
// update invalidates, and the codex trampoline below is the toast style we
// actually ship. The F10/F9 banner uses the AOB-resolved trampoline only.)

// ShowTutorialPopup callers — codex/medal upper-left toast.
// Three entries pinned by static analysis (agent run, May 2026):
//   - inner   0x7EF5B0  `void(CSPopupMenu*, int id, bool, bool)` (286-byte fn)
//   - outer   0x7EE630  `void(CSPopupMenu*, int id, bool)` (4 direct call sites)
//   - tramp   0x80DA50  `void(int id)` — resolves singleton internally
// CSPopupMenu singleton ptr lives in .data at `CSFeMan_slot + 0x80`.
// AOB anchor for outer (24 bytes, unique across image):
//   48 8B C4 44 88 40 18 89 50 10 55 56 57 41 56 41 57 48 8D 68 A1 48 81 EC
// Patch-resilient anchor: LEA xref in real-.text to string
//   "CS::CSPopupMenu::_CanOpenTutorialParam" in .rdata.
//
// Note: eldenring.exe has TWO `.text` sections (VMProtect adds one). When
// pinning via pefile, scan the original MSVC `.text` at RVA 0x1000..0x29A3000,
// NOT the VMP-added one at 0x4C0E000+ — different content, will miss real fns.
// Resolve the trampoline by AOB (NOT a hardcoded RVA): a game update shifts
// every function's RVA (the May-2026 patch moved this one from 0x80DA50 to
// 0x80D960), so we pin it by a stable surrounding-byte signature that survives
// patches. modutils::scan returns the address of the AOB's first byte = the
// function entry. Resolved once and cached.
static void show_tutorial_popup_trampoline(uintptr_t /*er*/, int tutorial_id)
{
    static void (*fn)(int) = nullptr;
    static bool tried = false;
    if (!tried)
    {
        tried = true;
        fn = reinterpret_cast<void (*)(int)>(modutils::scan<void>({
            .aob = goblin::sig::WORLDMAP_POINT_FN,
        }));
        spdlog::info("[TOAST] trampoline ShowTutorialPopup @ {:p}", (void *)fn);
    }
    if (fn) fn(tutorial_id);
}

// SEH-guarded dispatch of one toast method. POD-only locals (no C++ unwinding).
static void seh_dispatch_toast(int method, uintptr_t er, void * /*mm*/, void *fe,
                               void ** /*csfeman_slot*/, bool icons_on,
                               const wchar_t *text)
{
    (void)method; (void)fe; (void)text;
    int tutorial_id = icons_on ? goblin::TUTORIAL_FMG_ID_ON : goblin::TUTORIAL_FMG_ID_OFF;
    __try
    {
        // Only the AOB-resolved codex trampoline remains (Summon path removed).
        show_tutorial_popup_trampoline(er, tutorial_id);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}



// ── Native discovered-grace pin suppression (RE e4b3f6a; config grace_suppress_native) ──
// Graces are WorldMapWarpPinData built by FUN_14088b7b0(this, out, WarpData* param_3) from a
// WarpData whose source entry (warpData+0x8) holds: state byte @+0x1E (bits 0/1/2 = registered/
// discovered/visible), iconId @+0x08. PHASE A (now): hook + LOG each build ([WARPPIN] state/iconId)
// to confirm we can identify discovered graces at build time. Suppression (skip/hide the discovered
// ones) is added once the log confirms identification. Read-only RPM in the detour; calls the orig.
namespace
{
using warp_pin_fn = void *(__fastcall *)(void *, void *, void *, void *);
warp_pin_fn g_warp_pin_orig = nullptr;

// Local duplicate of goblin_icon_harvest.cpp's icon_rpm_i32/icon_rpm_ptr (same
// per-file-copy convention as e.g. goblin_markers.cpp / goblin_kindling.cpp keeping
// their own AOB copies) — this detour is the one place in this file that still
// needs them after the icon-harvest block moved out (PR 1).
inline bool icon_rpm_i32(uintptr_t a, int &out)
{
    SIZE_T n = 0;
    return a && ReadProcessMemory(GetCurrentProcess(), (void *)a, &out, 4, &n) && n == 4;
}
inline bool icon_rpm_ptr(uintptr_t a, uintptr_t &out)
{
    SIZE_T n = 0;
    return a && ReadProcessMemory(GetCurrentProcess(), (void *)a, &out, 8, &n) && n == 8;
}

void *__fastcall warp_pin_detour(void *a1, void *a2, void *warpData, void *a4)
{
    void *ret = g_warp_pin_orig(a1, a2, warpData, a4);
    if (!goblin::config::graceSuppressNative) return ret;

    // Identify the just-built grace pin's draw state (source state byte @ warpData+0x8 +0x1E;
    // state != 0 = a drawn/registered grace, as confirmed by [WARPPIN]).
    uintptr_t src = 0; icon_rpm_ptr(reinterpret_cast<uintptr_t>(warpData) + 0x8, src);
    int state = 0, iconId = -1;
    if (src > 0x10000)
    {
        int sb = 0; icon_rpm_i32(src + 0x1C, sb);   // +0x1E = byte 2 of the dword at +0x1C
        state = (sb >> 16) & 0xff;
        icon_rpm_i32(src + 0x08, iconId);
    }

    // Suppression now happens at the vt[1] SetTo apply point (warp_setto_detour below) by hiding
    // the pin's "Icon_0" child — NOT here. Zeroing pin+0x60/+0xC suppressed the draw but also broke
    // fast-travel (the map cursor only snaps to a _visible widget; draw + click are coupled at
    // _visible — RE windows_grace_warppin_teleport_re_findings.md). This builder hook is kept
    // log-only (phase-A diagnostic: confirms discovered-grace identity at build time).
    static int logged = 0;
    if (logged < 40)
    {
        ++logged;
        spdlog::info("[WARPPIN] build src={:#x} stateByte(+0x1E)={:#x} (bits0-2={}) iconId={} pin={:#x}",
                     src, state & 0xff, state & 7, iconId, reinterpret_cast<uintptr_t>(ret));
    }
    return ret;
}

// ── Grace DRAW-ONLY suppression (RE windows_grace_warppin_teleport_re_findings.md §4) ──
// Hook vt[1] WorldMap(Warp|Point)PinData::SetTo = FUN_14087ae20: the per-refresh widget bind that
// sets widget._visible from pin+0xC. After the original runs, for a DISCOVERED WarpPinData, hide
// ONLY the "Icon_0" child of the pin's GFx widget — the outer widget stays _visible, so the map
// cursor still snaps to it and fast-travel works; only the native icon image is gone (the overlay
// draws our own). pin+0xC/+0x60 are left untouched (poking them is the layer-trap that re-fills
// every SetTo). Runs on the engine thread (in-context), so we call the GFx fns directly.
using setto_fn = void *(__fastcall *)(void *, void *, void *, void *);
setto_fn g_setto_orig = nullptr;
// GFx helpers (resolve at hook-install, cached): get a named child proxy / set a widget _visible /
// release the stack proxy. Signatures inferred from the RE pseudocode (doc §4).
void *g_warppin_vftable = nullptr;   // er + 0x2ad8228 (WorldMapWarpPinData::vftable) — grace filter

void *__fastcall warp_setto_detour(void *pin, void *widgetRoot, void *a3, void *a4)
{
    // Decide suppression BEFORE calling orig. SetTo binds the per-pin GFx row, reading pin+0xC to set
    // the row's _visible — then RELEASES the (stack) widget proxy at its end (FUN_140d7f850). So poking
    // the proxy AFTER orig is a no-op on a dead proxy (that was the bug). Instead force pin+0xC=0 around
    // orig so SetTo's OWN set_visible(widgetRoot,0) runs while the proxy is live → row hidden. Restore
    // pin+0xC afterward so the cursor's nearest-pin selection (FUN_1409cab60, reads pin+0xC + position,
    // NOT the row _visible) still treats the grace as selectable → fast-travel survives.
    bool suppress = false;
    uint8_t *pVis = nullptr;
    uint8_t savedVis = 0;
    if (goblin::config::graceSuppressNative && goblin::config::graceOverlay && pin && widgetRoot
        && *reinterpret_cast<void **>(pin) == g_warppin_vftable)
    {
        uint32_t state = *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(pin) + 0x60);
        if ((state & 7) != 0)   // discovered grace (undiscovered → engine draws nothing anyway)
        {
            suppress = true;
            pVis = reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(pin) + 0xC);
            savedVis = *pVis;
            *pVis = 0;   // SetTo will bind the row _visible = 0
        }
    }
    void *ret = g_setto_orig(pin, widgetRoot, a3, a4);
    if (suppress)
        *pVis = savedVis ? savedVis : 1;   // restore so selection vt[6] keeps the grace clickable
    return ret;
}
} // namespace

void goblin::install_grace_suppression_hook()
{
    if (!goblin::config::graceSuppressNative) return;
    uintptr_t er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!er) return;
    void *fn = reinterpret_cast<void *>(er + 0x88b7b0);   // FUN_14088b7b0 (RE e4b3f6a §1)
    try
    {
        modutils::hook(fn, reinterpret_cast<void *>(&warp_pin_detour),
                       reinterpret_cast<void **>(&g_warp_pin_orig));
        spdlog::info("[WARPPIN] WarpPinData builder hooked @ {} (phase A: log only)", fn);
    }
    catch (const std::exception &e)
    {
        spdlog::error("[WARPPIN] hook failed: {}", e.what());
        g_warp_pin_orig = nullptr;
    }

    // DRAW-ONLY suppression: hook vt[1] SetTo and force pin+0xC=0 around orig so SetTo's own
    // set_visible hides the per-pin GFx row while its proxy is live (poking after orig hit a released
    // proxy = no-op, the earlier bug). pin+0xC is restored after orig so the cursor's nearest-pin
    // selection (FUN_1409cab60, reads pin+0xC + position) keeps the grace clickable → teleport survives.
    // (RE windows_grace_warppin_cursor_re_findings.md.)
    constexpr bool kSetToHookEnabled = true;
    if (kSetToHookEnabled)
    {
        g_warppin_vftable = reinterpret_cast<void *>(er + 0x2ad8228); // WorldMapWarpPinData::vftable
        void *setto = reinterpret_cast<void *>(er + 0x87ae20);   // FUN_14087ae20 (vt[1] SetTo)
        try
        {
            modutils::hook(setto, reinterpret_cast<void *>(&warp_setto_detour),
                           reinterpret_cast<void **>(&g_setto_orig));
            spdlog::info("[WARPPIN] SetTo hooked @ {} (draw-only: hide row, keep teleport)", setto);
        }
        catch (const std::exception &e)
        {
            spdlog::error("[WARPPIN] SetTo hook failed: {}", e.what());
            g_setto_orig = nullptr;
        }
    }
    else
    {
        (void)&warp_setto_detour; // keep referenced while the hook is disabled
        spdlog::info("[WARPPIN] SetTo draw-only hook DISABLED (proxy ABI crashed; pending RE)");
    }
}



// Save request posted by the menu; the watcher does the file I/O.
static std::atomic<bool> g_save_req{false};
void goblin::ui::request_save() { g_save_req.store(true); }

// Danger zone. Clearing quest progress is just a string reset (render-thread
// safe; the browser reparses config::questProgress each frame). Reset-to-defaults
// re-seeds + writes the ini, so it's posted to the watcher to keep file I/O off
// the render thread.
static std::atomic<bool> g_reset_defaults_req{false};
void goblin::ui::reset_quest_progress() { goblin::config::questProgress.clear(); }
void goblin::ui::reset_to_defaults() { g_reset_defaults_req.store(true); }

// Toast request QUEUE. Replaces the old single-slot request so several toasts
// (e.g. a burst of coverage-gap hits) can be shown in sequence instead of
// overwriting each other. The watcher (menu_auto_toggle_loop) drains it one at
// a time, spaced TOAST_SPACING_MS apart so each stays on screen.
static std::mutex g_toast_mtx;
static std::deque<int> g_toast_queue;        // tutorial ids waiting to fire
static int64_t g_next_toast_ms = 0;          // earliest time the next may fire
static constexpr size_t TOAST_QUEUE_CAP = 32;
static constexpr int64_t TOAST_SPACING_MS = 2500;

static void show_toggle_banner(bool icons_on)
{
    static bool resolved = false;
    static uintptr_t er = 0;
    static void **menu_man_slot = nullptr;
    static void **fe_man_slot = nullptr;
    if (!resolved)
    {
        resolved = true;
        er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
        menu_man_slot = reinterpret_cast<void **>(modutils::scan<void *>({
            .aob = goblin::sig::CSMENUMAN_SLOT,
            .relative_offsets = {{3, 7}},
        }));
        fe_man_slot = reinterpret_cast<void **>(modutils::scan<void *>({
            .aob = goblin::sig::EVENT_FLAG_MAN_SLOT_ALT,
            .relative_offsets = {{3, 7}},
        }));
        spdlog::info("[TOAST] resolve er=0x{:X} CSMenuMan_slot={:p} CSFeMan_slot={:p}",
                     er, (void *)menu_man_slot, (void *)fe_man_slot);
    }
    if (!er || !menu_man_slot || !fe_man_slot) return;
    void *mm = *menu_man_slot, *fe = *fe_man_slot;
    if (!mm || !fe) return;

    int method = g_toast_method.load();
    if (method < 0 || method >= TOAST_METHOD_COUNT) method = 0;
    const wchar_t *text = icons_on ? L"Map icons: ON" : L"Map icons: OFF";
    spdlog::info("[TOAST] fire method [{}] (icons {})", TOAST_METHOD_NAMES[method],
                 icons_on ? "ON" : "OFF");
    seh_dispatch_toast(method, er, mm, fe, fe_man_slot, icons_on, text);
}

// SEH-guarded trampoline fire (POD-only locals — no C++ unwinding).
static void seh_fire_trampoline(uintptr_t er, int tutorial_id)
{
    __try { show_tutorial_popup_trampoline(er, tutorial_id); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// Fire an upper-left codex toast for one of the injected TutorialParam rows
// (a TUTORIAL_FMG_ID_* id). Static text via the same trampoline path as the
// F10 banner — no FMG rewrite. Used by the F9 marker-dump banner.
void goblin::show_codex_toast(int tutorial_id)
{
    static uintptr_t er = 0;
    if (!er) er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!er) return;
    seh_fire_trampoline(er, tutorial_id);
}

// Queue a codex toast to be fired (spaced) by the watcher thread. Thread-safe;
// callable from any thread (e.g. the debug-events drain). Drops silently if the
// queue is full so a flood of gaps can't grow it without bound.
void goblin::enqueue_toast(int tutorial_id)
{
    if (tutorial_id <= 0) return;
    std::lock_guard<std::mutex> lk(g_toast_mtx);
    if (g_toast_queue.size() < TOAST_QUEUE_CAP)
        g_toast_queue.push_back(tutorial_id);
}


// WorldMapPointParam state owner. Since the 16-align fix in inject_map_entries
// (see docs/ersc_hosting_and_map_autohide.md), the expanded table is safe during
// ERSC hosting — the old "expand only while the map is open" auto-hide is no
// longer needed and has been removed. The table now stays EXPANDED always; the
// hotkey is a pure personal show/hide toggle.
//
// Desired table state:
//   userDisabled (F10/gamepad master-off) -> VANILLA  (user hid the icons)
//   else                                  -> EXPANDED  (icons everywhere)
//
// (The retired map-state auto-hide read CSMenuMan+0xCD with inverse logic;
// it's fully documented in docs/ersc_hosting_and_map_autohide.md should a
// future patch ever need it back.)
const std::vector<uint8_t *> &goblin::injected_row_ptrs()
{
    return g_injected_row_ptrs;
}

// ── Either-flag (OR) kill indicators ─────────────────────────────────
// Some quest fights have two mutually-exclusive completion flags (one per
// story branch) and no single "battle over" flag. Example: the academy
// battle — 7608 = Sellen's battle body defeated (sided with Jerren),
// 7609 = Jerren defeated (sided with Sellen); after either one, BOTH NPCs
// stop being attackable, so both markers should show the checkmark.
// Such rows are baked with the PRIMARY flag; once the ALT flag turns on
// this rewrites the matching fields so the checkmark/hide reacts within
// the running session. Pairs mirror data/quest_invader_overrides.json.
//
// Event-flag query — same AOBs as goblin_markers.cpp / goblin_kindling.cpp
// (each keeps its own local copy by established convention there).
using OrPairIsFlagFn = bool (*)(void *, uint32_t *);
static OrPairIsFlagFn g_orp_is_flag = nullptr;
static void **g_orp_event_man_slot = nullptr;
static bool g_orp_resolve_tried = false;

bool orp_flag_set(uint32_t flag_id)
{
    if (!g_orp_resolve_tried)
    {
        g_orp_resolve_tried = true;
        try
        {
            g_orp_is_flag = modutils::scan<bool(void *, uint32_t *)>(
                { .aob = goblin::sig::IS_EVENT_FLAG });
            g_orp_event_man_slot = reinterpret_cast<void **>(modutils::scan<void *>(
                { .aob = goblin::sig::EVENT_FLAG_MAN_SLOT,
                  .relative_offsets = { {3, 7} } }));
        }
        catch (...) { g_orp_is_flag = nullptr; g_orp_event_man_slot = nullptr; }
    }
    if (!g_orp_is_flag || !g_orp_event_man_slot) return false;
    void *event_man = *g_orp_event_man_slot;
    if (!event_man) return false;
    uint32_t id = flag_id;
    return g_orp_is_flag(event_man, &id);
}

// Accessors for goblin_loot_resolve.cpp's live-persistence classifier — see
// goblin_inject_shared.hpp. This resolve is genuinely shared with other sections
// in THIS file (quest-finishable cache, cluster depletion, apply_flag_or_pairs,
// goblin::ui::read_event_flag), so it stays owned here, not moved.
bool orp_manager_resolve_tried() { return g_orp_resolve_tried; }
void **orp_event_man_slot_ptr() { return g_orp_event_man_slot; }

// Part 2: per-questline "unfinishable" cache. One byte per QUEST_BROWSER entry,
// indexed by array order (same index the overlay passes). Written here on the
// watcher thread, read by ui::quest_unfinishable() on the render thread (a
// single-byte read; a benign cross-thread race at worst flips one frame late).
static std::vector<uint8_t> g_quest_unfinishable;

int goblin::refresh_quest_finishable()
{
    const size_t n = goblin::generated::QUEST_BROWSER_COUNT;
    if (g_quest_unfinishable.size() != n) g_quest_unfinishable.assign(n, 0);
    // Cold-API safety: if AlwaysOn (6001) can't read true, leave the cache as-is
    // rather than marking everything finishable on a not-yet-warm flag API.
    if (!orp_flag_set(6001)) return 0;
    int unfinishable = 0;
    for (size_t i = 0; i < n; i++)
    {
        uint32_t f = goblin::generated::QUEST_BROWSER[i].fail_flag;
        bool dead = (f != 0) && orp_flag_set(f);
        g_quest_unfinishable[i] = dead ? 1 : 0;
        if (dead) unfinishable++;
    }
    return unfinishable;
}

bool goblin::ui::quest_unfinishable(size_t i)
{
    return i < g_quest_unfinishable.size() && g_quest_unfinishable[i] != 0;
}

// Live event-flag reader exposed for the overlay's flag-capture finalize step
// (re-check captured flags so only PERSISTED ones are logged). Plain free
// function so it matches the bool(*)(uint32_t) callback the capture tool takes.
bool goblin::ui::read_event_flag(uint32_t id) { return orp_flag_set(id); }


struct FlagOrPair { uint32_t primary; uint32_t alt; };
static constexpr FlagOrPair FLAG_OR_PAIRS[] = {
    {7608, 7609},  // Sellen/Jerren academy battle
};

void goblin::apply_flag_or_pairs()
{
    GOBLIN_BENCH("refresh.flag_or_pairs");
    for (const auto &pr : FLAG_OR_PAIRS)
    {
        if (!orp_flag_set(pr.alt))
            continue;
        for (uint8_t *ptr : g_injected_row_ptrs)
        {
            // Skip live-loot rows: their textDisableFlagId1 holds a lot pickup
            // flag (set by refresh_loot_from_itemlot), not a boss/quest flag —
            // don't let a value-collision rewrite it.
            if (g_lot_backed_set.count(ptr)) continue;
            auto *p = reinterpret_cast<from::paramdef::WORLD_MAP_POINT_PARAM_ST *>(ptr);
            if (p->clearedEventFlagId == pr.primary) p->clearedEventFlagId = pr.alt;
            if (p->textDisableFlagId1 == pr.primary) p->textDisableFlagId1 = pr.alt;
            if (p->textDisableFlagId2 == pr.primary) p->textDisableFlagId2 = pr.alt;
        }
    }
}

void goblin::menu_auto_toggle_loop()
{
    bool prev_user_disabled = icons_user_disabled();

    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Master-off banner. The overlay itself honours the toggle — goblin_overlay
        // skips render_markers when ui::icons_enabled() is false — so this thread only
        // fires the visual banner when the user flips master-off via the F10 hotkey.
        bool user_disabled_now = icons_user_disabled();
        if (user_disabled_now != prev_user_disabled)
        {
            show_toggle_banner(!user_disabled_now);
            prev_user_disabled = user_disabled_now;
        }

        // Per-section toggle: persist the choice to the ini (single owner of file I/O,
        // off the render thread). The overlay reads section_visible() live via the
        // ui:: getter, so no blob mutation is needed — just save.
        if (take_section_apply_req() >= 0)
            goblin::save_section_states(goblin::config_ini_path());

        // Toast queue: fire one at a time, spaced so consecutive toasts don't
        // overwrite each other on screen.
        {
            int fire_id = 0;
            int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
            {
                std::lock_guard<std::mutex> lk(g_toast_mtx);
                if (!g_toast_queue.empty() && now_ms >= g_next_toast_ms)
                {
                    fire_id = g_toast_queue.front();
                    g_toast_queue.pop_front();
                    g_next_toast_ms = now_ms + TOAST_SPACING_MS;
                }
            }
            if (fire_id)
                goblin::show_codex_toast(fire_id);
        }

        // Menu "Save" → persist current visibility to the ini (file I/O here, off
        // the render thread).
        if (g_save_req.exchange(false))
            persist_settings();

        // Danger zone: re-seed config from defaults + write the ini (off the render
        // thread). Runtime visibility is unchanged until a restart.
        if (g_reset_defaults_req.exchange(false))
            goblin::reset_to_defaults_and_save(goblin::config_ini_path());
    }
}
