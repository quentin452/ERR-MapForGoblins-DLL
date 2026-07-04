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
1. **Death rune icon (bloodstain)** — show a marker where the player died / dropped runes.
   **RECON 2026-07-04 — feasible, 2 small gaps:**
   - **Location is EASY (no frame bridge):** at the death moment, record `get_player_map_pos` — that is
     already the WORLD/marker frame our markers use (the same reader that draws the player dot). We do NOT
     need the game's native bloodstain coords (the Hexinton CT has "Bloodstain coords" but those are
     chunk/physics space → the chunk→world bridge wrinkle; skip it entirely).
   - **Gap 1 — death DETECTION (the moment):** no player-HP reader exists yet. Add one via the known
     WorldChrMan chain (AOB `48 8B FA 0F 11 41 70 48 8B 05`, `[[[[[WCM]+0x1E508]…]]]` → ChrIns → HP), fire
     on the HP→0 rising edge; OR find a death event flag. `DisableRuneLoss.dll` patches the rune-loss fn
     (its exact AOB needs Ghidra) — we don't need its hook, just the death moment.
   - **Gap 2 — native icon NAME:** `native_map_point_icon_by_name("<name>")` already resolves+draws a
     native map-point sprite by name (used for graces). Need the exact lost-runes/bloodstain symbol name.
     *USER to confirm which `.dcx` icon is the rune-loss one.*
   - Draw ONE at a time (a new death replaces it; clear on pickup — later). Same store + minimap path.
2. **Other-player icons (multiplayer)** — show other players' positions/icons (co-op/invasion). Check if
   there's existing RE on the multiplayer player list; likely none → a new RE item. SAME for the local
   **player icon**: today the minimap/vmap draw our own RED arrow — the NATIVE "you are here" player glyph
   exists in the assets; use it instead of the hand-made red once resolved.

### Known bug — custom markers are Base-ER only
Placement + draw are gated `active_world == 0`. In a CUSTOM vworld the right-click/list/pins do nothing.
Fix: let custom markers live per-vworld (store the active-world id alongside group), and draw/place in the
active world's coord space. Note before shipping more world-owned features.

### ⚠ SHIP RULE — custom content must stay Marker-Mapper compatible
When we ship anything custom (custom items via `custom_items.toml`, custom worlds, custom markers, …),
it MUST remain compatible with the CURRENT marker-mapper pipeline (the live marker build + the vmap/native
render). Don't ship a custom data path that the marker mapper can't ingest / that desyncs from how markers
are built and drawn. Acceptance: the custom thing shows + behaves through the SAME marker/render path as
everything else (or an explicit, tested additive layer), not a fork that drifts.

### Persistence
Custom markers are IN-SESSION only. Persist to `<mod>/custom_markers.toml` (like `virtual_worlds.toml`) so
they survive a restart — the `TOML_EXCEPTIONS 0` + `parse_file` path (the only one working under Proton).
