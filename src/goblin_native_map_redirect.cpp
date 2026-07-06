// Native-map REDIRECT — hook the WorldMapDialog create-callback so the native map NEVER opens; the vmap
// stands in as the map instead. docs/re/native_map_redirect_linux_re_plan.md (Ghidra pin 7d1f23b).
//
// FUN_1407fd4b0 (er+0x7fd4b0) is the WORLD-MAP-ONLY create-callback: registered as the WorldMapDialog slot
// in the menu-dialog factory table (er+0x2abb910), it allocates 0x3ed0 (WorldMapDialog sizeof) and calls the
// ctor — it can construct nothing else. Both keybind systems (kb + gamepad) funnel through this one table
// call, so ONE hook covers both. When the user opted the vmap in as the map (config vmap_on_map_key), the
// detour returns null WITHOUT calling the original → nothing is allocated or pushed (menu stack stays
// balanced), the native map never opens or renders, and we TOGGLE the vmap redirect flag (the render mirrors
// the vmap open state to it). Off → construct the native normally.
#include "goblin_inject.hpp"
#include "goblin_config.hpp"
#include "goblin_overlay_render_api.hpp"
#include "re_signatures.hpp"
#include "modutils.hpp"

#include <windows.h>
#include <exception>
#include <spdlog/spdlog.h>

namespace
{
using wm_create_fn = void *(__fastcall *)(void *, void *, void *, void *);
wm_create_fn g_wm_create_orig = nullptr;

void *__fastcall wm_create_detour(void *a1, void *a2, void *a3, void *a4)
{
    if (goblin::config::vmapOnMapKey)
    {
        const bool now_open = !goblin::overlay_api::vmap_redirect();  // toggle: each map-key press flips it
        goblin::overlay_api::set_vmap_redirect(now_open);
        spdlog::info("[VMAP-REDIRECT] native map-open suppressed -> vmap {}", now_open ? "OPEN" : "CLOSE");
        return nullptr;  // native WorldMapDialog never constructed
    }
    return g_wm_create_orig(a1, a2, a3, a4);
}
} // namespace

void goblin::install_native_map_redirect_hook()
{
    uintptr_t er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!er) return;
    void *fn = goblin::sig::resolve_func_aob(goblin::sig::WORLDMAP_CREATE_CB, er, 0x7fd4b0, "WORLDMAP_CREATE_CB");
    if (!fn)
    {
        spdlog::error("[VMAP-REDIRECT] WorldMapDialog create-cb not resolved — redirect disabled");
        return;
    }
    try
    {
        modutils::hook(fn, reinterpret_cast<void *>(&wm_create_detour),
                       reinterpret_cast<void **>(&g_wm_create_orig));
        spdlog::info("[VMAP-REDIRECT] WorldMapDialog create-cb hooked @ {}", fn);
    }
    catch (const std::exception &e)
    {
        spdlog::error("[VMAP-REDIRECT] hook failed: {}", e.what());
        g_wm_create_orig = nullptr;
    }
}
