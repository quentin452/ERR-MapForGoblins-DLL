#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// WndProc hook (input capture) — extracted from goblin_overlay.cpp (see
// docs/plans/input_module_refactor_plan.md, slice 5, the last and biggest one). Unlike
// MinHook-based hooks, this subclasses the window via SetWindowLongPtrW(GWLP_WNDPROC), so
// install/uninstall need the live HWND instead of an install-time-only lookup.

namespace goblin::input
{
// Subclass hwnd's WndProc. Call once, after the window is known (from init_imgui()).
void install_wndproc_hook(HWND hwnd);

// Restore the original WndProc. Call once on shutdown, before the window is destroyed.
void uninstall_wndproc_hook(HWND hwnd);

// Diagnostic counters, read-and-reset by goblin_overlay.cpp's [KBDIAG]/[CLICKDIAG] dumps.
unsigned diag_wm_char_exchange();
unsigned diag_wm_keydown_exchange();
unsigned diag_wndproc_lbdown_while_open_load();
} // namespace goblin::input
