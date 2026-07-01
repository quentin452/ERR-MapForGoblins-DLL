# Input hooks now live under `src/input/`

`docs/plans/input_module_refactor_plan.md` — DONE 2026-07-01, branch `feat/input-module`. All 5
input hook groups moved out of `goblin_overlay.cpp` into their own files, one per device class.
`goblin_overlay.cpp` keeps only `hk_present` (per-frame consumer of most of the shared state
below) and the ImGui panel/render code.

## File map

| File | Hooks | Install call (from `goblin::overlay::initialize()`) |
|------|-------|-------------------------------------------------------|
| `src/input/input_directinput.cpp` | `hk_di_get_device_state`/`hk_di_get_device_data` | `install_directinput_hooks()` |
| `src/input/input_gamepad.cpp` | `hk_xinput_get_state` | `install_xinput_hook()` |
| `src/input/input_cursor.cpp` | `hk_set_cursor_pos`/`hk_clip_cursor`/`hk_get_cursor_pos` | `install_cursor_hooks()` |
| `src/input/input_rawinput.cpp` | `hk_get_raw_input_data`/`hk_get_raw_input_buffer` | `install_rawinput_hooks()` |
| `src/input/input_wndproc.cpp` | `hk_wndproc` (subclassed via `SetWindowLongPtrW`, not MinHook) | `install_wndproc_hook(hwnd)` / `uninstall_wndproc_hook(hwnd)` |
| `src/input/input_shared.hpp` | no hooks — thin accessors for state that crosses the hook/`hk_present` boundary | — |

## Where the next diagnostic counter goes

Each module already owns its hook-internal diagnostic atomics and exposes `diag_*_exchange()` (or
`_load()` for non-resetting reads) accessors — e.g. `input_cursor.cpp`'s
`diag_set_cursor_pos_exchange()`, `input_wndproc.cpp`'s `diag_wm_char_exchange()`. To add a new
counter for a bug in one of these hooks: add the atomic + `fetch_add` inside that hook's own
`.cpp`, expose one more accessor in its `.hpp`, call it from wherever `goblin_overlay.cpp` builds
the `[XXXDIAG]` log line (still centralized in `hk_present`, since that's where the periodic
1/sec dump lives). No need to touch any OTHER input file.

## Why some state didn't move (`input_shared.hpp`)

The scope audit before this refactor (see the plan's "Scope audit" section) found `hk_present`
reads/writes nearly every cross-cutting flag the hooks also touch — `g_show`, `g_user_show`,
`g_has_focus`, `g_last_input_was_gamepad`, `g_gamepad_active_streak`,
`g_ignore_next_mousemove_for_gamepad_flag`, `g_nav_frames`. Moving `hk_present` itself was
explicitly out of scope (it's ~500 lines of gamepad-switch/recenter/diag-overlay behavior, not a
raw hook), so these globals **stay defined in `goblin_overlay.cpp`** and `input_shared.hpp`
exposes get/set accessor functions instead. This was "option (a)" from the plan's revised
recommendation — cheaper and lower-risk than also absorbing `hk_present`'s input-reactive slice
into the input module ("option (b)", not started, would be the natural next step if this area
needs touching again).

## Pattern for future hook edits

Every accessor added during this refactor is a direct passthrough — no logic changed, only where
the code physically lives. If you need to touch hook behavior (not just add a counter), edit the
hook's own file directly; if the change needs new shared state `hk_present` must also read/write,
add the accessor pair to `input_shared.hpp` and implement it in `goblin_overlay.cpp` next to
`menu_open()`/`nav_frames_active()` (same file, same pattern, easy to find — grep
`goblin::input::` in `goblin_overlay.cpp`).
