# Heightfield direct-read (vs raycast) — Linux probe findings (2026-07-05)

**Question:** can we read the terrain height DATA directly (a grid) instead of the per-point raycast the
relief sampler uses? Requires: is the ground an `hknpHeightFieldShape` (grid → readable) or an
`hknpCompressedMeshShape` (baked mesh → raycast-only)?

## Probe (a) — RESULT: inconclusive from Linux; the ground is block-streamed, not in the world body list
Added `hf_shape_probe` (`goblin_heightfield.cpp` tick_present, RPC): cast at the player, then scan the query
ctx / hknpWorld for hknp shape vtables (RVA `0x2ee_` neighbourhood).
- **ctx does NOT cache the hit shape** — only holds a ref to `hknpWorld` (`ctx+0x8`, vtable er+0x2eedc78).
- **2-level scan from hknpWorld found 19 hknp shapes** (dynamic bodies — player capsule / NPCs / props),
  ~15 distinct vtables: `0x2ee36b0 0x2ee5f18 0x2ee84a0 0x2eea960 0x2eedc98 0x2eedf00 0x2eedfc8 0x2eeeb00
  0x2eeeb20 0x2eed588 0x2eef0a8 0x2eef108 0x2eef138 0x2eef198 0x2eef1e0 0x2eef210 0x2eefce8`.
- **NONE match the findings' ground vtables** (`0x2ee2a18` heightfield, `0x2eeb908`/`0x2eec698` mesh).

⇒ The **static map collision (the ground) is NOT in `hknpWorld`'s direct body list** — it is **streamed
per-block** (CSWorldGeom / the `h######` MSB-Hit collision), a level below the dynamic body storage the
2-level scan reaches. The raycast hits that block collision; the query path does not hand its shape back.

## Verdict
Direct terrain-grid read is a **deeper RE than a quick probe** — it needs the **block collision structure**
(streamed per mapId block) → the ground shape → IF `hknpHeightFieldShape`, its sample-grid layout. Two ways
forward when pursued:
1. **Ghidra**: name the shape vtables above + trace what the cast's collector records as the hit body/shape
   (its result layout), so the DLL can read the hit shape directly; then confirm heightfield vs mesh.
2. **Block-collision walk (Linux)**: from CSWorldGeom / the streamed block, reach the ground collision shape.

**For now: the raycast sampler works (proven, D2.1–D2.3).** Direct-read is a perf/richness optimisation, not
a blocker — parked behind the block-collision RE. The `hf_shape_probe` diagnostic stays for the follow-up.
