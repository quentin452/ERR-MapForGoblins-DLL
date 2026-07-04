# RE brief — CSPcKeyConfig binding table (read the user's LIVE key/pad binds)

**Goal:** decode ER's keybinding config so MFG reads the player's **actual live binds** — keyboard AND
gamepad — for a given game/menu command, instead of hardcoded VK checks. Then mod hotkeys/triggers respect
the user's remapping and work on a controller, mod-agnostically. Builds directly on
`windows_input_path_re.md` (raw device layer + the singleton already identified).

Static Ghidra on `D:\ghidra_proj2\ER`, app 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`, read-only; the DLL
is in-process (Linux/Proton) and can RPM the live config + the polled pad state.

## What's ALREADY RE'd (don't redo — see `windows_input_path_re.md`)
- **Raw keyboard poll** `FUN_14074f960` (0x74f960), modifier reader `FUN_14074f880`.
- **Gamepad (XInput)** `FUN_141f29010` polls pads 0..3 → live `XINPUT_STATE` at **`padObj+0x48`**, per-pad
  table **`DAT_1430b92e0`** (stride 0x10). ⇒ the mod can already READ the live pad state the engine polls.
- **`CSPcKeyConfig` singleton = `DAT_143d5deb8`** (ctor `FUN_1402438f0`→`FUN_1402429d0`; sibling
  `DAT_143d5dec0`) — *"maps physical input → virtual game/menu commands"*. **Identified, NOT decoded.**
- Binding params: `KeyAssignParam_TypeA/B/C`, `KeyAssignMenuItemParam`, `DefaultKeyAssign`,
  `Game.DefaultKeyAssignType` (@0x142bed910).

## What MFG does today (the gap)
- Overlay toggle = `GetAsyncKeyState(config::overlayToggleKey)` — a hardcoded/ini VK, **keyboard-only,
  remap-blind, no pad** (`goblin_overlay.cpp:1345`). The vmap OPEN is already fine (gated on
  `world_map_open()` = the game's real menu state, so remap/pad-agnostic) — the gaps are (a) navigating the
  overlay/vmap with a **gamepad**, (b) any mod hotkey being kb-only + remap-blind, (c) triggering on an ER
  action by its **bound** input.

## What we need (priority order)
1. **CSPcKeyConfig layout.** From `*(DAT_143d5deb8)`, the structure that maps a **command → binding**. How are
   commands indexed (array by command id? a map?), and what's the binding record — `{keyboard VK/scancode,
   mouse button, gamepad button}`? Give the offsets to walk `singleton → command[id] → {kb, pad}`.
2. **Command-id enum.** The command ids MFG needs first: **Open Map** (+ the map-layer/OW↔UG toggle), menu
   **Confirm / Cancel**, **DPad/stick nav** (Up/Down/Left/Right), **Menu/Options**. These come from the
   `KeyAssign*` param rows (each row = a command with its default binds) — give at least the **Open Map** id
   and how the id space is organized (game vs menu command tables — `TypeA/B/C`).
3. **Gamepad button encoding.** How CSPcKeyConfig stores the pad button (an ER-internal button enum vs a raw
   `XINPUT_GAMEPAD.wButtons` mask), and the mapping to what to watch in the polled state
   (`DAT_1430b92e0` / `padObj+0x48`) — so a decoded bind can be tested against the live pad.
4. **Live read confirm.** `CSPcKeyConfig = *(DAT_143d5deb8) → command → {kb, pad}` must reflect **live
   remaps** (change a bind in ER's Key Config / Controller options → the read updates). Confirm the config is
   the LIVE one, not a boot snapshot.

## Constraints
- **Read-only, mod-agnostic** — read the ACTIVE install's live config (respects the user's remap + control
  scheme), no bake.
- **Off-thread safe** — a pure RPM read of the config struct + the polled pad state (no engine call), so the
  present/RPC thread can use it (same discipline as the player-pos probes).

## Cheap intermediate (flag it — needs NO bind decode)
Gamepad **navigation** of the overlay/vmap needs only the already-RE'd live pad state: feed
`DAT_1430b92e0`/`padObj+0x48` `XINPUT_STATE` into **ImGui's built-in gamepad nav**
(`ImGuiConfigFlags_NavEnableGamepad` + `io.AddKeyEvent(ImGuiKey_GamepadDpad*/FaceButton*, …)`). Reads the pad
the engine already reads → no conflict, works today. So a pad player can navigate the mod UI **before** this
RE lands; the RE adds *remap-aware binds* (open on the user's key/button, trigger on ER actions).

## Leads / anchors
```
CSPcKeyConfig singleton   DAT_143d5deb8   (*deref → command→binding table; sibling DAT_143d5dec0)
ctor                      FUN_1402438f0 → FUN_1402429d0   (named in FUN_140080a40)
bind params               KeyAssignParam_TypeA/B/C, KeyAssignMenuItemParam, DefaultKeyAssign
kb poll                   FUN_14074f960 (0x74f960)   modifiers FUN_14074f880
pad poll (live state)     FUN_141f29010 → padObj+0x48 / DAT_1430b92e0 (per-pad XINPUT_STATE, stride 0x10)
```

## Deliverable
A live **bind reader** — `binding_for(command_id) → {vk, pad_button}` reading `*(DAT_143d5deb8)` — plus the
command ids MFG needs (Open Map first) and the pad-button encoding. Enough for the DLL to (a) fire mod
features on the user's ACTUAL bind (kb or pad), and (b) drive ImGui gamepad nav from the decoded/polled pad.
Findings → `docs/re/windows_keybinding_config_re_findings.md`.
