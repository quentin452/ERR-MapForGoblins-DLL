# 3D world-to-screen (camera view-projection) — RE findings (static, Ghidra, 2026-07-05)

Status: **static recon done; live-probe pending.** Answers most of the structural part of
`windows_world_to_screen_camera_re_prompt.md`. The exact live View-Proj field + resolve chain and the
row/col-major + NDC convention are best confirmed with the LIVE probe (repo-proven method) — the static
candidates below are strong but not yet runtime-verified. Imagebase `0x140000000`; tool `query.java` on
`D:\ghidra_proj2\ER` (out → `D:\ghidra_scripts\out_query.txt`).

## What's already in place (no RE needed — the overlay path is ready)
- **D3D12 Present hook exists.** `src/goblin_overlay.cpp` already installs a DX12 Present detour
  (`hk_present`, `IDXGISwapChain3`), captures `ID3D12Device` / command queue / command list, and runs a
  present-thread pump (`goblin::debug_rpc::pump()`, `heightfield::tick_present()`). So `w2s3d` + a debug
  `ImDrawList` dot can be wired straight into the present frame once we have the matrix — no new hook.
- **Oracle ready.** Player world XYZ = `LocalPlayer+0x6C0/+0x6C4/+0x6C8` (float, `[WorldChrMan+0x1E508]`,
  `src/goblin_world_position.cpp`), player yaw `+0x6CC`. This is the acceptance-test oracle: project player
  pos → must land on the feet on screen.
- **Singleton-resolve idiom.** Repo resolves singletons by **fixed RVA preferred + AOB fallback** (e.g.
  WorldChrMan slot `er+0x3D65F88`, `goblin_enemy_names.cpp` / `goblin_world_position.cpp`). Same pattern will
  resolve the camera anchor once pinned.

## Camera architecture (confirmed static)
Builds on `windows_freecam_re_findings.md`. RTTI (er base `0x140000000`):
- `CSCameraImp@CS@@` vtable **er+0x2a27fe0**, ctor **er+0x3bad20** (dtor 0x3bae40). 4-slot camera manager:
  ctor zeroes `param_1[1..4]` (= inst **+0x08/+0x10/+0x18/+0x20**, the active + alternate cameras), a flags
  word at +0x28, ptr at +0x30. vmethods are thin getters (`+0x38`, `+0x40`) — NOT the view-matrix accessor.
  It's an `FD4Singleton<CSCamera,CSCameraImp>`; reflection-class object at `DAT_143d62990` (er+0x3d62990) —
  **NB: that is the DLRuntimeClass, NOT the instance pointer.** The instance global is not exposed by the
  ctors seen; pin it live (or via a `GetInstance` accessor scan).
- `GameRend@CS@@` vtable er+0x2a7f2c8; **`GameRendCameraSet@GameRend@CS@@`** vtable **er+0x2a7f2b8**.
  `GameRend` is the outer object; `GameRendCameraSet` is an **embedded subobject at +0x10** (ctor sets
  `*param_1 = GameRend::vftable` then `param_1[2] = GameRendCameraSet::vftable`). Its own vtable is tiny
  (dtor / one method / ctor) → it's a **data holder**, not where ViewProj is computed.

## ★ The matrix candidates (from the GameRend init `FUN_1406800f0` = er+0x6800f0)
`FUN_1406800f0` default-initializes the `GameRend`/`GameRendCameraSet` instance and lays down **two
consecutive 4×4 float blocks**, copied from static default matrices (`er+0x30b07a0` block):
- **Matrix A @ instance +0xF0** (`param_1[0x1e..0x25]`, 8 qwords = 16 floats = 0xF0..0x12F).
- **Matrix B @ instance +0x130** (`param_1[0x26..0x2d]`, 16 floats = 0x130..0x16F).
- Earlier: a 3rd 4×4-ish block @ **+0x30** (`param_1[6..9]`, from `er+0x3d67a28`) + scalar params
  `+0x54=0x3f402037` (~0.7539), `+0x58=1.0f`, `+0xa4=0x3ecccccd` (0.4), `+0xa8=0x3f333333` (0.7),
  `+0xac=0x41700000` (15.0) — look like camera/lens params (near/far/FOV-ish), useful to sanity-check which
  struct we're in.

⇒ **A@+0xF0 and B@+0x130 are the prime View / Projection (or ViewProj) candidates.** They're default (near-
identity) at construction; the per-frame renderer overwrites them — confirm live which is View vs Proj vs
combined, and row- vs column-major.

## Camera-source VIEW transform (alternate anchor — from CSCam step `FUN_14076e7c0` = er+0x76e7c0)
The `CSCam`/`CSPersCam` per-frame step reads the camera's view block via the chain
**`[[cam+0x10]+0x18]+0x10`**: a **64-byte 4×4** (bytes +0x10..+0x4f) followed by params
+0x50/+0x54/+0x58/+0x5c. This is the camera **world/view** transform (per freecam note it applies to the
gameplay cam too). Gives VIEW; Projection would still need building from the lens params — so the GameRend
+0xF0/+0x130 pair (likely already-combined render matrices) is the preferred single-read target.

## Anchors / addresses (er-relative)
- `GameRend`/`GameRendCameraSet` init `FUN_1406800f0` (er+0x6800f0); ctor `FUN_140680460` (er+0x680460);
  vtable ctor `FUN_140680690` (er+0x680690); matrix defaults from `er+0x30b07a0` and `er+0x3d67a28`.
- `CSCameraImp` ctor `FUN_1403bad20` (er+0x3bad20); vtable er+0x2a27fe0; reflection-class `DAT_143d62990`.
- `CSCam/CSPersCam` step `FUN_14076e7c0` (er+0x76e7c0), called from `FUN_140766980` (menu-mgr step).
- Freecam globals: `DAT_143d6b880` (menu-scoped camera), `DAT_143d65f88` (WorldChrMan; +0x1e508=LocalPlayer).

## LIVE RPM probe attempt (2026-07-05) — partial result + a DEAD method
Ran external `ReadProcessMemory` probes (Python, `D:\ghidra_scripts\w2s_probe.py`, `w2s_probe2.py`,
`w2s_scan_global.py`) against a live in-world `eldenring.exe` (base ASLR'd, resolved via Toolhelp).

**Confirmed live:**
- Scanning committed memory for the `GameRendCameraSet` vtable (`base+0x2a7f2b8`) finds **2 live
  instances**. Instance #1 (the active one) holds at **GameRend+0xF0** a clean **VIEW matrix** — an
  orthonormal 3×3 rotation + a translation 4th row, 4th column `(0,0,0,1)`, i.e. **row-vector / DirectX
  convention** (`v·M`). Instance #0's blocks are uninitialised ramp garbage.
- **GameRend+0x130 = identity** in the live instance ⇒ the *combined* View-Projection is **NOT** stored at
  the two default-init offsets. The projection / combined VP lives elsewhere (built per-draw, or a sibling
  field / a D3D12 constant buffer).

**DEAD method — do NOT repeat: brute-force "scan all memory for a matrix that projects the player".**
Two structural reasons it can't work from an external RPM process:
1. **Read-tearing / motion.** RPM reads the player pos and the candidate matrices at *different instants*.
   With the camera or player moving (observed: player X drifted -47.05 → -45.58 between runs) no matrix
   ever matches a stale player pos → the strict 3-point filter found **0** hits; loosening it produced
   **~26 000** coincidental false positives (bone/skeleton/UI matrices, config float blocks). Unworkable
   signal-to-noise.
2. It stresses the game (~8 GB walk) — **the game crashed** during the global scan.
An empirical match would require a *frozen frame* (game paused, single atomic snapshot of pos+matrix),
which external RPM can't guarantee. ⇒ **pivot to static-first (below), or do the match INSIDE the DLL**
(same-thread, same-frame read of pos+matrix — no tearing).

## Revised next step — static-first (deterministic), verify with ONE read
Don't scan. Find WHERE the projection / combined ViewProj is written, in Ghidra:
1. Trace the **camera step** (`CSCameraStep` / `CSStepTask<CSCameraStep>` ctors er+0x3bb8b0/3bba00/3bbc30/
   3bbd0; `CSCam/CSPersCam` step er+0x76e7c0) → find the function that **builds the perspective matrix**
   (recognisable: `1/tan(fov/2)`, aspect, near/far, a `-1` in the w-row) and where the **View×Proj** result
   is stored / uploaded to the D3D12 constant buffer.
2. That yields a deterministic offset (from `GameRend+0xF0` VIEW, or the VP field). Then a *single* RPM read
   on a **paused** game confirms it — no scanning.
3. Alternatively (fastest to a testable result): keep the confirmed **VIEW @ GameRend+0xF0** and **build the
   projection in the DLL** from the lens params (ctor put FOV/near/far-like scalars at GameRend
   +0x54/+0x58/+0xa4/+0xa8/+0xac); combine `VP = View·Proj` at present time. Because the DLL reads pos+View
   on the SAME present frame, there is no tearing. Draw the debug dot; iterate the convention live.

Feeds `runtime_modding_framework_vision.md` #4 (virtual greybox world), an in-game hknp collision wireframe
(no Havok-VDB version lock), and any 3D-anchored HUD. Probe scripts: `D:\ghidra_scripts\w2s_probe*.py`.

## ★ LIVE CALIBRATION 2026-07-05 (Linux/Proton) — VIEW+FOV confirmed, blocked on a CAMERA-RELATIVE render frame
Ran the in-DLL `w2s_probe dot on` + `conv 0..3` calibration live (`tmp/w2s_calib.py`; full dump logged as
`[W2S]`). The GameRendCameraSet instance resolves (`GameRend=0x24e7c500`), and the pieces are confirmed:
- **VIEW@+0xF0 (live):**
  ```
  [-0.5960  0.0000  0.8030  0.0000]
  [ 0.0000  1.0000  0.0000  0.0000]
  [-0.8030  0.0000 -0.5960  0.0000]
  [-2.1262  4.7951  3.9725  1.0000]   <- translation row
  ```
  Clean Y-rotation upper-3×3, 4th col (0,0,0,1) = **row-vector / DirectX** (`v·M`), as the static recon said.
- **FOV CONFIRMED = 0.7505 rad** (~43°) — it's `lens@+0x54`; `lens@+0x5c = 500000` = the far plane. So the
  `fovy` default in `goblin_w2s.cpp` was already right, and `+0x130` is identity (no combined VP), all as found.
- **Convention = conv2** (row-vector `v·M`, **+Z forward**) — the ONLY one giving `fwd>0` for the player.

**⛔ But no dot lands on screen — root cause = camera-RELATIVE rendering (a real gap the plan missed).**
The VIEW **translation row is TINY: `(-2.13, 4.80, 3.97)`**. A true global world→view would have translation
`= -R·cam_world_pos`, magnitude ~100+ for a camera at world ~(-58,92,99). It's ~4 ⇒ **the VIEW operates in a
render frame RE-CENTRED near the camera** (ER rebases world coords near the camera for float precision — standard
in large-world engines). Meanwhile `get_player_world_pos` (`LocalPlayer+0x6C0`) returns the **global** frame
`(-58.13, 92.80, 99.97)`. Feeding global coords into a render-local VIEW projects into the wrong space:
`conv2 view=(-47.76, 97.59, -102.29) → px=(320, -768)` (off the top). Note `vy = playerY(92.80)+transY(4.80)`
= the Y row is pass-through identity, so the world Y is used raw against a ~4-unit translation = far off-screen.
**This is NOT a wrong convention/FOV** (both are right) — it's a missing coordinate rebase.

**⇒ NEXT (the real w2s3d unblocker): find the render REBASE ORIGIN** — the large offset the engine subtracts
from world coords before the VIEW (near the camera/player, ≈ `(-56, 88, 96)` this frame). Then
`player_render = player_global − origin`, project `player_render` through VIEW (conv2) → should lock to the feet.
Candidates to scan: a `float3`/`double3` ≈ `(-56,88,96)` in the GameRend/camera struct, or the camera's GLOBAL
position elsewhere in the struct, or a WorldChrMan render-origin field. Cheap next probe = extend `w2s_probe` to
dump the qwords/floats around `GameRend+0x00..+0xF0` and near the camera and look for that ~(-56,88,96) vector.
(Alternative reframing the user raised: don't ImGui-project at all — render the virtual 3D via a **separate 3D
backend** sharing ER's swapchain/depth, which sidesteps needing this w2s matrix — see HANDOFF discussion.)

## ★ 2026-07-07 (Ghidra RE) — GameRend has NO static slot; the VIEW-writer chain (how +0xF0 is filled)
Ran the FD4Singleton assert-map + vtable-ref trace (`D:\ghidra_proj2`, pyghidra) to answer "what static
pointer gets us to GameRend without a scan" (the greybox-render Windows-hardening task). Answer: **there is
none — GameRend is not an FD4Singleton.** It is constructed inside the game's task-step tree and referenced
by pointer from several owners, none static:
- **`renderObj+0xE8`** — the per-frame render step `FUN_140b019b0` (er+0xb019b0), called from
  `FUN_140aff640` (er+0xaff640), the InGameStep update. `param_1` (the render step) is passed in, not static.
- **`InGameStep_megaobj+0xB3628`** — the InGameStep factory (`FUN_140aec120`→`FUN_140aed820`) allocates
  GameRend (`FUN_1406800f0`, 0x180 bytes) and stores it there; the dtor `FUN_140aed380` reads it back.
- The mega-object itself is `new`'d in `FUN_140aeaaa0` and stored in a parent `CSStepTask` member array
  (`FUN_140b0b1c0` → member at another `new`'d parent, er+0xae786f). RTTI of the enclosing steps:
  `EzChildStep` / `InGameStep` / `MoveMapStep` / `CSStepTask<TitleStep>` — heap task tree, no `mov [rip],inst`
  store anywhere. Reaching it from a static = walking live task pointers with non-AOB-able slot indices.
- **`GameRendCameraSet@GameRend@CS@@` vtable = er+0x2a7f2b8** (confirmed via `rtti_index.txt`, this 2.6.2.0
  build) — so the mod's `VT_CAMSET_RVA` is correct; a vtable scan IS the right way to find the live instance
  (the fix was to make that scan present-thread-safe, not to replace it — see
  `windows_w2s_camera_finder_present_hang_findings.md`).

**How VIEW@GameRend+0xF0 is produced each frame** (useful if we ever want the VIEW without touching
GameRend): `FUN_140b019b0` does `buf = FUN_1403f0f60(FUN_140507ff0(WorldChrMan), local)` then copies `buf`
into `GameRend+0xF0` (the 4×4) `+0x110`/`+0x120` (params):
- **`FUN_140507ff0(WorldChrMan)`** (er+0x507ff0) — the camera SOURCE getter: returns the freecam object when
  `DAT_143d66198 != 0`, else `*(WorldChrMan + 0x1E508)` = **LocalPlayer** (the SAME `[WCM+0x1E508]` the mod
  already resolves). WorldChrMan static = **er+0x3D65F88** (mod's `WCM_FINDER`).
- **`FUN_1403f0f60(camsrc, out)`** (er+0x3f0f60) — `FUN_14045e540(*(*(camsrc + 0x190) + 0x68))`: reads
  `[[camsrc+0x190]+0x68]` (the SAME pose/phys object the coord-teleport writes — `posObj` chain) and computes
  the VIEW. ⇒ a scan-free VIEW is theoretically reachable as `WorldChrMan → [+0x1E508] → [+0x190] → [+0x68] →
  FUN_14045e540`, but it needs calling/replicating `FUN_14045e540` (own RE + present-thread crash-risk
  validation). Deferred — the time-boxed vtable scan is the low-risk path that keeps the proven +0xF0 read.

### `FUN_14045e540` decoded (er+0x45e540) — and why the scan-free deref path is NOT a clean win
`FUN_14045e540(poseObj, out[16])` (disasm confirmed) builds a **4×4 from the pose object's fields**, NOT a
stored view it could just copy:
- `poseObj+0x60..0x6c` = a **quaternion** (x,y,z,w); `poseObj+0x70/74/78` = a **position** vec3 (the SAME
  `+0x70` the coord-teleport writes on `[[LocalPlayer+0x190]+0x68]`).
- It expands the quat to a rotation matrix (the `q*(q+q)` / `ANDPS mask` SIMD idiom), scales by static
  constants (`er+0x279230..`, `er+0x2792f0..`, `er+0x29e678`), and writes rows into `out[0..15]`; the
  translation row (`out[12..15]`) mixes the `+0x70` position. Caller `FUN_140b019b0` then `MOVAPS`-copies
  `out` → GameRend+0xF0/+0x100/+0x110/+0x120.
- **Two reasons a blind deref path is risky:** (1) it's derived from the **player/freecam POSE**, yet the
  mod's live-confirmed VIEW@+0xF0 had a TINY (render-rebased) translation, not the player's ~(-58,92,99) — so
  either those constants encode the rebase, or `FUN_140b019b0` is NOT the writer the mod actually reads;
  (2) `FUN_140b019b0` is one sub-update inside `FUN_140aff640` (the InGameStep tick) — there are **several
  GameRend-like writers**, so which one owns the gameplay VIEW must be settled LIVE (A/B on a running game).
- ⇒ Chosen path instead (2026-07-07): keep the proven vtable scan, just make it fast by restricting to
  **MEM_PRIVATE** heap regions (the instance is an FD4-heap alloc). See
  `windows_w2s_camera_finder_present_hang_findings.md`. Revisit the scan-free deref only if the narrowed scan
  still can't resolve the camera (the `w2s_probe` coverage diagnostic will say which).
