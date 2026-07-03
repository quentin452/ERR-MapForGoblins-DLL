# Windows-Ghidra + live-probe RE prompt — Freecam (detached, input-driven dev camera)

## Why
The dev "creative mode" mini-track (`docs/HANDOFF.md`, vision doc "Future directions"): a free-flying
camera detached from the player is the verification + authoring tool for the world-editing work — move/ADD
placement, the World Editor (vision #2), and later "build worlds without relaunching the game." Every open
confirm on that track ("does the moved/spawned asset land where I put it?") needs eyeballing the world at
an arbitrary viewpoint. Freecam is moderate, well-trodden ER RE (not a frontier).

## What the RTTI index already shows (anchors — verify)
From `tools/ghidra/rtti_index.txt` (er base 0x140000000):
- **`CSCameraImp@CS@@`** — the camera singleton impl (`FD4Singleton<CSCamera, CSCameraImp>`); ctors
  er+0x3bae40 / er+0x3bad20. This is the master camera manager (holds the active camera + output view).
- **`ChrCam@CS@@`** — the gameplay (player-follow) camera; ctors er+0x3b0850 / er+0x3b0670.
- **`CSDebugCam@CS@@`** — FromSoft's built-in **debug free camera**; ctors er+0x5609..a / er+0x5bb9412.
  Companions: `CSDebugCamPad`, `CSEzDebugCamPad`, `CSDebugCamKeyAssign` — its pad/key fly-input drivers.
- `CSCam@CS@@` (ctors er+0x76e7c0, er+0xb0eaf0, er+0xb0ec90), `GameRendCameraSet@GameRend@CS@@`
  (er+0x680460… — the render-side camera set that consumes the final view matrix).
- Related infra we already have: player pos (`windows_player_pos_RESOLVED`), the FieldIns transform
  setter pattern (`vtable[0xd0] SetWorldMatrix`), input device flags (`input-device-active-flag`).

## Two candidate routes (RE both enough to choose)

### Route 1 — enable the built-in `CSDebugCam` (cleanest IF it survives retail)
FromSoft's debug camera is present in the retail binary (RTTI proves the class + its pad input exist). If
its **activation path** is reachable, we get free fly + FromSoft's own controls for near-zero code.
Questions:
1. How is the active camera selected in `CSCameraImp`? Find the field/enum that switches ChrCam ↔
   DebugCam (a "camera mode" on the singleton, or a slot the update loop reads). Decompile the
   `CSCameraImp` ctor + its per-frame step (`CSCameraStep`) to find the mode field + the branch that picks
   the active camera.
2. Is `CSDebugCam` instantiated at boot, or only under a dev flag? Find its ctor callers + any global
   gate (a `DAT_…` bool, an `#ifdef DEBUG`-style dead branch, or a menu/keybind that sets the mode).
3. If gated, is the gate a single writable flag (flip it) or is the instantiation/step code removed?
   (Retail often keeps the class + vtable but strips the activation caller — determine which.)

### Route 2 — freeze + drive the active camera's OUTPUT transform (reliable fallback)
Independent of the debug cam: find the camera's final world/view transform (position + orientation, or the
4x4 view matrix) that the renderer consumes, freeze the game's own per-frame camera update, and drive the
transform from our input. This is what most retail freecam mods do.
4. In `CSCameraImp` (or `GameRendCameraSet`), locate the **output view transform** — the world-space camera
   position + rotation (or the view/world matrix) read by the renderer each frame. Give its offset from the
   singleton + the exact layout (vec3 pos + quat/basis, or a mat4).
5. Find the **per-frame writer** of that transform (the camera step that recomputes it from ChrCam/player).
   To hold a freecam we must either (a) skip/hook that writer while freecam is active, or (b) write our
   transform AFTER it each frame (a present-time or post-step override). Identify the cleanest hook point.
6. Confirm the singleton accessor (AOB) for `CSCameraImp` so the DLL can resolve it (like `MSG_REPOSITORY`
   / `WORLD_GEOM_MAN_SLOT`). Note whether the camera struct is stable enough to pin by offset.

## Live probe (Proton, dev RPC)
Behind a throwaway `freecam` RPC (mirror `goblin_geom_move.cpp` style):
- **R1 probe:** flip the debug-cam mode flag (Q1–Q3) and see if the view detaches + the pad flies it.
- **R2 probe:** freeze the camera writer + write a test transform (e.g. lift the camera +10 Y, yaw 90°);
  confirm the view moves and holds, game alive. Then wire WASD/mouse (or pad) → transform deltas.
Report which route detaches the view cleanly; that picks the implementation.

## Deliverable
`docs/re/windows_freecam_re_findings.md`: the camera singleton accessor + output-transform offset/layout,
the per-frame writer + chosen freeze/override hook, the debug-cam mode field (and whether R1 is viable),
and the live-probe result. With that, a `freecam` toggle + input drive is a small implementation.

## Notes
- Static-first (routes are readable without runtime); the live probe is a short in-DLL test on Proton.
- Pin code SIGs / struct offsets, not raw RVAs (AOB doctrine, `common.md`).
- Mod-agnostic: camera is engine-level (vanilla/ERR/ERTE/Convergence share it) — don't anchor on ERR data.
- Interplay to keep in mind: freezing the camera must not desync input/HUD; keep it behind the dev gate and
  restore cleanly on toggle-off (same discipline as `move_asset` restore).
