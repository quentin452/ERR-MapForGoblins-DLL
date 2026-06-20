# Windows RE — the device-independent world-map view centre (map-space → screen)

**App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. Cheat-Engine + running game.**

The overlay projects markers onto the open world map. The **world → map-space** stage is
SOLVED (converter constants, `marker_to_mapspace_re_findings.md`: `mapX = worldX − 7040`,
`mapZ = −worldZ + 16512`). What's left is **map-space → screen px**, and the current field we
use for it is WRONG. Need the correct, **input-device-independent** view centre (or the
engine's projection).

## The projection + what's broken

Screen projection (from `world_map_projection_re_findings.md`):
```
screenX = (markerU − centreU) · zoom · scale + screenW/2 + bias
screenY = (markerV − centreV) · zoom · scale + screenH/2 + bias
```
- `zoom` = `CS::WorldMapArea + 0x380` (live, correct).
- `markerU/V` = the marker's map-space coord (solved).
- **`centreU/V` is the problem.** We used `cursor + 0xFC / +0x100` as the view centre. LIVE
  RESULT: it is **RETICLE-COUPLED** — when the mouse (or gamepad stick) moves, the markers
  FOLLOW the reticle instead of staying fixed on the map. With the mouse held still they look
  roughly right; with a gamepad they slide with the cursor. So `+0xFC/+0x100` is NOT the
  stable view centre.
- Tried `centre = pan + (screenW/2)/zoom` with `pan = WorldMapArea + 0x378/+0x37C` →
  **WRONG position for ALL markers** (so the pan→centre relation isn't that, or pan isn't in
  the same space/sign).

## What to find (either solves it)

**(A) The stable view-centre field** — the map-space coordinate that sits at the SCREEN CENTRE
of the map widget, that updates when the view PANS (mouse-edge-scroll OR gamepad) but is NOT
the reticle. Likely on `CS::WorldMapArea` (`cursor + 0xF0`) or the WorldMapDialog
(`cursor − 0x2DB0`). Method: open the map, note a marker's fixed map-space coord; pan with the
GAMEPAD; delta-scan WorldMapArea + dialog for the two f32 that change by the pan amount and,
combined with zoom, reproduce the marker's on-screen pixel. Confirm it's identical for mouse
and gamepad panning. Give its offset + the exact `screen = f(marker, centre, zoom)`.

**(B) OR the engine's world→screen projection** (pixel-exact, device-independent by
construction). Leads from prior notes: Scaleform `localToGlobal` `FUN_14117d180` /
`FUN_140d82070`, and `FUN_140d82770`. Find the call that maps a map-space point (or a
`WorldMapPointIns`) to screen px; give a C++-callable signature (this=?, args=?) or the
matrix it reads. Render-thread-only is fine — we call it from the Present hook.

## Known-good anchors (already resolved — reuse, don't re-derive)

- Cursor O(1): `CSMenuMan` static slot `base+0x3d6b7b0` → WorldMapDialog → `+0x2DB0` = cursor
  (`CS::WorldMapCursorControl` vtable RVA `0x2b29a90`).
- `cursor + 0xF0` = `CS::WorldMapArea`: pan `+0x378/+0x37C`, zoom `+0x380`, fullRect `+0x350` =
  `[0,0,10496,10496]`.
- `cursor + 0x104/+0x108` = the reticle's map-space coords (track the cursor/mouse).
- `cursor + 0xFC/+0x100` = the reticle-coupled field we wrongly used as centre.
- Open-region getter (solved): page `dialog+0xA88` (0 base / 10 DLC), layer
  `*(u8*)(*(void**)(dialog+0x2B68)+0xB8)` (0 surface / 1 underground).

## Deliverable

The offset of the stable view-centre (A) + the exact screen formula, OR the projection
function (B) signature. Live-verify: a KNOWN grace's map-space coord projects onto its native
icon and STAYS there while panning with BOTH mouse and gamepad, across zoom. Then we bake it
into `project_uv` (goblin_overlay.cpp), replacing the `cursor+0xFC` centre. There is a `Y`
hotkey in the overlay that A/B-toggles centre candidates for quick testing.
