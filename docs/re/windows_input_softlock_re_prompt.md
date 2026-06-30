# Windows RE prompt — map-exit input softlock + F1 mouse-dead

Status: OPEN. Needs runtime RE on Windows (Cheat Engine / x64dbg / Ghidra against a running game).
Linux/Proton can read code but cannot confirm which input path ELDEN RING latches.

## Symptom (user repro, 2026-06-30, ERRv2.2.9.6)

1. Hold a movement key (e.g. Z) **while the world map is open** (F1 overlay panel likely also open).
2. Exit the map.
3. Result: player can be **soft-locked into permanent movement "à vie"** (game thinks the key is
   still held), AND the **ImGui F1 panel stops responding to mouse input** afterwards.

## What is already ruled out (static analysis, src/goblin_overlay.cpp)

- **WndProc is NOT the culprit for the stuck key.** `hk_wndproc` (line ~1287) explicitly passes
  `WM_KEYUP` / `WM_SYSKEYUP` through to the game in every branch (F1-open ~1300-1306, map-open ~1326,
  both-closed ~1341). There is even a comment about the "held forever" bug. So the key-release message
  does reach the original WndProc.
- Marker rebuild / GPU release on close is NOT involved (that was the close-LAG bug, instrumented
  separately as `render.minimap`, commit d792a3a).

## Primary hypothesis — DirectInput buffered key-UP drop

ELDEN RING reads movement via **DirectInput**, not WndProc. Two DI hooks blank input while the F1
panel is up (`g_show == true`):

- `hk_di_get_device_state` (src/goblin_overlay.cpp ~1272) — zeroes the immediate DI device state.
- `hk_di_get_device_data` (src/goblin_overlay.cpp ~1281) — sets `*inout = 0` (drops the buffered
  event count) so the game reads **zero buffered transitions**.

Buffered DI delivers discrete DOWN/UP transition events. If a key's **DOWN** transition was consumed
by the game before/while the panel state changed, but its **UP** transition lands in a frame where
`hk_di_get_device_data` blanks the buffer, the game **never sees the release** → its internal movement
latch stays down → permanent movement. The WndProc keyup-passthrough does not save it because the
game's movement input comes from DI, not WndProc.

## Secondary hypothesis — no map-close cleanup edge (the F1 mouse-dead half)

The overlay has a falling-edge handler **only for F1 close** (Present hook, src/goblin_overlay.cpp
~3025-3037: re-clip cursor on `s_prev_show && !g_show`). There is **no falling edge for
`world_map_open()` transitions**, and **zero `io.ClearInputKeys()` / `io.AddKeyEvent` resets anywhere**.
So if the map closes while F1 is open, ImGui input/cursor capture state is never reconciled →
subsequent F1 mouse routing can be left inconsistent (mouse-dead).

`world_map_open()` = `CSMenuMan + 0xCD == 7` (src/goblin_inject.cpp ~1716-1721).
F1 state `g_show` is set from `g_user_show && foreground` (src/goblin_overlay.cpp ~3008).

## Windows verification steps

1. **Confirm the input path.** With CE/x64dbg, break on the game's movement read. Determine whether
   ELDEN RING movement comes from `IDirectInputDevice8::GetDeviceState`, `GetDeviceData` (buffered),
   GetAsyncKeyState, or WM_*. This decides whether the DI hooks are even on the movement path.
2. **Reproduce + watch the latch.** Repro the softlock, then inspect the player/input struct for the
   stuck "key down" flag. Find the address the game uses as the movement-held latch; confirm it stays
   set after physical release.
3. **Test the DI-drop theory.** Temporarily make `hk_di_get_device_data` pass UP transitions through
   (or never blank while a movement key is physically down) and check whether the softlock disappears.
4. **Test the cleanup-edge theory (mouse-dead half).** Add a `world_map_open()` falling-edge handler
   that calls `ImGui::GetIO().ClearInputKeys()` + re-clips the cursor, and check whether F1 mouse
   routing recovers after the map-exit sequence.

## Candidate fix directions (do NOT ship un-verified on Linux)

- DI hook: on the `g_show` (and/or map-open) **falling edge**, flush/forward any pending key-UP
  transitions, or stop blanking buffered UP events for keys the game still believes are held.
- Add a `world_map_open()` falling-edge handler mirroring the F1-close handler: `ClearInputKeys()`,
  re-clip cursor, reconcile `MouseDrawCursor`.
- Consider gating DI blanking on `g_show && foreground` only, and explicitly synthesizing balanced
  UP events on the closing edge.

## Pointers

- src/goblin_overlay.cpp ~1175-1193 — cursor hooks (`hk_set_cursor_pos`, `hk_clip_cursor`)
- src/goblin_overlay.cpp ~1272-1284 — DI hooks (state + buffered data blanking)
- src/goblin_overlay.cpp ~1287-1342 — `hk_wndproc` message disposition
- src/goblin_overlay.cpp ~3004-3037, ~3075 — F1 falling-edge cursor restore + `MouseDrawCursor`
- src/goblin_inject.cpp ~1716-1721 — `world_map_open()`
