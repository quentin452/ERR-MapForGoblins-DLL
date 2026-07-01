---
name: imgui_config_flags_research
description: "Checklist of ImGui IO.ConfigFlags / IO settings relevant to MapForGoblins' gamepad+mouse+keyboard coexistence, cursor, and nav friction points"
metadata:
  node_type: memory
  type: reference
---

# ImGui config-flag / IO-setting research

Backlog item 1 (`docs/memory/bugs/dx-bugs-backlog.md`, reported 2026-07-01): the dx-bugs
backlog has several friction points around gamepad+mouse+keyboard coexistence
(items 2/3/6/12, F2) that keep getting fixed one hand-rolled edge case at a time
(`g_last_input_was_gamepad`, `g_ignore_next_mousemove_for_gamepad_flag`, cursor recenter
edges). Before adding more custom state machines, check whether ImGui already has a flag
for it — this doc is the checklist so future sessions don't re-derive ImGui's IO surface
from scratch. **Not investigated in-game yet** — flip one at a time, verify, note the
result inline.

Currently set (`init_imgui`, `src/goblin_overlay.cpp`):
- `ImGuiConfigFlags_NavEnableGamepad` — widget nav via D-pad/stick (PR C-2 part 1).

## Candidates worth trying

| Flag / setting | What it does | Why it might help here |
| --- | --- | --- |
| `ImGuiConfigFlags_NavEnableKeyboard` | Tab/arrow-key widget navigation, like the gamepad flag but for keyboard | NOT currently set. If ever added, note it changes Tab/arrow behavior in text fields — verify it doesn't eat arrow-key cursor movement inside `InputTextWithHint` before enabling. |
| `ImGuiConfigFlags_NoMouseCursorChange` | Stop ImGui from calling `SetCursor()` to swap the OS cursor shape | Could matter if the OS cursor's shape/visibility state gets desynced across the alt-tab boundary (bug 2) — cheap to test in isolation. |
| `io.ConfigNavMoveSetMousePos` | When nav moves focus, also warps the OS mouse to the newly-focused widget | We hand-roll a similar "cursor should agree with what's focused" behavior via `recenter_cursor_to_window()`. If this flag does the equivalent for nav-focus changes specifically, it could replace some of that hand-rolled logic — but verify it doesn't fight our own `SetCursorPos` hook (`hk_set_cursor_pos`) the same way the current recenter loop did. |
| `io.ConfigInputTrickleEventQueue` (default true) | Spreads multiple same-frame input events across frames instead of collapsing them | Relevant if a fast alt-tab-away/back within one frame window is losing/collapsing the `WM_KILLFOCUS`/`WM_SETFOCUS` pair — worth confirming this is still on (it's ImGui's default; we don't touch it) rather than assuming. |
| `io.MouseDrawCursor` | Have ImGui draw its own software cursor instead of relying on the OS cursor | Only useful if OS-cursor confinement/visibility keeps desyncing from ImGui's idea of cursor position (ties into item 6, item 12 mouse-passthrough). Heavier hammer — try the targeted fixes first. |
| `ImGuiConfigFlags_NavNoCaptureKeyboard` | If keyboard nav is ever enabled, stop it from eating Tab for focus-cycling (lets the app's own Tab handling win) | Only relevant if `NavEnableKeyboard` above is ever turned on. |
| `ImGuiConfigFlags_NoMouse` | Fully disables ImGui's mouse handling | NOT a candidate to *set* — listed here only so nobody accidentally flips it while debugging bug 3 (mouse already becomes hard to use; this flag would make it categorically impossible, not fix it). |

## How to use this doc

Pick one row, flip the flag behind a `#if 0`/build-time toggle or a debug INI key, build,
and note the in-game result (works / no effect / makes it worse) directly in this table so
the checklist stays a record of what's actually been tried, not just what's theoretically
possible.
