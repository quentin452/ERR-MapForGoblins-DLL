# Far-terrain / whole-map relief — implementation plan (offline collision bake + incremental override)

The far-field companion to `heightfield_relief_plan.md` (Track D2, near-field live raycast). Goal: a
**whole-map** ground-height relief (hillshade) on the vmap, beyond the loaded-collision radius, **mod-agnostic**.

Grounded in the RE that reshaped the original prompt (`docs/re/far_terrain_heightmap_re_findings.md`):
**there is NO far-terrain heightmap texture** — ER terrain is FLVER meshes, and the walkable-ground truth is
the **`hkxpwv` map collision** (`hknpCompressedMeshShape`).

**Strategy: v0-first, heavy path GATED.** Ship a FREE relief v0 from the placement Y the disk-MSB parse
already captures (~480K samples), evaluate it in-game, and **only** build the offline `hkxpwv` collision bake
IF v0 proves insufficient (§6 USER GATE). The bake + incremental-override design (authored `.toml` = free;
third-party mod = hash-detect + re-derive) is kept below the gate — runtime cost then stays proportional to
the *diff*, never the whole map — but we likely stop at v0.

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

## 6. Slices (v0-first, then a USER GATE)
- **D-far -1 — MSB Y-cloud relief v0 (FREE, already-parsed) ← FIRST BRICK.** Tap the placement Y that the
  disk-MSB parse ALREADY captures (`loot_disk` stores `posY = pos[1]` on treasures/enemies/collectibles/
  regions/objacts/feature-assets — currently used only for the above/below-player badge). **Sample count is
  large, not ~10K** (that's the *filtered visible markers*): the ERR log (2026-07-04) shows **479,549 asset
  placements** + **26,057 enemy placements** parsed, each with a Y. **Key insight: the 476,655 "clutter"
  assets (pots/jars) MFG DISCARDS for markers are the BEST ground-Y data** — small props rest ON the ground →
  their base Y ≈ ground level. Build: keep `{x,z,y}` for the whole pre-filter placement stream (the parse
  already iterates it — just don't drop it), then a **FILTERED** cloud→heightfield (NOT naïve):
  1. **layer-separate** by tile/mapId (a basement point doesn't contaminate the field above);
  2. **vertical outlier reject** (drop points > k·MAD above the local median → towers/ledges/roofs/flying
     enemies; filter flying `c####` by NpcParam);
  3. **per-cell robust stat** = median (or min-of-topmost) of the assets in the cell;
  4. **type weighting** (ground clutter = high; big structures = low; graces = reliable anchors).
  Render via the Track-D2 hillshade path. **Zero Havok, zero new disk parse, mod-agnostic** (mods change MSB →
  auto-reflected). Coverage is **content-biased** (dense at POIs/dungeons, holes in truly-empty terrain) — the
  one honest limitation.
- **★ USER GATE (after v0 ships) — is v0 SUFFICIENT?** The user evaluates the v0 relief in-game.
  - **SUFFICIENT → STOP HERE.** D-far 0/1 (the offline collision bake) become **unbuilt / optional**. No Havok
    work at all. This is the intended happy path if the content-biased coverage looks good enough.
  - **INSUFFICIENT** (open-terrain holes too visible, too coarse) → proceed to the collision bake below.
- **D-far 0 — offline collision baker (build step) [ONLY IF v0 insufficient].** A dev tool (SoulsFormats-HKX /
  HKLib / Blender) that reads `hkxpwv` for a dimension (overworld m60 first), dequantizes, world-transforms
  via MSB, rasterizes walkable tris into an R16 heightfield + a per-tile hash manifest. Fills the open-terrain
  holes v0 can't. Deliverable: `base_heightfield.bin` + a format spec (extent, cell size, world→cell map =
  marker frame `worldX=mapU+7040, worldZ=-mapV+16512`). v0's cloud becomes its **cross-check oracle**.
- **D-far 1 — in-DLL load baked + blend [with the bake].** Load the baked heightfield; `sample(x,z)→Y`;
  blend: live raycast (near, exact) > baked collision (far) > MSB v0 (fill). Same hillshade renderer.
- **Followup — authored Y for custom `.toml` objects (hypothetical).** When bundles/vworlds place custom
  objects via `.toml`, each carries its Y (authored → exact). Feed those into the same cloud/heightfield so a
  CUSTOM world gets relief from its own authored content (the new-world degenerate case, §3): base = ∅, the
  `.toml` objects' Y ARE the surface. No parse — you wrote the heights. (Also the natural place for a supplied
  heightmap PNG / procedural grid per custom world.)
- **D-far 3 (optional, deferred) — third-party override detection + re-derive.** Hash-scan the mod overlay's
  map files vs the manifest; re-derive only changed tiles (needs the in-DLL Havok parser). Only for
  terrain-reshaping mods; log baked-vs-rederived-vs-v0 (no silent staleness).
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
- **✅ FRAME VALIDATED 2026-07-05 (`far_relief_probe` RPC, in-game ERR).** The one blocking unknown — is the
  parsed `posY` world-frame or block-local — is answered: **world-frame, directly usable, no per-tile Y
  correction.** The probe (reads the cached `g_parsed`, no new parse) buckets area-60 collectible Y by tile:
  **135,204 samples / 266 tiles; globalY [-17..1970]; within-tile relief mean 59; cross-tile |Δmedian| mean
  49, max 530**; the median-Y strip is a smooth coastal→inland gradient. cross-tile Δ ≈ within-tile spread and
  globalY spans the full elevation range continuously ⇒ not block-local. This CLEARS §7 risk (b) for the
  overworld. Also confirms coverage is content-biased: **266 of ~1681 overworld tiles (~16%)** carry samples
  → dense at POIs, holes in empty terrain (the honest limitation for the USER GATE). `far_relief_probe` is the
  reusable oracle (also cross-checks D-far 0 later).
- **First brick = D-far -1** (MSB Y-cloud v0) — FREE, no Havok, no new parse, ships visible relief. Then the
  **USER GATE** decides whether the collision bake (D-far 0) is even needed. **The heavy Havok path is gated
  behind v0 turning out insufficient** — likely we stop at v0 for a good while.
- v0 build detail: source = `g_parsed.collectibles` (+ enemies/treasures) in `map_entry_layer.cpp` — the
  clutter is skipped for markers at the `!aeg_is_gather → continue` (~L579), so tee off `{worldX,worldZ,posY}`
  for ALL placements (incl. pots/jars) from the cache BEFORE that filter, or in a dedicated pass over
  `g_parsed`. Grid → `heightfield::Cell[]` (normals from grid gradient) → the existing D2 hillshade render.
- (Only if the gate opens) confirm the `hkxpwv` per-tile naming + MSB map-piece transform (community-known;
  verify on ERR) and decide the heightfield format in D-far 0's spec.
- Cross-ref: `heightfield_relief_plan.md` (near field), `imgui_only_map_plan.md` (Track D umbrella),
  `docs/re/far_terrain_heightmap_re_findings.md` (the source RE), `procedural_map_derivation_design.md`
  (Convergence-trap: read the active install, never hardcode), `virtual_world_multi_world_design.md`
  (custom-world relief = the followup's authored-`.toml`-Y case).
```
