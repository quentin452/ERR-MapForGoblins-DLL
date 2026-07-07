# r3d (mod-owned D3D12 backend) — renders on Windows, but STALLS the present to 0 FPS

**Status: BLOCKER for r3d on native Windows. The geometry renders CORRECTLY; the draw collapses the
present rate to 0 FPS (→ CPU-100% freeze). Found 2026-07-07 wiring the objects.toml greybox realizer.**

## Symptom (user screenshot, in-world tutorial popup)

`objects realize` (3 boxes) → `r3d` enabled → the colorful wireframe cube **renders perfectly** in the
scene (perspective, world-anchored via the ER camera). But the RTSS/Afterburner overlay reads **`D3D12
0 FPS`** — the present rate collapses to zero and the game freezes (spinning cursor, CPU ~88%). Each test
ended in the recurring `eldenring.exe+0x1EB9999` teardown crash (the user force-quitting the freeze).

So: **the render path is CORRECT** (cube visible, right shape/camera/color) — the bug is that submitting
r3d's D3D12 work into ER's command list **stalls the present** on native Windows D3D12.

## Why Windows-specific

r3d (`goblin_r3d.cpp`, `virtual_world_3d_backend_plan.md` steps 1–2) was only ever live-verified on
**Linux/Proton via vkd3d-proton**, which is far more forgiving than native Windows D3D12 about
synchronization / resource-state / command-list hazards. This is the FIRST time r3d drew real boxes on
the native-Windows box, and the driver stalls where vkd3d did not.

Note the gameplay gate (`get_player_world_pos() && !world_map_open()`) does NOT help here: the freeze
reproduced during a tutorial popup, which is in-world + map-closed (passes the gate). The stall is the
DRAW itself, not the game-state it draws in.

## Prime suspects (D3D12 sync/state, in likely order)

1. **Resource state / missing barrier** — r3d records into ER's command list after the RTV barrier
   (`goblin_overlay.cpp` ~2043) but manages no barriers/fences of its own; a hazard between r3d's draw and
   ER's/ImGui's subsequent work can serialize hard (or TDR) on native D3D12.
2. **Upload-heap VB/IB every frame** (`make_upload_buf`) — legal but reading an UPLOAD-heap resource each
   frame without a copy to a DEFAULT-heap resource can stall the GPU on native drivers. Move geometry to a
   DEFAULT heap uploaded once.
3. **Command-list state left dirty** — r3d changes PSO/root-sig/topology/VB/IB/viewport/scissor and relies
   on "ImGui re-binds after"; verify ImGui actually restores EVERYTHING r3d touched (esp. root signature
   + primitive topology) before ER's own later submits.
4. **PSO/RTV mismatch** — `R8G8B8A8_UNORM` was matched to the swapchain on Linux; re-verify the native
   swapchain format (HDR? `R10G10B10A2`?) — a format mismatch can fault on native D3D12.

## Recommendation

The objects.toml **realizer is proven** (the greybox rendered from TOML — the whole data→3D pipeline
works). Two ways to a usable greybox render ON WINDOWS:

- **A — harden r3d for native D3D12** (fix the present stall: default-heap geometry, explicit barriers,
  verify state restore + swapchain format). The cube already renders, so this is close — but each iteration
  needs a live boot + a likely freeze, so debug carefully (PIX/GPU-validation-layer offline if possible).
- **B — render objects via the ImGui backend** (plan backend "A"): project each box's 8 corners with the
  existing w2s and draw 12 edges as an ImGui draw-list line loop. ImGui is rock-solid on this box (F1 /
  vmap / minimap render every frame with no stall), so this sidesteps the r3d D3D12 issue entirely and
  gives a stable greybox-outline render immediately. Less "true 3D" (no depth/fill) but reliable.

Meanwhile `objects realize` no longer auto-enables r3d (it would freeze); enable `r3d 1` only to reproduce
this stall.
