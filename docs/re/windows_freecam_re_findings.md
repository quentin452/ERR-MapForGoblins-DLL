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

## ★ `FUN_140766980` is the `CSMenuManImp` per-frame step (CORRECTION)
`FUN_140766980` is registered as **`CSMenuManImp`'s update task** — its ctor `FUN_1407650a0`
(= `CSMenuManImp` ctor) does `param_1[0x111] = FUN_140766980`. So `param_1` there is the **menu manager**,
and `DAT_143d6b880` is a **menu/debug-scoped camera** (map/model-viewer/photo-style), **NOT** the gameplay
camera. (My first read that it was "the active camera" was wrong — it's the menu manager's camera.)
Still useful:
- The **debug-hotkey infra is live in retail** (`FUN_140ddb560()` key-poll vs scancodes + hold timers
  `DAT_143d6b7cc..7f0`) — reusable as freecam input; the transform block `[*(inst+0x10)+0x18]+0x10` read by
  `FUN_14076e7c0` is the per-camera view layout (applies to the gameplay cam too).
- `DAT_143d65f88` = **WorldChrMan** (`+0x1e508` = LocalPlayer) — the player the gameplay cam follows.

**`CSDebugCam` is wired into `CSModelViewerStep`** (dtor `FUN_14028b480` constructs a `CSDebugCam` at
`+0x6c`). So the debug free camera belongs to the **dev model-viewer subsystem** — Route 1 = enabling a dev
tool that is very likely gated off in retail (a dev/`#ifdef`-style gate), not a single gameplay flag.

## Two routes — corrected read
- **Route 2 (freeze the gameplay cam + drive its output) — the reliable path.** Target is NOT
  `DAT_143d6b880` (menu-scoped) but the GAMEPLAY camera via **`CSCameraImp`** (4-slot singleton) → active
  camera → the render view matrix in **`GameRendCameraSet@GameRend@CS@@`** (er+0x680460…). Freecam = freeze
  the `ChrCam` update (stop it recomputing from LocalPlayer) and write our own view transform (the
  `[…+0x18]+0x10` block layout) each frame, or override the render view matrix at present time.
- **Route 1 (enable `CSDebugCam`) — lower priority.** It's a model-viewer/dev-subsystem camera; activating
  it means turning on a dev tool that's probably gated. Reusable pieces (its pad/key input) but not the
  quick win first assumed.

## Next queries (precise, revised)
1. **`GameRendCameraSet` ctor** (er+0x680460) + its vtable — locate the **render view/projection matrix**
   the renderer consumes; that's the cleanest freeze/override point (independent of which logical camera
   feeds it).
2. **`CSCameraImp` active-camera + `ChrCam` step** — how `CSCameraImp` picks its active slot (of the 4) and
   where `ChrCam` writes its output transform each frame (the recompute to freeze). Grep `rtti_index.txt`
   for the `ChrCam`/`CSCameraImp` step tasks.
3. **Singleton AOB** for `CSCameraImp` (the `FD4Singleton<CSCamera,CSCameraImp>` instance ptr) so the DLL
   can resolve it, like `MSG_REPOSITORY`.

## Anchors (er-relative, imagebase 0x140000000)
- `CSCameraImp` dtor `FUN_1403bae40`; `ChrCam` dtor `FUN_1403b0850`; `CSDebugCam` ctor-stub `FUN_14560901a`
  (+ `thunk` er+0xb0f070); `CSCam/CSPersCam` step `FUN_14076e7c0`.
- Camera per-frame step `FUN_140766980` (caller `FUN_1407650a0` er+0x7650a0).
- Globals: `DAT_143d6b880` (active camera instance), `DAT_143d65f88` (WorldChrMan; `+0x1e508`=LocalPlayer),
  debug key-poll `FUN_140ddb560`, hold timers `DAT_143d6b7cc..7f0`.
- RTTI: `CSCameraImp@CS@@` er+0x2a27fe0; `ChrCam@CS@@` er+0x2a279f8; `CSDebugCam@CS@@` er+0x2b64210;
  `GameRendCameraSet` er+0x2a7f2b8 (see `tools/ghidra/rtti_index.txt`).
