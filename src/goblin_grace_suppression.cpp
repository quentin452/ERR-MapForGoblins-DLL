#include "goblin_inject.hpp"
#include "goblin_config.hpp"
#include "modutils.hpp"

#include <spdlog/spdlog.h>
#include <windows.h>

//
// Native grace-pin suppression (builder log + DRAW-ONLY hide) — split out of
// goblin_inject.cpp 2026-07-01 (docs/plans/goblin_inject_refactor_plan.md
// PR 4c). Pure relocation, no logic changes. Already known-independent since
// before PR 3's audit (RE e4b3f6a / windows_grace_warppin_teleport_re_findings.md
// §4) — fully self-contained per a fresh file-wide grep, zero coupling to
// anything else in goblin_inject.cpp. Public entry point
// (install_grace_suppression_hook) declared in goblin_inject.hpp, called
// from dllmain.cpp — facade unchanged. Local duplicate of icon_rpm_i32/
// icon_rpm_ptr moves with it (same per-file-copy convention PR 0/1
// established).
//

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
