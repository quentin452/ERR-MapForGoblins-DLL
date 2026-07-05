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

## Next step — the LIVE probe (repo-proven, the acceptance test doubles as the search)
Static guessing on row/col-major is low-yield; do it empirically behind a throwaway `w2s_probe` RPC:
1. Resolve a camera struct pointer live (pin the `CSCameraImp` FD4Singleton instance global, or reach
   `GameRend` — scan for the pointer whose +0xF0/+0x130 blocks read as sane matrices).
2. Read candidate 64-byte blocks (GameRend +0xF0, +0x130; and the `[[cam+0x10]+0x18]+0x10` view block).
3. For each candidate ViewProj `M`: `clip = M * vec4(playerWorld,1)`; try both row- and column-major and
   both NDC-Y signs. The correct `M` projects player pos → the player's on-screen feet (we have W,H).
4. Draw a debug ImGui dot at the projected pixel; it must lock to the character through a full camera
   orbit + zoom, cull behind camera (`clip.w<=0`). Then ship `w2s3d` as a `GOBLIN_RENDER_API`.

Feeds `runtime_modding_framework_vision.md` #4 (virtual greybox world), an in-game hknp collision wireframe
(no Havok-VDB version lock), and any 3D-anchored HUD.
