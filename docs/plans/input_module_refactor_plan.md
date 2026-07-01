# Plan — extract input hooks out of `goblin_overlay.cpp` into a dedicated input module

Status: scoped, not started. Fork an implementation branch from `master` when work actually starts
(per the "plans live on master" policy) — do not keep a plan-only branch.

## Why (motivation — real bug, not speculative cleanup)

2026-07-01: user reported search-bar text fields stopped accepting keyboard input. Log evidence
(`docs/memory/bugs/dx-bugs-backlog.md` — search "wm_char/sec=0"): `[KBDIAG]` showed
`WantCaptureKeyboard=true WantTextInput=true` (ImGui confirms a field is focused) but
`wm_char/sec=0 wm_keydown/sec=0` for 13+ straight seconds, no Alt+Tab in the window. The counters
that proved this (`g_diag_wm_char`/`g_diag_wm_keydown`) already exist, but adding them required
editing `hk_wndproc` in the middle of a 4700+ line file that also owns rendering, cursor-lock,
gamepad isolation, and ImGui setup — every previous input bug this fork has chased (Alt+Tab
cursor-lock, gamepad-toggle debounce, Proton click-swallow, this keyboard bug) needed a NEW ad-hoc
counter added inline, by hand, in that same file, each time.

**Goal:** move the actual input HOOKS (not necessarily all consumers) into their own small
translation units under `src/input/`, one per device class, each already carrying its own counters
— so the NEXT input bug gets a `[XXXDIAG]` log line by reading an existing counter instead of
threading a new atomic through `goblin_overlay.cpp` again.

## Scope — hooks to move (all currently in `goblin_overlay.cpp`)

| Hook | Current line | Device class |
|------|-------------|--------------|
| `hk_wndproc` | `:1572` | mouse+keyboard window messages (WM_*) |
| `hk_get_cursor_pos` / `hk_set_cursor_pos` / `hk_clip_cursor` | `:1440` / `:1393` / `:1407` | cursor position/clip |
| `hk_get_raw_input_data` / `hk_get_raw_input_buffer` | `:1469` / `:1516` | raw input (mouse + keyboard) |
| `hk_di_get_device_state` / `hk_di_get_device_data` | `:1553` / `:1561` | DirectInput8 |
| `hk_xinput_get_state` | `:1426` | XInput / gamepad |

Excluded from this pass (stays in `goblin_overlay.cpp`, just a *consumer* of whatever the input
module exposes): `hk_present` (per-frame gamepad poll driving `g_user_show`/recenter — behavioral
logic, not a raw hook), ImGui setup/render, ImGui_ImplWin32 calls themselves.

## Design sketch (finalize when work starts — not locked in)

- One `.cpp`/`.hpp` pair per device class under `src/input/`: `input_mouse.cpp`, `input_keyboard.cpp`,
  `input_gamepad.cpp`, `input_rawinput.cpp` (or fewer files if a device class is trivial — don't
  force a 1:1 split if e.g. DirectInput is 10 lines).
- Each unit owns its own hook state (the `o_*` original-fn pointers, its diagnostic atomics) and
  exposes a small init/install function called once from `goblin_overlay.cpp`'s existing hook-install
  site — mirrors how `goblin_worldmap_probe.cpp` / `goblin_crashdump.cpp` are already separate units
  consumed by `goblin_overlay.cpp`, so this is consistent with the existing pattern, not a new one.
- Shared cross-cutting state (`g_show`, `g_user_show`, `g_has_focus`) stays wherever it already is
  (probably a shared header) — this refactor is about the HOOK BODIES, not re-owning that state.
- Diagnostic counters (`g_diag_wm_char`, `g_diag_wm_keydown`, `g_diag_get_raw_input_data`, etc.)
  move with their owning hook; each unit should expose a uniform "dump counters" call so a future
  `[XXXDIAG]` log line is one function call, not hand-threading atomics again.

## Explicit non-goals

- Not fixing the keyboard bug itself in this plan — that's tracked separately
  (`docs/memory/bugs/dx-bugs-backlog.md`, KBDIAG follow-up). This refactor makes the NEXT diagnostic
  pass cheaper; it is not required to fix the current bug (which can also be diagnosed inline if
  urgent — don't block the bugfix on this landing first).
- Not touching rendering, gamepad nav-isolation policy, or focus-tracking logic — pure move +
  light restructuring of hook registration, same behavior.

## Suggested order

1. Land the keyboard-bug diagnostic (raw-keyboard-event counter, per the in-progress investigation)
   inline first if the bug is still being chased — don't block user-facing debugging on this
   refactor landing.
2. Fork `feat/input-module` from `master` when ready to start.
3. Move one device class at a time (start with mouse+keyboard, the two currently under
   investigation), build+deploy+smoke-test after each move before starting the next — this file has
   burned multiple sessions on subtle input regressions, don't batch all 5 hook groups into one
   untested commit.
4. Update `docs/memory/features/` (or a new `docs/memory/tooling/input-hooks.md`) with the new file
   map once landed, so future sessions know where to add the next counter.
