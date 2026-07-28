#include "input_gamepad.hpp"
#include "input_shared.hpp"

#include <intrin.h>   // _ReturnAddress() — caller-range check in hk_xinput_get_state

#include <MinHook.h>
#include <spdlog/spdlog.h>

#include "goblin_crashdump.hpp"   // goblin::self_module_range() — XInputGetState hook caller check
#include "goblin_inject.hpp"                 // goblin::world_map_open() — in-world chip input gate
#include "goblin_overlay_render_loader.hpp"  // call_inworld_hovered() — "the reticle is on a chip"

namespace goblin::input
{
namespace
{
using XInputGetStateFn = DWORD(WINAPI *)(DWORD, XINPUT_STATE *);
XInputGetStateFn o_xinput_get_state = nullptr;
bool g_xinput_available = false;

// XInputGetState is polled, not message-based, so unlike mouse/keyboard it can't be
// swallowed via a wndproc — the game and ImGui's own gamepad-nav backend read the SAME
// physical controller state. While the menu is open: a caller inside OUR OWN module
// (ImGui's vendored backend, or our own toggle/recenter poll via xinput_get_state_real())
// gets the real data; a caller outside it (the game) gets a REAL, CONNECTED result with the
// Gamepad struct zeroed. NOT ERROR_DEVICE_NOT_CONNECTED: that simulates an actual unplug,
// and games commonly back off / debounce reconnect-polling a "disconnected" slot — reported
// in testing as the controller feeling unresponsive to the game for a bit after closing the
// menu. Reporting SUCCESS with zeroed buttons/sticks each poll also means any button
// released while the menu was open is delivered as a real release (dwPacketNumber still
// advances from the real state), so nothing can look "stuck held" once the menu closes.
DWORD WINAPI hk_xinput_get_state(DWORD user_index, XINPUT_STATE *state)
{
    const DWORD result = o_xinput_get_state(user_index, state);
    if (result == ERROR_SUCCESS && state)
    {
        const auto [self_base, self_end] = goblin::self_module_range();
        const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
        const bool caller_is_us = self_base && ret >= self_base && ret < self_end;
        if (!caller_is_us)
        {
            if (menu_open())
                state->Gamepad = {};   // connected, real packet number, but nothing held
            // The fullscreen vmap STANDS IN for the native map, so the game must not act on the pad
            // either — B closed our map AND rolled/cancelled in ER underneath, X placed a pin AND fired
            // its own action (user 2026-07-28: "double input"). Everything is masked EXCEPT the map
            // button itself (BACK/START): that press is what the WorldMapDialog create-hook toggles the
            // redirect on, i.e. the way out. Buttons only — sticks stay live so the game keeps stepping.
            else if (vmap_covers_map())
                state->Gamepad.wButtons &= (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START);
            // F1 CLOSED, native map open, and our in-world reticle is sitting on a region chip: hide
            // ONLY X from the game, so activating the chip can't also fire the map screen's own X
            // action. Exactly the contract the mouse path already has (input_wndproc eats the L-press
            // over a chip and nothing else) — pad input is polled, not message-based, so the mask has
            // to happen here. ImGui's own backend polls from inside this module (caller_is_us) and
            // still sees the real X, which is what toggles the chip.
            else if (goblin::world_map_open() && goblin::overlay_render_loader::call_inworld_hovered())
                state->Gamepad.wButtons &= ~XINPUT_GAMEPAD_X;
        }
    }
    return result;
}
} // namespace

void install_xinput_hook()
{
    // Resolved dynamically (no static link dep). Same load order the original poll used:
    // newest-to-oldest so a system that only has the older redistributable still works.
    XInputGetStateFn raw = nullptr;
    for (const char *dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"})
    {
        if (HMODULE h = LoadLibraryA(dll))
        {
            raw = reinterpret_cast<XInputGetStateFn>(GetProcAddress(h, "XInputGetState"));
            if (raw) { spdlog::info("[OVERLAY] XInput loaded: {}", dll); break; }
        }
    }
    if (!raw)
    {
        spdlog::warn("[OVERLAY] No XInput DLL found — gamepad toggle/recenter/nav disabled");
        return;
    }
    MH_STATUS cs = MH_CreateHook(reinterpret_cast<void *>(raw),
                                  reinterpret_cast<void *>(&hk_xinput_get_state),
                                  reinterpret_cast<void **>(&o_xinput_get_state));
    MH_STATUS es = (cs == MH_OK) ? MH_EnableHook(reinterpret_cast<void *>(raw)) : cs;
    if (cs != MH_OK || es != MH_OK)
        spdlog::error("[OVERLAY] XInputGetState HOOK FAILED (create={}, enable={}) — "
                      "gamepad toggle/recenter/nav disabled",
                      MH_StatusToString(cs), MH_StatusToString(es));
    else
    {
        g_xinput_available = true;
        spdlog::info("[OVERLAY] XInputGetState hook installed");
    }
}

bool xinput_available() { return g_xinput_available; }

DWORD xinput_get_state_real(DWORD user_index, XINPUT_STATE *state)
{
    return o_xinput_get_state(user_index, state);
}
} // namespace goblin::input
