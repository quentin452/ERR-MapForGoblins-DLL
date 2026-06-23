# RE prompt — Underground player position (m12 / DLC m40-43) for the overlay map

**Status: BLOCKED / WAITING on RE.** The overlay's distance-adaptive clustering and a
"you are here" player marker need the player's world position. It works on the OVERWORLD
but is **unavailable underground**, so distance-adaptive is currently gated to the
overworld only (`src/worldmap/map_renderer.cpp`, `!(open_grp & 1)`).

## What we need

The player's CURRENT position while underground (Siofra / Ainsel / Nokron / Deeproot =
map block **m12**, and the DLC underground = **m40..m43**), in a frame we can convert to
the overlay's UNIFIED marker space — i.e. the same space markers land in after
`goblin::marker_world_pos(area, gridX, gridZ, posX, posZ, conv_underground=true)`. With
that we can place the player dot + compute near/far distance underground.

Overworld already works via `goblin::get_player_map_pos` (returns area 60/61 + world
X/Z) — see `src/goblin_inject.cpp`.

## What we tried and the exact failure (live log, 100% save, at the Mimic Tear grace, area 12)

Two existing probes both FAIL underground:

1. `get_player_world_pos` (goblin_inject.cpp ~853) reads WorldChrMan:
   - Chain A: `WorldChrMan + 0x10EF8` (LocalPlayer) → `[+0]` subA → floats at `+0x6B0/+0x6B4/+0x6B8`.
   - Chain B: `player + 0x58` → `+0x10` → `+0x190` (physMod) → `+0x68` (x,y,z).
   - **Underground log:** `subA` non-null but A = `X=-nan Y=-nan Z=-0.0`; `phys = 0x0` (chain B null) → B fails. So both candidates return NaN/garbage underground.
   - AOB: `WCM_FINDER` (resolved PASS).

2. `get_player_map_pos` (goblin_inject.cpp ~914): MapId singleton `+0x2c` getter → area + tile; map-pos/geom manager `+0x70/+0x74` = "block-local X/Z".
   - **Underground log:** `area=12 gx=2 gz=0 raw=(519,-0)` → tile (2,0), local ≈ (7,0) = a coarse ORIGIN, NOT the real Mimic-Tear/Nokron location. The block-local fields read ~origin underground; the float is leaf-block-local garbage; the MapId tile is coarse (gridX 1/2).
   - AOBs: `PLAYER_MAPID_SLOT`, geom-mgr slot (shared with goblin_collected) — both resolved PASS.

## The ask

Find a reliable source of the underground player world position and how to map it to the
unified marker frame. Likely directions:
- The correct WorldChrMan offset chain for the player's PHYSICS/world transform that is
  valid underground (chain B's `+0x58/+0x10/+0x190/+0x68` gives a null physMod
  underground — wrong offset for this game version, OR a different node holds the
  underground transform).
- OR the map-block-local coordinate + the m12/m40-43 block origin, so
  `world = block_origin + local`, then feed through the same projection the markers use
  (`marker_world_pos` with `conv_underground=true`). Need: which field holds the FINE
  block-local position (not the coarse `+0x70/+0x74`), and the per-sub-page block origin
  (Siofra/Ainsel/Nokron are different layers at the same overworld X/Z → discriminated by
  MapId sub-page / tabId; see `tab_for_tile`, `GRACE_ANCHORS.tab_id`).

## Deliverable

Offsets (AOB-anchored, never hardcode RVAs — they drift per patch) + a short note on the
frame, so `get_player_map_pos`/`get_player_world_pos` can return a correct underground
position. Then re-enable distance-adaptive underground (remove the `!(open_grp & 1)` gate;
the tile/sub-page helpers `player_map_tab` / `grace_anchor_tab` / `grace_anchor_tile` are
already in `goblin_inject.cpp` for the tab gradient).

See also: `docs/re/re_findings_playerpos.md`,
`docs/re/windows_re_briefing_playerpos_questbrowser.md`.
