#pragma once
// goblin_r3d — minimal mod-owned D3D12 3D backend (virtual_world_3d_backend_plan.md, step 1).
//
// Draws real 3D geometry INTO ER's swapchain via the present hook (goblin_overlay.cpp), reusing the
// device + command queue + per-frame command list the overlay already holds. Step 1 = prove the pipeline:
// one spinning greybox WIREFRAME cube with a hardcoded transform (no depth, no real camera yet — the
// player-driven camera is step 2 / case 2). ImGui renders AFTER this, so it overlays on top.

struct ID3D12Device;
struct ID3D12GraphicsCommandList;

namespace goblin::r3d
{
    // Record the test cube into `cl` (the caller already bound the backbuffer as RTV, no depth).
    // Lazy one-time init on first call (needs the live device). No-op when disabled or init failed.
    void draw_test_cube(ID3D12Device *dev, ID3D12GraphicsCommandList *cl, float vpW, float vpH);

    void set_enabled(bool on);   // RPC `r3d 0|1|toggle`
    bool enabled();

    // Debug-render tool: draw a world-anchored wireframe box at an arbitrary WORLD position (visualise an
    // invisible mesh / entity / loot at its real coords). Boxes render via the live ER camera (get_camera),
    // so they sit in the world. With no boxes added, r3d draws the default cube at the player.
    void add_box(float x, float y, float z, float size);  // RPC `r3d box <x> <y> <z> [size]`
    // Per-axis box: center `pos` (world XYZ) + `half` extents — the greybox-object render (matches an
    // add_collision walkable box built from the same half-extents). Wireframe for now (unit-cube base
    // scaled per axis); solid/greybox material is a later render upgrade.
    void add_box_ex(const float pos[3], const float half[3]);
    void clear_boxes();                                   // RPC `r3d clear`
    int box_count();
}
