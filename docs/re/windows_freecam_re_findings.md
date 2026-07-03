# Freecam — RE findings (recon / IN PROGRESS)

Status: **recon done (static, Ghidra, 2026-07-03); not yet a working freecam.** Answers the structural
part of `windows_freecam_re_prompt.md`; the exact override point (Route 2) + debug-cam activation (Route 1)
need 1–2 more query passes. Imagebase `0x140000000`; tool `query.java` on `D:\ghidra_proj2\ER`.

## Camera architecture (confirmed)
- **`CSCameraImp`** (singleton, `FD4Singleton<CSCamera,CSCameraImp>`) — dtor `FUN_1403bae40` (er+0x3bae40)
  releases **4 camera sub-slots** at `+0x08/+0x10/+0x18/+0x20` (the active + alternate cameras).
- **`ChrCam`** (gameplay/player-follow cam, itself an FD4Singleton) — dtor `FUN_1403b0850` (er+0x3b0850);
  sub-objects at `+0x60/+0x68/+0x70`, a pose/rig at `+0xC0`.
- **`CSDebugCam`** (FromSoft's built-in free camera) — base ctor is a **cold stub** `FUN_14560901a`
  (er+0x560901a: just sets the vtable), reached via `thunk @ er+0xb0f070` which has **many construction
  callers** (er+0x26797c, 0x2684ac, 0x28b480, 0x297bf0, 0x29a4b0, 0x2a8a2c, 0x2a9cb0 …). Companions:
  `CSDebugCamPad`/`CSEzDebugCamPad`/`CSDebugCamKeyAssign` (its pad/key fly-input). ⇒ the debug-cam CODE is
  present in retail; whether it's *activated as the active camera* is the open Route-1 question.
- **`CSCam`/`CSPersCam`** — setup/step `FUN_14076e7c0` (er+0x76e7c0) copies a **view-transform block** out
  of `*(self+0x10)+0x18` (bytes `+0x10..+0x5c` ≈ a 4x4 matrix + params) into the perspective cam.

## ★ The per-frame camera STEP — `FUN_140766980` (er+0x766980)
This is the camera manager's per-frame update (arg2 = frame Δt at `+8`). Key mechanics:
- **`DAT_143d6b880` (er+0x3d6b880) = the active camera instance global.** Each frame:
  `if (DAT_143d6b880) FUN_14076e7c0(DAT_143d6b880, dt, …)` — so the camera whose transform ends up on
  screen hangs off this global, and its view transform is `[*(DAT_143d6b880+0x10)+0x18] + 0x10` (the block
  `FUN_14076e7c0` reads).
- **Debug hotkey infra is live in retail:** `FUN_140ddb560()` is a key-poll; its result is compared to
  scancodes (`0x2d,0x38,0x0a,0x4a,0x0e,…`) with per-key hold timers `DAT_143d6b7cc..7f0`. Reusable as
  freecam input, and evidence the debug-camera toggles were not fully stripped.
- Reads the player: `plVar11 = *(DAT_143d65f88 + 0x1e508)` — `DAT_143d65f88` = **WorldChrMan**,
  `+0x1e508` = LocalPlayer (matches `windows_player_pos_RESOLVED`). The gameplay cam is driven off this.

## Two routes — current read
- **Route 2 (freeze + drive the output transform) — most tractable.** The active camera is
  `DAT_143d6b880`; its on-screen view transform is the `[*(+0x10)+0x18]+0x10` block. Freecam = stop the
  game recomputing it from the player and write our own each frame. Cleanest override points to evaluate:
  hook `FUN_14076e7c0` (per-frame consumer) or write the transform block AFTER the step (present-time), or
  find the **render-side** `GameRendCameraSet@GameRend@CS@@` (er+0x680460…) view matrix and override there.
- **Route 1 (enable `CSDebugCam`) — viable-looking but needs the activation path.** The class + input exist
  in retail; the open question is which field/enum in `CSCameraImp` (one of the 4 slots at `+0x08..+0x20`)
  selects DebugCam as active, and whether a single writable flag flips it. If it does, we get FromSoft's
  fly controls for near-zero code.

## Next queries (precise)
1. **Route 2 write-point:** decompile `FUN_140766980`'s caller `FUN_1407650a0` (er+0x7650a0) + find where
   `DAT_143d6b880`'s transform block is WRITTEN from the player (the recompute to freeze), and
   `GameRendCameraSet` ctor (er+0x680460) for the render view matrix.
2. **Route 1 activation:** decompile a couple of the `CSDebugCam` construction callers (er+0x28b480,
   0x29a4b0) + how `CSCameraImp` picks its active slot — is there a "camera mode" field + a debug branch.
3. **Singleton AOB:** pin the `CSCameraImp`/camera-manager singleton accessor (the owner of `param_1` in
   `FUN_140766980`) so the DLL can resolve it, like `MSG_REPOSITORY`.

## Anchors (er-relative, imagebase 0x140000000)
- `CSCameraImp` dtor `FUN_1403bae40`; `ChrCam` dtor `FUN_1403b0850`; `CSDebugCam` ctor-stub `FUN_14560901a`
  (+ `thunk` er+0xb0f070); `CSCam/CSPersCam` step `FUN_14076e7c0`.
- Camera per-frame step `FUN_140766980` (caller `FUN_1407650a0` er+0x7650a0).
- Globals: `DAT_143d6b880` (active camera instance), `DAT_143d65f88` (WorldChrMan; `+0x1e508`=LocalPlayer),
  debug key-poll `FUN_140ddb560`, hold timers `DAT_143d6b7cc..7f0`.
- RTTI: `CSCameraImp@CS@@` er+0x2a27fe0; `ChrCam@CS@@` er+0x2a279f8; `CSDebugCam@CS@@` er+0x2b64210;
  `GameRendCameraSet` er+0x2a7f2b8 (see `tools/ghidra/rtti_index.txt`).
