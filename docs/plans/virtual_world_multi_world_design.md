# Virtual multi-world — architecture design (framework-owned worlds)

Design for MapForGoblins' custom "dev worlds" (World Virtualization vision #1): the framework holds N of
its OWN worlds and shows the active one's map + markers. Answers 4 architecture questions raised
2026-07-04. Complements `docs/re/worldmap_new_page_spike_findings.md` (native page = unsolved write
frontier → we go MOD-OWNED) and `docs/runtime_modding_framework_vision.md`.

## The unifying fact (how ER's map works)
ER's world map ART is **`WorldMapTile` sheets** — BAKED DDS texture tiles (`tileId = group*10000 +
gridX*100 + gridZ`), streamed per-view, **NOT generated at launch**. `group 0/1/2 = overworld / underground
/ DLC` are **separate sheets + separate converter slots + separate coordinate spaces** — i.e. ER already
uses a "dimension"-like separation per page. Consequence: overworld and DLC do NOT share map coordinates,
and we CANNOT generate ER-style tiles — a custom world supplies its OWN map image (or a procedural grid).

## Decision 1 — Collision avoidance: the FRAMEWORK assigns position, never the player
Authors place content **relative to their world's origin**; the framework maps that to absolute coordinates
in a reserved region. Two levels:
- **Marker-only world (current virtual page):** each world = its OWN mod coordinate namespace (never the
  engine's space) → collision is structurally impossible; the framework just keeps worlds in separate
  namespaces (trivial — a per-world origin).
- **Walkable world (future, needs ADD geom):** the framework assigns a **reserved mapId / area block** per
  world (ER's own dimension mechanism — the same way underground/DLC are physically separate map regions).
  Geometry is placed into that reserved dimension, so it never collides with base maps or other worlds.
  The player NEVER picks an absolute position — they pick a world + a world-relative spot; the framework
  resolves it.

## Decision 2 — "Which world am I in": a framework ACTIVE-WORLD state
The framework tracks an **active world id** (default = base ER). Source of truth:
- **Engine-backed (walkable) world:** the player's live **mapId** (`PLAYER_MAPID_SLOT`, already RE'd) →
  mapId→world lookup. Entering a world's reserved mapId auto-sets active world.
- **Marker-only world:** an explicit framework selection (activate a bundle via UI/RPC). You are physically
  still in some ER mapId, but the framework's active world decides which world's map+markers to show.
The virtual page reads the active-world id and renders that world.

## Decision 3 — Open with "M" (the game map key), not F1
Production UX: the virtual world map is what **M** shows when the active world is a custom world, NOT a
separate dev window.
- Hook the worldmap-open (already RE'd: `worldmap_open()` + the dialog/page state).
- When M opens the map AND active world is virtual → draw the MFG virtual page in place of / over the native
  Scaleform map (suppress or ignore the native map render for that frame set).
- Keep the current `vmap` RPC + the F1 Dev-tab toggle as a **DEV harness** (drive/inspect without being in
  a custom world). Slice A's window is that harness; production = M-triggered.

## Decision 4 — The map surface for a custom world
Since we can't bake ER tiles, a custom world's map is a MOD-DRAWN surface (slice A canvas): a supplied
background image (per-world, from the bundle) OR the procedural reference grid, plus mod-projected markers.
Per-world projection (origin/scale) lives in the world's bundle.

## Where this lands in the slices (updates the virtual-page plan)
- ✅ Slice A — the mod-drawn canvas (pan/zoom/grid). DONE.
- Slice B — draw the active world's markers on the canvas (mod projection).
- Slice C — the WORLD model: an active-world id + per-world {origin/scale, marker set, optional bg image},
  bundle-backed (extends `goblin_world_bundle`). Marker-group tagging (synthetic group ≥100) so markers
  belong to a world. Framework assigns coordinate namespaces (Decision 1, marker level).
- Slice D — M-integration (Decision 3): open the virtual page from the game map key when active world is
  virtual; the F1/`vmap` path stays as the dev harness.
- Later (walkable, needs ADD geom + a reserved-mapId allocator) — Decision 1's dimension level + Decision 2's
  mapId→world. Blocked on the geom-spawn ADD frontier (pivot 2, Windows RE).

## Open RE items (small, mostly confirm-live)
- mapId→world lookup + a reserved-mapId/area allocator (only for walkable worlds — not on the marker path).
- The exact M-open hook point to swap in the virtual page (worldmap-open is RE'd; the suppress/overlay of
  the native Scaleform map for a full-screen mod surface needs a probe).
