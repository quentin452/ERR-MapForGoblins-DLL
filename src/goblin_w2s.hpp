#pragma once
// goblin_w2s — 3D world-to-screen (gameplay camera view-projection).
//
// The unblocker for the in-game ImGui/ESP overlay (runtime_modding_framework_vision.md #4):
// world (X,Y,Z) -> screen pixels -> ImDrawList. Runs ON the present thread (the RPC pump + overlay
// draw both do), so it reads the live player pos and the camera VIEW matrix on the SAME frame — no
// read-tearing (the failure mode that killed the external-RPM scan, see
// docs/re/windows_world_to_screen_camera_re_findings.md).
//
// Stage 1 (this file): a diagnostic `w2s_probe` that locates the GameRend camera instance, reads the
// VIEW matrix at GameRend+0xF0, and reports the player's view-space coords + nearby lens scalars, so
// the projection convention + FOV can be pinned live before wiring the final w2s3d + debug dot.

#include <string>

namespace goblin::w2s
{
    // RPC `w2s_probe`: diagnostic dump (present-thread). Returns a human-readable one-liner+detail.
    std::string probe();

    // Toggle the per-present debug dot (drawn by draw_present). `w2s_probe dot on|off`.
    void set_debug_dot(bool on);

    // Live-tune the projection while calibrating (`w2s_probe conv <0..3>` / `fovy <rad>`).
    void set_conv(int c);
    void set_fovy(float f);

    // Called once per present frame from the overlay; draws the debug dot at the projected player
    // pixel when enabled. No-op when disabled or the camera can't be resolved.
    void draw_present();

    // Live ER camera for the mod-owned 3D backend (goblin_r3d): the render-local VIEW matrix (16 floats,
    // row-major, row-vector v*M, conv2 forward=-vz), the per-frame render REBASE origin (subtract from a
    // WORLD point before the VIEW), and the vertical FOV. Lets r3d build a world->clip matrix aligned with
    // ER's own view. Re-finds the origin each call (the render re-centres per frame). Present-thread only.
    bool get_camera(float outView[16], float outOrigin[3], float &outFovy, float vpW, float vpH);
}
