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
1. **Death rune icon (bloodstain)** — ✅ DONE 2026-07-04 (native, persistent).
   Mirrors the game's OWN bloodstain: `read_bloodstain` (`goblin_inventory.cpp`) reads `[GameDataMan+0x48]`
   — area-local X/Y/Z @ +0/+4/+8, mapId @ +0x38, runes @ +0x34 (Hexinton CT). `death_marker::tick()` (per
   present frame, `goblin_overlay.cpp`) reads it: `souls>0` → project mapId→(area,gridX,gridZ), `wx=gridX*
   256+x` (overworld frame, same as graces) → `death_marker::set`; `souls<=0` (collected) → `clear`. So it's
   SAVE-BACKED (persists across restart), auto-clears on pickup, one marker replaced on death — EXACTLY ER,
   no hook (simpler than DisableRuneLoss which patches the drop). Drawn = native `MENU_MAP_DropSoul` on vmap
   + minimap. Verified in-game (souls=1154 → icon auto-placed at the projected Limgrave spot).
   **TODO:** underground/legacy dungeon deaths (area != 60/61) need the WorldMapLegacyConvParam fold (tick
   currently skips them). Also `get_player_hp` (ChrDataModule `[[LP+0x190]+0]+0x138` cur / +0x13C max,
   validated cur/max=1214) is committed + exposed (RPC `hp_probe`) — unused by the marker now, kept for Track B.

   ~~RECON 2026-07-04 — feasible, 2 small gaps:~~ (superseded by the native read above)
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
2. **Player icons — split into (a) achievable now, (b) RE effort** (recon 2026-07-04):
   - **(a) Native LOCAL player cursor** (replace our red arrow) — NO new RE. We have pos + yaw; draw
     `MENU_MAP_Player_02` (effigy) + `MENU_MAP_Bearing` (arrow, ROTATED by yaw via `dl->AddImageQuad` — 4
     rotated corners; calibrate a +π/2 offset) instead of the hand-made red triangle, on vmap + minimap.
   - **(b) OTHER players (co-op/invaders/phantoms)** — real RE. NOT GameDataMan (single-player save). Other
     players are ChrIns in WorldChrMan, but there's NO CT recipe for the list: must RE WorldChrMan's ChrSet
     (the ChrIns array + count), filter by chrType/handle (player vs NPC vs local), read pos @ ChrIns+0x6C0.
     ALSO hard to test (needs a live MP session with phantoms — the offline dev box has none). Track-2-ish.
     Native icons found: `MENU_MAP_Host/Guests/Coop_01-02/Friend_00-03/Enemy_00-03/Raid_01-02`.

### vmap not resolution-aware (2026-07-04)
The minimap scales everything by `uiScale = screenH/1080` (icons/size follow the resolution). The vmap
(`panel_virtual_map.cpp`) uses FIXED px for its icons/pins/text — no uiScale, no FontGlobalScale. Readable at
1080p, but tiny at 4K / huge at 720p (partly mitigated by the resizable window). TODO: thread a uiScale into
the vmap draw (icon half, pin sizes, tooltip text) like the minimap does.

### Minimap icons too small (2026-07-04)
`draw_minimap`: `half = 6px * uiScale * masterScale * iconScale`, clamped `[3px, 10px]`. At 1080p (uiScale=1)
that's 12px icons capped hard at 10px — small (the death marker uses the same `half`, so it reads tiny too).
This is a GENERAL minimap sizing issue, not death-specific. Fix candidate: bump base 6→8 + cap 10→14, but it
enlarges every minimap icon on a small HUD (risk of crowding) → tune/verify before committing.

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
