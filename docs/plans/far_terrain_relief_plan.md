# Far-terrain / whole-map relief — implementation plan (offline collision bake + incremental override)

The far-field companion to `heightfield_relief_plan.md` (Track D2, near-field live raycast). Goal: a
**whole-map** ground-height relief (hillshade) on the vmap, beyond the loaded-collision radius, **mod-agnostic**.

Grounded in the RE that reshaped the original prompt (`docs/re/far_terrain_heightmap_re_findings.md`):
**there is NO far-terrain heightmap texture** — ER terrain is FLVER meshes, and the walkable-ground truth is
the **`hkxpwv` map collision** (`hknpCompressedMeshShape`). This plan bakes that offline into a base
heightfield and layers authored + detected overrides on top, so runtime cost is proportional to the *diff*,
never the whole map.

---

## 0. Two layers — this plan is ONLY the map-relief layer
`virtual_world_multi_world_design.md` separates them; keep them separate:
- **Layer 1 — map RELIEF (this plan):** the 2.5D hillshade backdrop on the vmap. Doable now.
- **Layer 2 — a WALKABLE 3D world:** real streamed geometry+collision in a reserved mapId = the geom-ADD /
  MSB / streaming frontier (pivot 2, `add_collision`). **Out of scope here.** A custom world's *relief* (L1)
  is easy; making it *exist under the player's feet* (L2) is the wall.

## 1. The source decision (settled)
- **Bake source = `hkxpwv` map collision**, NOT the render FLVER. Rationale (RE-verified,
  `far_terrain_heightmap_re_findings.md` §5b): collision IS walkable ground by construction → **no false
  relief** (no props/decoration/multi-layer), plain triangle soup, `hknpMaterialLibrary` filters water/non-
  ground, and it is the **same `hknpCompressedMeshShape` the near-field raycast hits** → seamless near↔far.
- **`hkxpwv` exists on disk at full detail** even where far collision isn't streamed at runtime (the reason
  the raycast misses it) → whole-map coverage offline.
- Format: Havok tagfile → `hknpPhysicsSceneData` → `hknpPhysicsSystemData` → `hknpCompressedMeshShape`
  (quantized verts + `hkcdStaticTree` BVH + tri indices) + `hknpMaterialLibrary`.

## 2. The pipeline (the whole architecture)
```
OFFLINE (build step, dev machine)                 RUNTIME (in-DLL, cheap)
─────────────────────────────────                 ──────────────────────────────
parse hkxpwv (SoulsFormats/HKLib/Blender)  ──►  base_heightfield.bin  ──►  load (mmap, ~10–30 MB)
  → dequantize verts, world-transform (MSB)                                 + per-tile hash manifest
  → rasterize walkable tris → grid (R16 Y)                                       │
  → per-tile content hash                                                        ▼
                                                          sample(x,z) = override? patched : base
                                                                 ▲                        ▲
                                            near field: LIVE raycast (Track D2, exact) ───┘
```
- **`heightfield(dimension) = base_bake ⊕ overrides`.** Base covers everything; overrides patch only what
  changed. Sampling is O(1) (grid lookup).

## 3. The override model (why runtime cost ∝ diff, not map size)
Two DIFFERENT override sources — do not conflate:
1. **MFG-authored `.toml` content (free).** Moves/adds authored by MFG's own bundles/vworld. Heights are
   **known by construction** (we placed them) → patch the grid directly, **no parse**. A moved *prop* = no-op
   on ground; a moved/added *terrain* piece = re-rasterize its footprint from authored data.
2. **Third-party ER mod file override (detected, rare).** Another mod's loose overlay replaces a tile's
   `hkxpwv`. Detect per-file: does the loose overlay carry that tile (existence + hash vs the manifest;
   the loose-over-packed reader already exists). Re-derive **only** the changed tiles (ms each, one-time,
   cached). Mods seldom reshape terrain → usually zero tiles.
- **New parallel world (degenerate case):** no ER base to override → base = ∅; the vworld's relief comes
  **entirely from its bundle** — marker-only (no heightfield), a supplied heightmap PNG, a procedural grid,
  or greybox `add_collision` heights (authored → known). Same rasterize engine, different input. **A world
  you author never needs a parse** — you wrote the heights.

## 4. In-DLL vs build-step split (the key that avoids the FLVER/Havok frontier)
- **Build-step (offline):** the Havok `hkxpwv` parse + dequantize + rasterize → base heightfield. Use
  **existing community tooling** (SoulsFormats HKX / HKLib, or a Blender ER-collision import). **No in-DLL
  Havok parser for the base.** Ship `base_heightfield.bin` + the hash manifest.
- **In-DLL (runtime):** only (a) load + sample the baked heightfield, (b) the near-field raycast (Track D2),
  (c) patch authored `.toml` overrides (free), (d) — deferred/optional — detect + re-derive third-party
  overridden tiles. (d) is the ONLY thing that would need an in-DLL Havok parser, and only for the rare
  terrain-reshaping mod; ship without it first.

## 5. Avoiding false relief (locked in by the source choice)
- Source = collision (walkable ground) → props/decoration/interiors excluded by construction.
- `hknpMaterialLibrary` → drop water/lava/non-ground materials.
- Multi-layer (caves/bridges): single-value rule = **topmost walkable** per cell (or match the raycast).
- Near↔far consistency: both are `hknpCompressedMeshShape` walkable → no seam at the streaming boundary.

## 6. Slices
- **D-far 0 — offline baker (build step).** A dev tool (C#/Python w/ SoulsFormats-HKX, or Blender) that reads
  `hkxpwv` for one dimension (overworld m60 first), dequantizes, world-transforms via MSB, rasterizes walkable
  tris into an R16 heightfield + writes the per-tile hash manifest. Deliverable: `base_heightfield.bin` for
  the overworld + a spec of its format (extent, cell size, world→cell map = the marker frame
  `worldX=mapU+7040, worldZ=-mapV+16512`).
- **D-far 1 — in-DLL load + sample + render.** Load the baked heightfield; `sample(x,z)→Y`; hillshade it on
  the vmap (reuse the Track-D2 hillshade renderer). Blend with the live near-field raycast where loaded.
- **D-far 2 — authored `.toml` overrides.** Patch the grid from bundle/vworld authored deltas (add/move
  terrain, or a supplied heightmap PNG for a custom world). Free (no parse).
- **D-far 3 (optional, deferred) — third-party override detection + re-derive.** Hash-scan the mod overlay's
  map files vs the manifest; for changed tiles, re-derive (needs the in-DLL Havok parser). Only for
  terrain-reshaping mods; log baked-vs-rederived (no silent staleness).
- **Sea level (cheap, any slice):** `GXWaterHeightMap`/`GXWaveTerrain` or a global constant to classify
  far cells sea-vs-land (prompt #5).

## 7. Reuse / frontier / risks
- **Reuse:** the near-field raycast (`goblin_heightfield`, Track D2) for the near field + hillshade renderer;
  the dvdbnd/Oodle reader + loose-over-packed resolution (`read_game_file_decompressed`) for tile detection;
  MSB-read (already RE'd) for world-transforms; the marker-frame projection.
- **Frontier touched:** the offline Havok `hkxpwv` parse (community-solved, offline → not a blocking wall).
  In-DLL Havok parse (slice D-far 3) is the only genuinely-new in-process code, and it's deferred/optional.
- **Risks:** (a) baked base is a snapshot → the prime-directive tension; mitigated because runtime-disk WINS
  per-tile where a mod diverges (hash detect), same pattern as loose-over-packed — but the hash-check MUST be
  reliable (a miss = serving vanilla terrain on a reshaped mod = silent wrong; log served-source).
  (b) coordinate frame: MSB/Havok is block-local; bake in the same frame the vmap/raycast use or it lands
  offset. (c) memory/size: whole-map R16 at a chosen cell size — pick resolution vs footprint (target
  ~10–30 MB). (d) DLC/underground = separate dimensions/archives → separate baked fields (per
  `virtual_world_multi_world_design.md` PIVOT: dimensions are worlds).

## 8. Prime-directive reconciliation (assumed, not hand-waved)
Baked = fast path ONLY where nothing changed; runtime-disk wins per-tile where the mod diverges (D-far 3) and
authored `.toml` wins where MFG places content (D-far 2). This is the loose-over-packed pattern applied to
terrain. Conditions: base derived by the offline baker (reproducible, not hand-drawn); reliable hash-check;
log baked-vs-rederived-vs-authored so staleness is never silent.

## 9. Open items / first brick
- **First brick = D-far 0** (offline baker for the overworld) — it de-risks everything and produces a
  shippable base heightfield with no in-DLL work. Then D-far 1 (load+render) banks visible value.
- Confirm the exact `hkxpwv` per-tile naming + the MSB transform for map pieces (community-known; verify on
  the ERR install).
- Decide the heightfield format (extent/cell size/endianness) in D-far 0's spec.
- Cross-ref: `heightfield_relief_plan.md` (near field), `imgui_only_map_plan.md` (Track D umbrella),
  `docs/re/far_terrain_heightmap_re_findings.md` (the source RE), `procedural_map_derivation_design.md`
  (Convergence-trap: read the active install, never hardcode).
```
