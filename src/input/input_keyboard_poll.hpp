#pragma once

// Keyboard text-input poll — fallback for dx-bugs F3 (2026-07-01): a real Alt+Tab cycle can
// permanently stop legacy WM_CHAR/WM_KEYDOWN/WM_KEYUP delivery to hk_wndproc under Wine/Proton
// (same RIDEV_NOLEGACY family as the earlier mouse-click fix, docs/memory/bugs/
// overlay-input-hook-freeze.md). Unlike mouse buttons (which the game's raw input NEVER posts
// as legacy messages, so polling was unconditionally safe), keyboard messages sometimes DO
// work (before the first Alt+Tab) — feeding both the message path and a poll would double
// characters. So this is the SOLE keyboard-text source while the menu is open: input_wndproc.cpp
// stops forwarding WM_CHAR/WM_KEYDOWN/WM_KEYUP/WM_SYSKEYDOWN/WM_SYSKEYUP to ImGui entirely, and
// this poll is the only thing that ever feeds ImGui keyboard state.

namespace goblin::input
{
// Poll GetAsyncKeyState for the keys ImGui's text/nav path needs, translate character-producing
// keys via ToUnicodeEx (respects layout/shift/capslock), and feed ImGui via
// io.AddKeyEvent/io.AddInputCharacter. Call once per frame while the menu is open (hk_present).
// No OS auto-repeat emulation (known limitation) — held keys type once per physical press.
void poll_keyboard_text_input();
} // namespace goblin::input
