# Input path — eldenring.exe (app 2.6.2.0), imagebase 0x140000000

RE'd from `D:\ghidra_proj2\ER` (scripts `find_input.java`..`find_input3.java`, `D:\ghidra_scripts`).
Goal: the device→command path, so the mod can watch the surface/underground map-toggle input
(the OW/UG state has no readable native field — see `windows_open_map_region_re_prompt.md` /
`ghidra-worldmap-re` memory; the only viable detection is watching the toggle input itself).

## Raw device layer

**Keyboard** (`GetKeyState` / `GetKeyboardState`, imported):
- `FUN_14074f880` (RVA 0x74f880) — modifier reader: Shift `VK 0x10`, Ctrl `0x11`, Alt `0x12`,
  NumLock `0x90`, CapsLock `0x14`, ScrollLock `0x91` → returns a modifier bitmask.
- `FUN_14074fe00` (0x74fe00) — `GetKeyboardState` reader.
- `FUN_14074f960` (0x74f960, 1174 B) — main keyboard poll (singleton-gated on `DAT_143d83148`).

**Gamepad (XInput)** — `XInputGetState` / `XInputSetState` imported:
- `FUN_141f29010` (0x1f29010) — per-pad poll: loops pads 0..3, `XInputGetState(i, &state)`, on success
  stores via `FUN_141f2a4e0(padObj+0x48, &DAT_1430b92e0 + i*0x10)`. So the **live pad state** lands at
  `padObj+0x48` (prev frame copied `+0x50`→`+0x58`). `DAT_1430b92e0` = per-pad XINPUT_STATE table (stride 0x10).
- aggregator `FUN_141f6bad0` (0x1f6bad0, 2906 B).

**DirectInput** — `DirectInput8Create` imported (legacy pad / fallback; "Failed to initialize DirectInput"
@ 0x1430b93c8, init `FUN_141f28260`).

## Key config / binding layer

- **`CSPcKeyConfig` singleton = `DAT_143d5deb8`** (created by `FUN_1402438f0`→`FUN_1402429d0`; sibling
  `DAT_143d5dec0`; named in `FUN_140080a40`). This maps physical input → virtual game/menu commands.
- Binding params: `KeyAssignParam_TypeA/B/C`, `KeyAssignMenuItemParam`, `DefaultKeyAssign`,
  `Game.DefaultKeyAssignType` (@ 0x142bed910).

## → Menu command → dialog (the wall)

Physical input → `CSPcKeyConfig` → a virtual menu command → the active dialog's handler. The world-map
layer switch (`WorldMapSwitchDialog`, see region doc) is opened/applied by one of these commands, BUT the
dialog is **factory-dispatched** (created via a factory table `FUN_1409cfb30`→`FUN_1409deb60`, no static
call edge) and the underground-state source (`thunk_FUN_144dc036a`) is **VMProtected** — so the specific
command→toggle edge is not statically traceable from here.

## Actionable handle for the mod

To detect the map-layer toggle without the game's hidden state: **watch the live input** the mod already
has access to —
- pad: read the XINPUT_STATE the engine polls (`padObj+0x48`, fed from `DAT_1430b92e0`), or just call
  `XInputGetState` yourself;
- keyboard: `GetAsyncKeyState` on the bound key.

Detect the rising edge of the toggle input while the map is open (gate via the resolved cursor / `CSMenuMan+0xCD`)
and maintain a shadow `underground` flag. Combine with the page key (`dialog+0xA88`: 0=base, 10=DLC) for the
full region: `page==DLC → DLC` · `underground → area 12` · else `area 60/61`.
