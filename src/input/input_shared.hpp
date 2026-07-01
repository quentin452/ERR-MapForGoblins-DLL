#pragma once

// Small cross-cutting accessors shared between goblin_overlay.cpp (which owns the actual
// g_show/g_user_show/... state) and the input hook modules under src/input/. Kept to
// read-only accessors deliberately — the state itself stays defined in goblin_overlay.cpp
// (its ~50 existing call sites are unchanged), this just gives the split-out hooks a way to
// read it without duplicating or renaming it. See docs/plans/input_module_refactor_plan.md.

namespace goblin::input
{
// True while the F1 overlay panel is visible this frame (mirrors goblin_overlay.cpp's
// internal g_show). Input hooks use this to decide whether to blank the game's view of a
// device.
bool menu_open();

// > 0 while an item-search locate/page-switch is in flight (mirrors goblin_overlay.cpp's
// internal g_nav_frames) — hooks that would otherwise fully blank the game's input jitter a
// net-zero 1px instead, so the game keeps stepping its world-map camera with the panel open.
int nav_frames_active();
} // namespace goblin::input
