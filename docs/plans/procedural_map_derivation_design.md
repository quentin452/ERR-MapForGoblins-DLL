# Procedural map derivation vs the "Convergence trap" — design note

Scoped 2026-07-04. **Not an implementation plan — a design/risk decision** that constrains the map-tile
work (`map_tile_loading_plan.md`) and the virtual-world map (`virtual_world_multi_world_design.md`).

## The problem (the "Convergence trap")

Big ELDEN RING overhauls (e.g. Convergence) reshape the 3D world — move/add/remove content — and then have
to **re-author the 2D world map by hand**, because ER's map is not derived from the 3D world at runtime; it
is a set of frozen artifacts baked once from vanilla. Three separate things must be hand-edited when the 3D
world changes:

1. **Tile ART** — the painted relief in `menu/71_MapTile.tpfbhd/.tpfbdt` (baked DDS, one per grid cell/LOD).
2. **Point / label params** — `WorldMapPointParam`, `WorldMapPlaceName` (grace dots, region names, POIs).
3. **The projection converters** — `WorldMapLegacyConvParam` + the per-dimension converter (origin/scale/
   grid-base/bias) that maps a world XZ in some area to a 2D map-space XY (with the underground/legacy fold).

The question this note answers: **for what MapForGoblins does, can we derive the map procedurally from live
data so we DON'T inherit that manual-2D-editing burden — or not at all?**

## Split the answer: OBJECTS (derive) vs TERRAIN ART (don't)

### Objects — already derived procedurally, and correctly

The things MFG actually modifies or shows — markers, POIs, loot, graces, and (future) moved/added
placements — are positioned on the map by projecting their **live** world coordinates through the **engine's
own live converter**, not through any baked table:

- `map_renderer.cpp:872 project_marker` → `goblin::worldmap_probe::project(area, gx, gz, px, pz, …)` runs
  against the **live `CS::WorldMapViewModel`** (read from the ACTIVE regulation), folding `LegacyConv` per
  dimension exactly like the native map. So if a mod changes the converter or the point params, MFG follows
  automatically — no hand-editing. This covers trap items **(2)** and **(3)**.
- `move_asset` already reads a moved geom instance's live world matrix (`inst+0x220`); projecting that through
  the same converter yields the correct 2D position with zero extra work. A future **ADD-geom** placement is
  identical: it has a transform → project it → it appears on the map. **The objects we modify are procedurally
  derivable, and the marker system already does it.**

There is a **baked FALLBACK** — `world_to_mapspace_xy` (`map_renderer.cpp:854`) hardcodes the vanilla
overworld affine (`mapX = worldX − 7040`, `mapZ = −worldZ + 16512`, scale 1). It is used only until the live
VM resolves or for areas the engine won't place. It is NOT the primary path — but see the risk below.

### Terrain ART — cannot be derived, and that's acceptable

The painted relief backdrop is hand-authored cartography. We cannot cheaply regenerate it to match FromSoft's
style. But we don't need to:

- **Read the ACTIVE install's tiles** (`maptile.cpp` via `read_game_file_decompressed`, loose overlay →
  packed dvdbnd). If a mod repainted its 2D tiles (as Convergence did), we show the mod's version for free.
  If a mod changed 3D but NOT the 2D tiles, the backdrop is stale — but that is the MOD's omission, and our
  **objects are still correct** (live-derived on top). Strictly better than vanilla, and prime-directive
  compliant ("read the active install's real files", never a baked snapshot of our own).
- **Fallback = grid / circle** (the universal fallback) for a custom virtual world with no tiles; a mod
  author can supply their own map image.
- A top-down **hillshade from the live 3D mesh** is technically conceivable (we can read MSB/geom) but there
  is no heightfield reader RE'd, and the result would look like a raw DEM, not ER's painted map. **Not worth
  it.** Explicitly out of scope.

## ⚠ The real risk is in OUR NEW code (map-tile slices 2/3)

The trap does not come from the engine — it comes from **us re-baking constants into the tile layer**:

- Slice 2 (done) places tiles with an approximate `col*256` and does not yet use a converter.
- Slice 3, as originally sketched, would **invert the hardcoded overworld affine** (`7040/16512`) to place
  tiles. **That re-bakes the Convergence trap into MFG's own map** — tiles would be wrong the moment a mod
  moves the converter or redivides the grid.

> **Update 2026-07-06 (fd0ad45):** static Ghidra + a live 10/10 validation
> (`docs/re/windows_worldmap_affine_resident_source_re_findings.md`, `test_converter_offvm.py`) PROVED the
> base converter fields — `origin / bias / scale / gridbase / area keys` — are **exe-invariant**, NOT a
> mod-changeable param (only the legacy-dungeon fold is param-driven). So the "a mod moves the converter /
> redivides the grid" worry below does **not** apply to the base affine. `project()` /
> `get_converter_affine()` now carry an exe-invariant off-VM FALLBACK for base areas (60/61/12; `origin 0 /
> gridbase 28,64 / bias 128 / scale 1` — identical to the live slot) so base projection works map-closed.
> This does NOT recreate the trap: the LIVE converter is still preferred whenever the map is open, and the
> hardcoded values are only a fallback for the *proven-invariant* base affine. The rule below still holds
> for anything mod-variable (the legacy fold, and any future grid the tile layer must NOT re-bake blindly).

**Decision / constraint for slice 3:** derive each tile's world quad from the **LIVE converter** — the same
`worldmap_probe` / `WorldMapViewModel` converter already used for markers — never from hardcoded constants.
The converter carries `gridXbase / gridZbase / originX / originZ / scale / biasX / biasZ` per dimension
(field layout in `docs/re/windows_world_to_mapspace_projection_re_findings.md` §1), which is exactly what maps
a tile name `M{MM}_L{L}_{col}_{row}_{suffix}` to a world/map-space rect. Then **tiles and markers share ONE
live projection** → both auto-correct on any mod, and MFG never hand-edits a 2D map.

Concretely, slice 3 should:
1. Read the per-dimension converter live (already available via `worldmap_probe`); expose the affine fields.
2. Map tile `col/row` (+ `{suffix}` sub-cell + LOD scale) → map-space rect via that affine, then map-space →
   world (inverse) so the tile quad lands in the same world space as the markers (the vmap canvas is in world
   units).
3. Never hardcode `7040/16512` or overworld `scale 1` in the tile path.

## Residual baked map data to finish killing (already tracked)

To make labels/regions live-derived too (closing trap item (2) fully): migrate `goblin_name_regions` /
`goblin_region_anchors` → live `WorldMapPointParam` + `WorldMapPlaceName`. Tracked in
`baked_data_full_removal_plan.md`; not gating the tile work, but it's the last hand-frozen map-data bit.

## Verdict

For MFG's scope (markers + placement/loot edits + custom virtual worlds), **the procedural path is the right
one and is already in place for the objects we modify** (live converter, not baked tables). The only
irreducibly-baked piece is the terrain ART backdrop, mitigated by reading the active install's tiles +
graceful fallback. **The one action that actually matters: wire slice-3 tile placement onto the live
converter, not hardcoded constants — otherwise we recreate the exact trap we're trying to avoid.**
