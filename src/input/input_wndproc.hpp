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
// Monotonic WM_KEYDOWN/WM_SYSKEYDOWN arrival count (never reset) — RPC key-delivery verify.
unsigned wm_keydown_total();
unsigned diag_wndproc_lbdown_while_open_load();

// RPC auto-idle (2026-07-03). The debug RPC calls mark_rpc_injection() right before each of its
// own SendInput calls so hk_wndproc doesn't count that echo as genuine user activity; the guard
// window covers the async WM delivery tail. ms_since_user_input() returns the age (ms) of the
// last real kb/mouse activity — the RPC gate/status use it to suspend input injection while the
// human is driving. Returns ~0ull (huge) when no user input has been seen yet.
void mark_rpc_injection(unsigned ms);
unsigned long long ms_since_user_input();
} // namespace goblin::input
