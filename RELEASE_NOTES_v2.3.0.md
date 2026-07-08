# MapForGoblins v2.3.0 — Death markers & safe teleports

## ★ Death marker (bloodstain) fidelity

- **Your bloodstain now shows on the NATIVE world map too** — above the other icons,
  projected through the game's own converter so it lands correctly on underground and
  DLC pages (it used to draw only on the Virtual Map and the minimap).
- **0-rune deaths are no longer invisible**: dying with no runes still leaves a real
  recoverable-spot record in the game, and ER's own map draws its icon there — the mod
  used to treat "0 runes" as "no bloodstain" and hide its marker. Existence now follows
  the engine's own bloodstain flag (set on any death, restored from the save), so the
  mod's DropSoul marker mirrors ER exactly, on every surface.

## Teleport safety (no more poisoned saves)

- Coordinate teleports (`warp_local`/`warp_xyz`, Virtual Map click-to-warp) could drop
  the player where no ground exists — a free-fall whose autosave then wedged EVERY
  subsequent load on the loading screen. Three guards now apply:
  - transiently invalid player-map reads are rejected instead of mis-framing the jump;
  - the target column is **live-checked with a physics raycast** — void, deep water and
    unstreamed ground are refused at any distance;
  - when the live check can't run, a conservative 1500 m cap applies.
- Already-poisoned save? Restore the newest healthy snapshot from the launcher's
  `ERR Backups/` folder next to the save file (documented recovery).

## Streamed far teleport

- Coordinate teleports beyond the streamed bubble now route through the game's OWN
  streaming instead of being refused: fast-travel to the nearest discovered grace, then
  one ground-checked hop to the exact target. New `warp_far <worldX> <worldZ>` RPC; the
  Virtual Map click-to-warp falls back to it automatically.

## Dev / RPC additions

- `ground_at <x> <z>` (walkable-ground existence check), `bloodstain_probe [dbg]` /
  `death_state` (bloodstain + death-marker diagnostics).

Full changelog: `docs/changelog.md` [v2.3.0].
