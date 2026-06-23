# RE brief — player position → world-map frame (the "you are here" transform)

**Goal:** find the EXACT transform from the player's runtime position to world-map space, by
decompiling the function that draws the native yellow "you are here" dot. We already have the
player-position *field* (runtime-confirmed); what's missing is the *frame / transform* so we can
place our own player dot on BOTH the open world map and an in-game minimap with one value.

App 2.6.2.0 / ERR 2.2.9.6. Static Ghidra is the right tool here (we need the math, not another
runtime address). Supersedes the §4 "open item (frame)" of
`docs/re/windows_player_pos_RESOLVED_re_findings.md`.

---

## What we already KNOW (runtime-confirmed, don't re-derive)

- **WorldChrMan** = `[eldenring.exe + 0x3D65F88]`. **LocalPlayer** = `[WorldChrMan + 0x1E508]`.
- **Player position field** = `float [LocalPlayer + 0x6C0 / +0x6C4 / +0x6C8]` (X / Y-height / Z).
  Confirmed LIVE in-process: it tracks movement and updates while the map is CLOSED. (A 2nd render
  copy sits at +0x6D4/+0x6D8/+0x6DC.)
- **In-game reading (overworld, Smoldering Church):** player MapId tile = `(gridX=46, gridZ=40)`;
  `[LocalPlayer+0x6C0]` X≈ **-69.7**, Y≈151.3 (height), Z≈ **3.8**. These are SMALL → they look
  **tile-local** (NOT absolute world, which would be ≈ 46*256 = 11776-ish). So the player position
  appears to be local to the current overworld map tile, needing `tile*256 + local` to reach the
  marker frame — but this is UNCONFIRMED and is exactly what we need decompiled.
- **Our marker → map-space transform (agent-confirmed, EXACT):** a marker's unified world
  coordinate is `worldX = gridXNo*256 + posX`, `worldZ = gridZNo*256 + posZ` (from
  WorldMapPointParam, after legacy-dungeon projection), and map-space is
  `mapU = worldX - 7040`, `mapV = -worldZ + 16512` (origin (7168,16384), bias 128, scale 1, Z
  flipped). We project mapU/mapV with the live pan/zoom to the screen.
- The native MapId singleton gives the player's reliable overworld **tile** `(gridX,gridZ)` =
  `(mid>>16)&0xff, (mid>>8)&0xff`, area = `(mid>>24)&0xff` (60 overworld, 61 DLC-OW, 12 base UG).

## What we NEED (decompile + report)

The native world map draws a yellow **"you are here"** marker at the player's position. Find the
function that computes its position and report the full chain:

1. **The world→map-UV function.** Prior leads from earlier RE in this repo (verify / supersede):
   `FUN_140d82770` (suspected world→map-UV), a cursor/position provider at `cursor+0x90`, ctor
   `FUN_1409bc5b0` / setup `FUN_1409be5e0`. Decompile whatever actually maps the player position to
   the world-map dialog's UV / draw coordinates.
2. **Which player field it reads** — confirm it's `LocalPlayer+0x6C0` (or which offset/chain), and
   whether it combines that with the **MapId tile** (i.e. `tile*256 + local`) or with any per-area
   origin / block table.
3. **Is the player position tile-LOCAL or ABSOLUTE world?** Decisively. If tile-local, what tile/
   block index does it pair with and where does that come from. If there exists a single ABSOLUTE-
   world position field we can read directly (so no tile bridge is needed), give its offset.
4. **The exact transform constants** (the game's world→map-space affine) so we can compare to our
   marker transform (`mapU = worldX - 7040`, `mapV = -worldZ + 16512`). Ideally the player path and
   the marker path resolve to the SAME map-space frame; confirm or give the delta.
5. **Overworld vs underground.** If visible: how the same function handles base underground (area
   12) and DLC (area 61/40-43) — does it switch transforms per area (the "eyeball" for DLC, the
   unified underground layer for m12)? Overworld is the priority; note the rest if cheap.

## Deliverable

- The offset chain + transform as pseudocode, e.g.
  `mapU = (tileX*256 + [LocalPlayer+0x6C0]) - 7040 ; mapV = -(tileZ*256 + [LocalPlayer+0x6C8]) + 16512`
  (or whatever it actually is), so we can drop it straight into `get_player_map_pos`.
- A worked example: for the in-game reading above (tile 46,40; local -69.7 / 3.8) give the map-space
  (mapU,mapV) the game's function would produce, so we can validate our port against it.
- Note the function RVAs/AOBs touched (we resolve statics by AOB; RVAs drift per patch).

## Why this serves both consumers

One correct player→map-space value (or world-absolute value in the marker frame) feeds BOTH: the
open world map (project the same as markers → "you are here" dot) and a player-centred minimap
(`screen = centre + (marker.world - player.world) * scale`). That's the single source we want.
