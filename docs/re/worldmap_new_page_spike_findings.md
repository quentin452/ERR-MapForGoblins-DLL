# New worldmap PAGE (for a custom "dev world" map entry) — spike findings

Spike (2026-07-04) for the user goal: a new map ENTRY so MapForGoblins markers show on a custom world.
Read-only research over `docs/re/*` + `src/worldmap/*`. **Verdict: a NATIVE new page is an unsolved WRITE
frontier; a MOD-OWNED virtual page is the achievable path and fits World Virtualization (vision #1).**

## The worldmap page model (read side — fully RE'd)
- **No clean engine "page" struct.** A page = a converter slot + a page-id byte, resolved live:
  - Open page read from the dialog: `dialog+0xA88` (page: 0=base, 10=DLC) + `[dialog+0x2B68]→+0xB8`
    (layer: 0=surface, 1=underground). `dialog = cursor − 0x2DB0`
    (`windows_world_to_mapspace_projection_re_findings.md`, `goblin_worldmap_probe.cpp:1101`).
  - Mod "group": `group = isDLC*2 | isUG` → 0/1/2/3 (`src/worldmap/marker_layer.hpp:39`); the renderer draws
    only the OPEN group's markers (`src/worldmap/map_renderer.hpp:4`).
- **The page SET = a converter array built live in `CS::WorldMapViewModel`** (NOT a standalone param table):
  `WorldMapViewModel+0xF8` = converter array (stride 0x30, 8 slots), count `+0x280`, vtable `0x2ad82e0`;
  page-byte table `DAT_142ad82f8 = [00 01 0a]` (overworld / base-UG / DLC). Filled in the VM ctor
  `FUN_1408855b0` from live `WorldMapLegacyConvParamGroup` — **regulation-driven, rebuilt per VM.**
- **Per-page projection inputs** (converter fields): `originX+0xC / originZ+0x14 / biasX+0x18 / biasZ+0x1C /
  scale+0x20 / legacyConvNode+0x28` + packed area/grid key `+0x08`. World→map-space:
  `mapX=(worldX−originX)*scale+biasX`, `mapZ=−(worldZ−originZ)*scale+biasZ` (Z flipped). Static full-map
  bounds `[0,0,10496,10496]` set in the `WorldMapArea` ctor (`world_map_projection_re_findings.md`).
- Supporting params (all READ / regulation-editable): `WorldMapLegacyConvParam` (area fold),
  `WorldMapPieceParam` (fog/reveal), `WorldMapTile` (art sheet streaming, `tileId=group*10000+gx*100+gz`),
  `WorldMapPointParam` (the marker rows). Page-SWITCH is RE'd + runtime-confirmed
  (`FUN_1409c1fc0(dialog,page)` base⇄DLC, `FUN_1409c7900(dialog,layer)` surface→UG).

## What a NATIVE new page would need — all WRITE-side UNKNOWN
| Component | Governing data | Write status |
|---|---|---|
| Page exists in projector | new converter slot in `WorldMapViewModel+0xF8` (bump `+0x280`) + a page byte in `DAT_142ad82f8` | ✗ regulation-ctor-built, no write path |
| Coord bounds / projection | new converter origin/scale/bias + `WorldMapArea` rect | ✗ |
| Area→page routing | `WorldMapLegacyConvParam` rows | param editable, but wiring a NEW page id UNKNOWN |
| Menu tab / selectability | dialog list objects + availability byte (`dialog+0x27c8` = DLC) | registration path UNKNOWN |
| Tile ART image | new `WorldMapTile` sheet + textures in the ERR `02_120_worldmap.gfx` Scaleform movie | UNKNOWN |
| Fog/reveal | `WorldMapPieceParam` + `openEventFlagId` | param editable |

⇒ A native page is **not a light data-layer task** — it is its own write frontier (the converter array is
engine-internal, built in the VM ctor from regulation; no injection path is RE'd), adjacent to the MSB wall.

## ★ The achievable path: a MOD-OWNED virtual page (recommended)
**For the mod's own overlay markers, a native page is NOT required** — the overlay projects + draws in the
backbuffer itself and only needs the open-page/group id to cull (`map_renderer.hpp:4`). So the fast, fully
mod-controlled route to "a new map entry with MFG markers" is a MapForGoblins-OWNED virtual page:
- A synthetic **group id** (e.g. ≥100) for the custom world's markers (extend the `group` model in
  `marker_layer.hpp`), so markers can be tagged to it and culled like the native groups.
- A mod-defined **projection** (origin/scale/bias) for the custom world's coordinate space — same math the
  probe already runs, just mod-supplied constants instead of the live converter.
- A **mod-drawn map surface**: a background image (custom or blank grid) + pan/zoom, drawn by the overlay
  (ImGui/D3D), openable via a mod UI toggle — instead of relying on the game's Scaleform map being open.
- Ties into **World Virtualization (vision #1)**: a custom world = a bundle whose map is this virtual page.

Effort: a real feature (a mod-owned map view + pan/zoom + a group/projection plumbing), but **all in mod
code, no engine write, Linux-doable** — vs the native page which is blocked on an unsolved write frontier.

## Recommendation
Pursue the **mod-owned virtual page**, not the native one, for the markers goal. Reserve native-page
registration as a far-frontier item (only if the custom world must appear as a real in-game map TAB). Next
concrete step if the user confirms: design the virtual-page slice (group id + projection + the mod-drawn
surface + open toggle), starting with the marker-group plumbing (cheapest, unlocks tagging markers to it).
