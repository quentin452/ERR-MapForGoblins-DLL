---
name: f1-close-edge-pan
description: "Endless map pan after closing F1 with the cursor at a screen edge — FIXED (cursor nudged out of the edge-pan band on the close falling edge). Includes the measured ER edge-pan band: ~150px at 1080p with speed falloff, so a 64px nudge was NOT enough."
metadata:
  node_type: memory
  type: project
---

Bug (user, 2026-07-02): ER edge-pans the world map while the OS cursor sits near the window
border; closing F1 with the cursor parked there (panel scrollbar at the screen edge, or an RPC
`mouse_move`) left the map panning forever until the mouse physically moved.

**FIXED 2026-07-02 (`fix/f1-close-edge-pan`, in-game validated via the RPC loop).** In
`hk_present`'s existing menu-close falling edge (goblin_overlay.cpp, next to the ClipCursor
re-confine): if the map is open (`world_map_open()`) and the cursor sits within `margin` of the
client rect border, clamp it inward via `set_cursor_pos_real` + a ±1px `SendInput` relative
jiggle (same pattern as debug_rpc's `move_cursor_client` — SetCursorPos alone holds one frame,
the map re-warps the OS cursor onto its raw-input reticle; a REAL mouse event makes the game
adopt the new position). Gated on `fg` (real close, not focus loss) and non-degenerate rects;
`g_ignore_next_mousemove_for_gamepad_flag` guard like the recenter helper. Logs
`[OVERLAY] F1 close: cursor in map edge-pan band (x,y) → nudged to (nx,ny)`.

**Key measurement — ER's map edge-pan band (1920×1080, ERR, live-probed 2026-07-02):** pan
speed FALLS OFF with distance from the border and only reaches zero ~**150px** in (x=100 fast,
x=130 slow, x=140 barely, x=150 none). First fix attempt used a 64px margin — reticle visibly
adopted the nudge yet the map kept panning (64 is well inside the band; the pan only stopped at
the map's own pan clamp). Final margin = `max(64, client_height/6)` (180px at 1080p),
height-proportional for other resolutions. Bottom edge did NOT pan in the probe (key-legend
bar region), but the uniform margin is kept — over-nudging is harmless.

Validation (scripted, ERR under Proton): left-edge close → nudge line + map static immediately
(frame diffs at t0/t2/t5 ≈ animation baseline ~30k px, vs ~1.9M px/3s while panning); centered
close → no nudge; map-closed + edge close → no nudge; SIG 30/30, 0 errors.

Related: the F2 fog locate-clamp repro frame (same pan clamp seen from the other side), and
[[overlay-gamepad-cursor-bugs]] for the cursor-recenter/gamepad-flag machinery this reuses.
