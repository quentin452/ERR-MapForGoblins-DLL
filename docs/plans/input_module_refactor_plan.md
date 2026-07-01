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

## Scope audit (2026-07-01)

Real line ranges (today, `src/goblin_overlay.cpp`, 4441 lines total) — smaller than the "4700+ line
file" framing above suggested, and `hk_wndproc` itself is modest; the bulk of the file between it
and `hk_present` (`:1718-3568`, ~1850 lines) is ImGui panel/UI drawing code, not hooks:

| Hook | Lines | Size |
|------|-------|------|
| `hk_set_cursor_pos` | 1393-1406 | ~14 |
| `hk_clip_cursor` | 1407-1425 | ~19 |
| `hk_xinput_get_state` | 1426-1439 | ~14 |
| `hk_get_cursor_pos` | 1440-1468 | ~29 |
| `hk_get_raw_input_data` | 1469-1515 | ~47 |
| `hk_get_raw_input_buffer` | 1516-1552 | ~37 |
| `hk_di_get_device_state` | 1553-1560 | ~8 |
| `hk_di_get_device_data` | 1561-1571 | ~11 |
| `hk_wndproc` | 1572-1717 | ~145 |

Install sites: **two places**, not scattered — the main `MH_CreateHook`/`hook_u32` block
(`goblin_overlay.cpp:4242-4304`) wires `SetCursorPos`/`ClipCursor`/`GetRawInputData`/
`GetRawInputBuffer`/`GetCursorPos`/`XInputGetState`/DirectInput; `hk_wndproc` is subclassed
separately via `SetWindowLongPtrW(..., GWLP_WNDPROC, ...)` inside `init_imgui()` (`:1861`).

**Headline finding: the 5 hooks themselves are cheap to move (self-contained, ~250 lines total,
all their state — `g_diag_*`, `g_virtual_cursor_*`, `g_imgui_reading_cursor` — is confined to this
file, zero outside `.cpp`/`.hpp` references except one comment in `map_renderer.cpp:1112`). The
real cost isn't the hooks — it's `hk_present` (`:3569-4069`, ~500 lines, explicitly EXCLUDED from
this pass), which reads/writes the SAME globals the hooks own:**

- `g_has_focus`, `g_last_input_was_gamepad`, `g_gamepad_active_streak`,
  `g_ignore_next_mousemove_for_gamepad_flag`, `g_gamepad_combo_recording/ready`,
  `g_toggle_kb_streak/armed`, `g_toggle_gamepad_streak/armed`, `g_nav_frames`,
  `g_imgui_reading_cursor`, `g_diag_raw_cursor_client/valid`, `g_xinput_available`, `g_show`,
  `g_user_show` — all written by `hk_wndproc`/`hk_xinput_get_state` and ALSO read/written by
  `hk_present`'s per-frame gamepad-poll/recenter/diag-overlay logic. None of these are hook-internal
  — every one crosses the hook/`hk_present` boundary in at least one direction (full per-variable
  read-site list gathered via grep, not reproduced here — see `git log` of this edit for the raw
  audit if needed).
- Cross-hook coupling found: `hk_get_raw_input_data`/`hk_get_raw_input_buffer` both call
  `accumulate_virtual_cursor()` (`:293`, file-local helper) which feeds `g_virtual_cursor_x/y`,
  consumed later by `hk_present`'s diagnostic draw (`:4013-4014`) — so even the "isolated" raw-input
  hooks feed forward into `hk_present`, not just backward into config.
- `hk_wndproc` also calls OUT of the input hook group into other modules already — `ImGui_ImplWin32_
  WndProcHandler` (expected, ImGui backend) and `goblin::world_map_open()` /
  `goblin::worldmap::inworld_hovered()` (a real dependency on the worldmap module — the input module
  will need to depend on worldmap query functions, not just the reverse).
- `hk_xinput_get_state` already depends on `goblin::self_module_range()` from
  `goblin_crashdump.cpp`/`.hpp` — confirms cross-TU dependencies for hooks are already normal in
  this codebase (not a new pattern this refactor introduces).

**Revised recommendation:** the plan's mouse+keyboard-first move order is still fine (those are
the 2 hooks under active investigation), but **`hk_present` cannot stay untouched** the way the
"Excluded from this pass" line implies — at minimum it needs `extern`/accessor-level access to every
global in the list above once those globals move into `src/input/*.cpp`. Two options, pick when
work starts:
  (a) keep the globals themselves in a small shared header (`input_state.hpp`, no `.cpp`) that both
      `src/input/*.cpp` and `goblin_overlay.cpp`'s `hk_present` include — cheapest, matches the "just
      move the hook bodies" framing;
  (b) also move `hk_present`'s input-reactive slice (gamepad-switch detection, recenter-on-pad-switch,
      toggle debounce) into the input module and have `hk_present` call one `input::poll_frame()`
      — more invasive but actually finishes the "one place per device class" goal, since right now
      HALF of the gamepad-toggle/debounce logic lives in `hk_present`, not in `hk_xinput_get_state`.
  Recommend (a) for the first pass (lower risk, matches "move hook bodies only"), note (b) as a
  possible follow-up once (a) is stable and diagnosable.

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
