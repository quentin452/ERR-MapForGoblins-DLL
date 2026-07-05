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
