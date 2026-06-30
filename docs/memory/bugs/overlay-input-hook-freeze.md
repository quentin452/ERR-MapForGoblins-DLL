---
name: overlay-input-hook-freeze
description: "Input-API detours run on the GAME thread — a blocking/looping detour hangs the game while the overlay (present thread) keeps rendering; observed as 'game frozen, DLL alive'."
metadata:
  node_type: memory
  type: project
---

# Overlay input-hook freeze — "game frozen, DLL alive"

**Observation (2026-06-30, Proton 11, exclusive Fullscreen).** ER froze (no input, no progress) while the
MapForGoblins overlay kept rendering normally — even at the main menu. A hung game with a live overlay.

**Cause.** The reverted ShowCursor build (`63f7bd7a`) tried to force the OS cursor visible while the F1
panel was open by swallowing the game's `ShowCursor(FALSE)` with a constant `return 1`. ER hides the
cursor with a loop — `while (ShowCursor(FALSE) >= 0);` — so a non-negative return makes that loop spin
forever. That loop runs on **ER's main/update thread**, but our overlay draws from the **DXGI Present /
render thread**, which kept going → the overlay looked alive while the game was wedged. Reverted to
`ff01dddf`; the freeze stopped.

**Guardrail (the durable lesson).** Every input-API detour we install — `ShowCursor`, `SetCursorPos`,
`ClipCursor`, `GetCursorPos`, `GetRawInputData/Buffer`, win32u `NtUser*` mirrors — executes **on whatever
game thread called it**, usually the main thread. A detour MUST:
- never loop/block/sleep, and never make a blocking cross-thread or wineserver round-trip;
- never return a value that traps a known game spin-loop (e.g. a constant `>= 0` from `ShowCursor`, or a
  value that makes a "retry until X" caller never satisfy X);
- keep the fast path to `return original(...)`.
When a game freezes but the overlay still renders, suspect one of OUR input detours blocking the game
thread before suspecting the engine.

**Followup / open watch.** The ShowCursor approach is gone, so this specific freeze is fixed. STILL TO
CONFIRM: that no freeze recurs in exclusive **Fullscreen** now (we run Borderless + button-polling, see
[[overlay-render-perf-followups]] is unrelated; the cursor saga is `docs/re/proton11_cursor_lock_re_prompt.md`).
If a Fullscreen freeze ever recurs WITHOUT any blocking detour in the build, investigate a separate
exclusive-fullscreen ↔ Present-hook deadlock (DXGI fullscreen mode-switch while we hold the present path).
General Wine guidance regardless: run ER **Borderless Windowed**, not exclusive Fullscreen.
