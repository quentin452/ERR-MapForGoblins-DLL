# Custom player markers — status + follow-ups

Our OWN player-placed marker system on the Virtual World Map (Path B — no RE, mod-agnostic). Store:
`src/goblin_custom_markers.{hpp,cpp}` (shared: vmap + minimap read it). UI: `panel_virtual_map.cpp`.

## Done (2026-07-04)
- **Place**: right-click the vmap canvas near empty space → drop a marker (blue pin, on top of all icons),
  tagged with the current group (which map).
- **List sidebar** (`Custom` toggle): each marker shows which map + coords, editable name, **Go** (pan),
  **TP** (teleport), **Delete**. Resizable.
- **Delete two ways** (#1): the sidebar Delete button OR right-click a pin on the canvas.
- **Cap per world** (#2): `kMaxPerGroup = 24`; `add()` refuses past it (status hint).
- **On the minimap** (#4): the current map's custom markers draw on the minimap, edge-clamped (bearing).

## Open follow-ups (before deleting the ER compass)

### #3 — TP is intra-region, want a REAL player teleport
`overlay_api::warp_to_world_xz` = the `warp_xyz` logic (write player local pos, offset by world delta,
`set_y=false`). It's **intra-region only** (a far / cross-map target lands in unstreamed void) and keeps
the current Y. NEEDED: a proper teleport that **streams the target area + snaps to ground Y** — reuse the
grace-warp streaming path (or a coordinate warp that triggers the load), not just a local-pos poke. RE:
the warp findings + `warp` (LuaWarp) already work for graces; a coord-warp with streaming is the gap.

### Native icons (replace our hand-made ones) — 2 items to NOTE
1. **Death rune icon (bloodstain)** — when the player dies, show the rune-loss marker (where runes dropped)
   on the map/minimap. `DisableRuneLoss.dll` HOOKS the rune-loss system → RE the exact mechanism (the
   death→bloodstain event + the dropped-runes location) so we can place our own marker there. Draw the
   **NATIVE** rune icon — it already exists in the decompiled `.dcx` (use it at runtime, don't bake).
   *User to confirm which .dcx icon is the correct one.*
2. **Other-player icons (multiplayer)** — show other players' positions/icons (co-op/invasion). Check if
   there's existing RE on the multiplayer player list; likely none → a new RE item. SAME for the local
   **player icon**: today the minimap/vmap draw our own RED arrow — the NATIVE "you are here" player glyph
   exists in the assets; use it instead of the hand-made red once resolved.

### Persistence
Custom markers are IN-SESSION only. Persist to `<mod>/custom_markers.toml` (like `virtual_worlds.toml`) so
they survive a restart — the `TOML_EXCEPTIONS 0` + `parse_file` path (the only one working under Proton).
