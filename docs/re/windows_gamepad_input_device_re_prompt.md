# RE brief — the GAMEPAD "active input device" flag (world-map fix)

**App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. Cheat-Engine + running game + Ghidra
(`D:\ghidra_proj2\ER`).** Build/deploy toolchain (no MSVC on box): clang-cl + xwin + ninja —
see memory `build-toolchain-clang-xwin`. Crash handler now logs a stack trace into the `.txt`.

## The bug (user-confirmed, gamepad only)
With a **gamepad**, the open world map misbehaves: it pans/drifts on its own and the overlay
markers are "jamais centré". **The ONLY thing that fixes it is a real hardware mouse/touchpad
MOVEMENT** (not a click, not a synthetic event). Once a real mouse move happens, it's fixed for
that map session. Moving the gamepad stick afterwards is fine.

## Root cause (established this session)
ER tracks an **"active input device"** state. A real mouse MOVEMENT (via **Raw Input / the HID
path**) flips it to "mouse", which is the state the world-map cursor/pan/overlay needs. In
"gamepad" state the map drifts (see the separate finding: the map pans on the
`GetCursorPos − screenCentre` delta, and on gamepad the OS cursor is frozen off-centre).

**Why nothing we injected worked** — the flip requires *genuine* hardware mouse motion:
- `SendInput` mouse move → flagged `LLMHF_INJECTED` → ER's real-input path ignores it.
- Modifying the game's own `IDirectInputDevice8::GetDeviceState` return (the mod already hooks
  it) → did NOT flip it either → the switch is NOT driven by that DInput mouse read.
- Returning screen-centre from `GetCursorPos` (position, not movement) → no flip (it's the
  *movement event*, not the position).

## What's needed (deliverable)
Find the **"active input device" flag/field** that a real mouse move sets, and a **stable way to
write it** from the mod (a memory WRITE is NOT filtered, unlike injected input). Then the mod sets
it to "mouse" on map-open while a gamepad is connected → reproduces the workaround for real.
Confirm: open map on gamepad → markers/ pan correct WITHOUT touching the mouse.

## Leads / what's already mapped
- **Mouse poller (GetCursorPos path):** `FUN_140e1e940` (RVA `0xe1e940`) — does `GetCursorPos`,
  compares to prev `obj+0x28/+0x2c`, sets `obj+0x30 = mouse-moved`, `obj+0x31 = in-window`.
  Caller `FUN_140e1e500` (periodic input poll) ← `FUN_140e33aa0`. NOTE this is the GetCursorPos
  delta path — SendInput *does* change GetCursorPos, yet synthetic doesn't fix → the device
  switch is NOT this `obj+0x30`; it's the **Raw Input / HID** path. Find that one.
- **Raw input:** `GetRawInputData` is NOT resolvable by name in the listing (dynamic/VMProtect).
  Find ER's WM_INPUT/RAWMOUSE consumer another way (xref the import thunk, or AOB on
  `RIM_TYPEMOUSE`/`usFlags`), then the device-type global it sets on `lLastX|lLastY != 0`.
- **Gamepad path:** `FUN_141f29010` (XInputGetState loop, pad table `DAT_1430b92e0` stride 0x10)
  → per-pad state `FUN_141f6bad0`. Look for the global it sets that the mouse path also sets to a
  different value = the shared "active device" enum.
- **CSPcKeyConfig singleton `DAT_143d5deb8`** (input config) — candidate owner of the device flag.
- Input-enable gate used by the world-map cursor tick: `FUN_140758050` reads CSMenuMan
  (`DAT_143d6b7b0`) `+0x19/+0x1a/+0x798` — input-mode-ish bytes; check if one is the device flag.

## FASTEST path = runtime CE memory-diff (the flag flips on mouse-move and STAYS)
1. Gamepad only, open map (bug). CE: First Scan → **Unknown initial value**, type **Byte**.
2. Move the real mouse ONCE (bug fixed), then don't touch it. CE: Next Scan → **Changed value**.
3. Use only the gamepad. CE: Next Scan → **Unchanged value** ×3-4 (drop gamepad noise).
4. Close/reopen the map on gamepad (bug back). CE: Next Scan → **Changed value**.
5. Repeat open(bug)/mouse-move(fix) with Changed/Unchanged → narrow to the flag (a byte/int that
   is value A in gamepad-state, value B after a real mouse move).
6. Capture its address + A/B values + a stable pointer path → bake a write in the mod
   (`goblin_overlay.cpp` map-open + XInput-connected gate), or hand it to the agent to wire.

## Done this session (shipped, committed)
- **clang-cl SEH-elision crash fixed** (THE crash): clang-cl drops `__try/__except` around raw
  loads/stores → converted every hot raw mem access to RPM/WPM (kindling, markers, collected,
  debug_events, probe). Crash handler now logs a stack trace.
- Probe: bounded `enumerate_menu_cursors` (replaced the OOB whole-RAM scan); confirmed there is
  only ONE world-map cursor (no separate gamepad cursor instance).
- `project_uv` view-centre `(pan+snapMid)/zoom` left behind the `Y` toggle (default = reticle);
  not the fix for this bug (this is an input-device-state bug, not a projection bug).
