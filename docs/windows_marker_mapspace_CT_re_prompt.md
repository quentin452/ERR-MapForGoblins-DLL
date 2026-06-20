# Task: pin the world→render MATRIX (incl. rotation) + deliver a Cheat Engine table

You are on **Windows** with **Ghidra/IDA** + **Cheat Engine (with the game running)** + x64dbg.
Repo: **ERR-MapForGoblins-DLL** (Elden Ring world-map mod), app **2.6.2.0 / ERR 2.2.9.6**,
imagebase `0x140000000`. Resolve by AOB; RVAs are reference for this build. Analyse the MSVC
`.text` at `0x140001000`, NOT the VMProtect `.text`.

Read first: `docs/marker_to_mapspace_re_findings.md` (the prior RE — render = (world−origin)·0.5,
per-page origin) and `docs/world_map_projection_re_findings.md` (screen projection — DONE).

## Why this round

We are projecting baked `WorldMapPointParam` rows into the map's render space ourselves. The
screen step is solved. For the world→render step the prior RE gave `render = (world − origin)·0.5`
with a per-page origin, but **by-hand calibration FAILED**: at the right scale the grace
constellation comes out **ROTATED** vs the real map (and a manual 0/90/180/270 + mirror search
couldn't recover it — rotating around the wrong pivot threw markers off-screen). So the real
transform is an **affine with a non-identity 2×2 matrix**:

```
render = M · world + T        // M = scale·Rotation (maybe reflection), T = per-page translation
```

M (the rotation/scale, likely shared) and T (per-page) are NOT lifted statically — the placement
happens in the Scaleform thunk `thunk_FUN_1457cd3df` inside the VMProtect region. We need to
**observe it at runtime** and hand the user a Cheat Engine table to verify/extract it live.

## Measured anchors (ground truth to check against)

| grace | dstPage | baked world (gridX·256+pos) | reticle render (+0x104,+0x108) |
|---|---|---|---|
| Dragonbarrow (area 21) | 61 | (≈12338, ≈11985) | (6018.75, 6187.28) |
| Academy Gate (area 16) | 60 | (≈9217, ≈13617) | (≈1826, ?) |

`+0x104/+0x108` on the cursor (`CS::WorldMapCursorControl`, vtable `0x142b29a90`) is the live
render coord (the probe `goblin_worldmap_probe.cpp` already reads it). `WorldMapArea` = cursor+0xF0,
pan `+0x378`, zoom `+0x380`, fullRect `+0x350` `[0,0,10496,10496]`.

## Goals

1. **Resolve `M` and per-page `T`.** Breakpoint the placement (`thunk_FUN_1457cd3df`'s real target;
   the icon update called from build `FUN_140a82a80` / reconcile `FUN_140a832a0`). Read the
   `Scaleform::Render::Matrix2x4<float>` (or the inputs/outputs) it applies for one marker → recover
   the 2×2 (rotation+scale, confirm the ~90° + any reflection + the ×0.5/×20-twips) and the
   per-page translation. Verify against the two anchors above (apply M·world+T → must equal the
   measured reticle render).
2. **Deliver a Cheat Engine table (.CT)** the user can run live to SEE the transform — the main
   deliverable, so the user can verify/extract origins per page without blind calibration:
   - **Address list** (AOB/pointer-path, auto-resolving): live cursor `+0x104/+0x108` (render),
     `WorldMapArea` pan/zoom/fullRect, the current page id (`menu+0x151` / `DAT_143d6cfc3`), and —
     if a readable matrix/origin exists per page — those fields.
   - **A logging script** (CE Lua or AA `{$lua}`) that hooks the placement and logs, per built
     marker, the `(world_in)` → `(render_out)` pair (+ its areaNo/page), so hovering or opening a
     page prints the real mapping. From a handful of pairs across pages the user reads off M + T.
   - Labeled, copy-pasteable: the user opens the CT, attaches to eldenring.exe, opens the map,
     and watches the values / reads the log.
3. **Recipe** (in the CT or a short .md): step-by-step for the user — attach, open map, hover a
   known grace, compare `world_in` vs `render_out`, confirm M (rotation/scale) is page-independent
   and T per-page; export the per-page T table.

## Deliverables format

- `MapForGoblins_mapspace.CT` (Cheat Engine table: address list + the hook/log script).
- The recovered `M` (2×2 + the twips/scale factor) and per-page `T` as a small constants table
  (so we bake `render = M·world + T` in `marker_world_pos`/the overlay).
- AOBs for every handle used (cursor vtable scan already exists; add the placement hook AOB + any
  matrix/origin field path). RVAs as reference.
- Note Z/Y sign + the page-id field to draw only the open page's markers.

## Notes
- The mod's probe (`debug_worldmap_probe`) + overlay (`overlay_markers_proto`, solo/L/K tooling)
  already read the live render coord and can apply a candidate `M`/`T` for instant visual check —
  the CT is the independent ground-truth source to derive them.
- VMP: if the placement thunk can't be statically lifted, the CT's runtime hook (CE can bp into VMP
  code) is exactly how to read the matrix — that's why the deliverable is a CT, not just a formula.
- Offsets are version-specific; resolve by AOB.
