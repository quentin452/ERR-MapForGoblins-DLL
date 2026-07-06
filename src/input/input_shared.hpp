#pragma once

// Small cross-cutting accessors shared between goblin_overlay.cpp (which owns the actual
// g_show/g_user_show/... state) and the input hook modules under src/input/. Kept to
// read-only accessors deliberately — the state itself stays defined in goblin_overlay.cpp
// (its ~50 existing call sites are unchanged), this just gives the split-out hooks a way to
// read it without duplicating or renaming it. See docs/plans/input_module_refactor_plan.md.

#include "goblin_dll_export.hpp"  // GOBLIN_RENDER_API (no-op unless GOBLIN_OVERLAY_HOTRELOAD_BUILD)

namespace goblin::input
{
// True while the F1 overlay panel is visible this frame (mirrors goblin_overlay.cpp's
// internal g_show). Input hooks use this to decide whether to blank the game's view of a
// device.
bool menu_open();

// True while the fullscreen Virtual World Map is standing in for the native map (the game map is
// open AND we open the vmap on the map key — vmap_on_map_key config, or a custom virtual world is
// active). The native map is still open underneath but fully covered, so the input hooks blank the
// game's MOUSE (cursor-pos pan + raw wheel/deltas) exactly like menu_open — but leave the KEYBOARD
// flowing so the map-close key still closes the native map (→ the vmap auto-closes; the user is
// never trapped). Defined in input_wndproc.cpp; used by the cursor + raw-input + wndproc hooks.
bool vmap_covers_map();

// > 0 while an item-search locate/page-switch is in flight (mirrors goblin_overlay.cpp's
// internal g_nav_frames) — hooks that would otherwise fully blank the game's input jitter a
// net-zero 1px instead, so the game keeps stepping its world-map camera with the panel open.
int nav_frames_active();

// F1 master open/close toggle state (mirrors goblin_overlay.cpp's internal g_user_show) — the
// keybind/gamepad-combo state, independent of g_show's per-frame foreground gate.
bool user_show();

// OS focus state, set only by real WM_SETFOCUS/WM_KILLFOCUS transitions (see
// goblin_overlay.cpp's g_has_focus declaration comment for why event-driven, not polled).
void set_has_focus(bool v);
bool has_focus();

// Mouse/keyboard vs. gamepad input-source tracking, shared between hk_wndproc (clears it on
// real mouse/kb activity) and hk_present's gamepad-switch debounce (sets it after N
// consecutive active frames) — see goblin_overlay.cpp's g_last_input_was_gamepad /
// kGamepadSwitchDebounceFrames comments.
// GOBLIN_RENDER_API: also read by the RENDER module (device-aware close-hint in draw_panel).
GOBLIN_RENDER_API bool last_input_was_gamepad();
void set_last_input_was_gamepad(bool v);
int gamepad_active_streak();
void set_gamepad_active_streak(int v);

// Set by hk_present's own cursor-recenter (SetCursorPos generates a real WM_MOUSEMOVE) so
// hk_wndproc doesn't mistake our own recenter for real mouse input and re-arm the
// gamepad-switch edge — see goblin_overlay.cpp's declaration comment for the infinite-loop
// this guards against.
bool ignore_next_mousemove_for_gamepad_flag();
void set_ignore_next_mousemove_for_gamepad_flag(bool v);
} // namespace goblin::input
