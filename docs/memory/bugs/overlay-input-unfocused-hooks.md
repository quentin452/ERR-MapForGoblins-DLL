---
name: overlay-input-unfocused-hooks
description: "Input-suppression hooks (cursor/raw-input/wndproc swallow) kept commandeering input while the game was backgrounded with F1 open — re-gated on OS focus without regressing the 2026-07-01 Alt+Tab cursor fix."
metadata:
  node_type: memory
  type: project
---

# Overlay input hooks fired while the game window was NOT focused

**Symptom (2026-07-23).** With the F1 panel open, alt-tabbing to another app left our input hooks active:
the cursor SetCursorPos/ClipCursor/GetCursorPos detours + raw-input blanking kept freezing/freeing the OS
cursor and swallowing input meant for the other window. "imgui input hooks fire even when window not focused."

**Root cause — a documented tradeoff, not an oversight.** On 2026-07-01 the `&& fg` focus gate was
**deliberately dropped** from `g_show` (`goblin_overlay.cpp`, the `g_show = g_user_show` assignment) to kill
a whole class of Alt+Tab cursor bugs (cursor stuck at centre, MousePos permanently invalid,
WantCaptureMouse never recovering) that stemmed from state getting invalidated on the focus-loss/regain
*transition*. The comment there literally predicted this bug: F1 stays fully active (drawing **and input
capture**) while backgrounded, so "our input-swallow hooks … will still be active and could interfere with
that other window."

**Fix.** Decouple drawing from input capture. `g_show` (drawing / ImGui visibility) stays
focus-INdependent — so ImGui's focus state is still never invalidated on the transition — but the INPUT
path is re-gated on OS focus:
- New header-only predicate `goblin::input::input_capture_active()` in `src/input/input_shared.hpp` =
  `(menu_open() || vmap_covers_map()) && has_focus()`.
- Cursor hooks (`input_cursor.cpp` ×3) and raw-input hooks (`input_rawinput.cpp` ×2) gate on it.
- The three wndproc swallow branches (`input_wndproc.cpp`) gained `has_focus() &&` (their conditions differ,
  so the predicate doesn't fit uniformly). The WM_SETFOCUS/KILLFOCUS forwarding + gamepad-state tracking +
  the `CallWindowProcW` passthrough all still run when unfocused.

**Why it doesn't regress the 2026-07-01 fix.** `has_focus()` is driven by real `WM_SETFOCUS`/`WM_KILLFOCUS`
messages (event-driven, stable), NOT the old per-frame `GetForegroundWindow()` poll that flapped
true/false several times during a single Alt+Tab under Wine. The F1 toggle at `goblin_overlay.cpp:1366`
already gates on `g_has_focus` and works in-game, so it's a proven-reliable signal. `g_has_focus` inits
`true` (focused-at-load). Only the backgrounded case changes; focused behavior is byte-identical.

Related: [[overlay-input-hook-freeze]] (detours run on the game thread — never block), [[dx-bugs-backlog]].
