# MapForGoblins v2.2.0 — Merchant map pins

## ★ Merchants on the map

- **Every shop NPC of your ACTIVE install is now pinned on the world map** — new
  "World - Merchants" category, on by default. Kalé, the Twin Maiden Husks, Patches,
  Gostoc, the cave and DLC merchants… 56 pins on ELDEN RING Reforged.
- Fully **mod-agnostic, zero baked data**: a new in-DLL ESD (EzState) parser reads the
  active install's own talk scripts (`OpenRegularShop` shop ranges) and joins them with
  the MSB NPC placements at boot — a mod that moves, replaces or ADDS merchants is
  picked up automatically (ERR's own reworked Kalé and added Roundtable vendors show
  correctly, which no static merchant table could do).
- The F1 item search's "Sold by merchants" rows now **name the seller**
  ("· sold by Twin Maiden Husks") — and the seller is a searchable, locatable pin.
- Merchants share the quest-NPC map glyph; redundant auto-detected NPC pins are deduped.

## Map projection fixes (long-standing)

- **Roundtable Hold markers finally land on the map's Roundtable inset** (bottom-left
  corner), next to the native grace icon. Items, merchants and quest pins there used to
  project ~2200 map units off the artwork — the game's inset icons use hand-placed
  virtual coordinates, which marker positions are now remapped into (live-derived,
  works on any mod).
- Markers from legacy maps with no map placement of their own no longer pile up at an
  unrelated dungeon entrance.
- **Coordinate teleport works**: `warp_local`/`warp_xyz` and the Virtual Map
  click-to-warp actually move the player now (havok body write; the old write was
  reclaimed by physics every frame).
- `vmap graces` reported every grace as discovered on a fresh save; it now reads the
  live discovery flags.

## Dev / RPC additions

- `coop` (session diagnostics), `hp` / `immortal` (dev god-mode), `mem_write` /
  `mem_scan_f3` (runtime-RE levers).

Full changelog: `docs/changelog.md` [v2.2.0].
