# Windows RE brief — pin the world→render affine `M·world + T` by HOOKING the placement

**App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. Cheat-Engine-capable Windows RE
agent.** Goal: deliver the exact world→render transform the world-map icon build uses, so
the overlay can place goblin markers on the real map. Read-only RE (we never alter the
game's render; we observe it to recover the math).

## The transform we need

```
render = M · world + T
   M = [[a,b],[c,d]]   2×2 scale·rotation, SHARED across pages (hypothesis)
   T = (e,f)           per-page translation (pages 60, 61, 12, 40, 41, 42, 43)
   world  = (gridXNo·256 + posX,  gridZNo·256 + posZ)     // CELLSIZE 256
   render = page-space coord (fullRect [0,0,10496,10496])
```

The **render → screen** step is already solved and pixel-exact
(`world_map_projection_re_findings.md`: `screen = (render − viewCentre)·zoom + canvas/2`,
scale 1.0). The render → screen probe and overlay are done. **Only `M` and per-page `T`
remain.**

## Why a HOOK (not the cursor-hover recipe)

Two prior routes are insufficient:
- **By-hand diagonal** (`render = (world − origin)·0.5`) — FAILED: the grace constellation
  comes out **rotated**, so `M` is a non-identity 2×2 (scale·**rotation**), not a diagonal.
  See `marker_to_mapspace_re_findings.md` §0.
- **Cursor-hover snapshot** (`marker_mapspace_CT_recipe.md`, and the in-DLL `solve_affine`
  in `goblin_overlay.cpp`) — reads the **cursor** render coord (`+0x104/+0x108`) while
  hovering a grace. Exact only if the reticle is perfectly snapped on the icon; hand-hover
  adds a few render-units of error per pair → the fitted `M` is noisy. Good enough to
  sanity-check, not to bake.

**This brief = hook the placement so `(world_in → render_out)` is read at the SOURCE, with
zero hover error**, for the actual grace icons. That gives clean pairs → an exact lstsq fit.

## Where the conversion happens (already mapped, `marker_to_mapspace_re_findings.md`)

- Build `FUN_140a82a80` and reconcile `FUN_140a832a0` (both recovered, **NON-VMP**) walk
  the `CSWorldMapPointMan` point RB-tree and, per visible point, call the Scaleform icon
  update `thunk_FUN_1457cd3df(point->icon, ctx)`.
- The world→render math is **inside that thunk** (a thunk into the VMProtect region); it
  writes a `Scaleform::Render::Matrix2x4<float>` onto the icon. The matrix's translation
  components are the render position. No plain function holds the per-page origin, and
  `CSWorldMapPointIns` exposes **no vmethod returning the render pos** (vt[4]
  `FUN_140a81140` is a MapId getter, not a coord).

So `world_in` is readable on the instance (param row), and `render_out` must be captured
either from the icon's written matrix or by reading the two floats the thunk stores.

## Plan — two capture points, pick whichever lands

### Capture `world_in` (easy, instance-side, NON-VMP)
At a breakpoint on the **call site** of `thunk_FUN_1457cd3df` inside `FUN_140a832a0`
(reconcile) or `FUN_140a82a80` (build), `point` (a `CSWorldMapPointIns*`) is live in a
register/stack slot. Its param row is at **`point + 0x80`** = the baked
`WORLD_MAP_POINT_PARAM_ST` (`areaNo` u8, `gridXNo` u8, `gridZNo` u8, `posX/posZ` f32 — same
layout the mod injects). Compute `world = (gridXNo·256+posX, gridZNo·256+posZ)` and the
`dstPage` (its `areaNo`, after the legacy-conv projection the game applies). Also read the
MapId via vt[4] if page disambiguation needs it.

### Capture `render_out` — try in this order
1. **Icon matrix write (preferred, exact).** Step into `thunk_FUN_1457cd3df` once (CE can
   bp into VMP). Find the instruction that writes two adjacent `float`s = the
   `Matrix2x4<float>` translation `(tx, ty)` onto the icon object (they should be in
   render space: hundreds … ~10496, scaling ~0.5× of world). Set a conditional/log
   breakpoint there → that's `render_out`. Pair it with the `world_in` read at the call
   site (same `point`, same frame).
2. **Icon display-object position (fallback).** After the thunk returns, read the
   Scaleform display object's local→page position off `point->icon` (the GFx node the
   matrix was written to). Same `(tx,ty)`.
3. **Cursor cross-check (sanity only).** The hover route's cursor `+0x104/+0x108` for a
   snapped grace should match `render_out` within hover error — use it to confirm you
   hooked the right floats, not as the primary pair.

## Deliverable

For each page **60, 61, 12, 40, 41, 42, 43**: capture **≥3** clean `(worldX, worldZ →
renderX, renderZ)` pairs (more = better fit; spread them across the page). Then solve:

```python
# pairs for ONE page: (wx, wz, rx, rz). render = M·world + T splits into two
# independent least-squares systems sharing the design matrix [wx wz 1].
import numpy as np
W  = np.array([[wx, wz, 1.0] for (wx, wz, _, _) in pairs])
rx = np.array([rX for (_, _, rX, _) in pairs])
rz = np.array([rZ for (_, _, _, rZ) in pairs])
abe, *_ = np.linalg.lstsq(W, rx, rcond=None)   # a, b, e
cdf, *_ = np.linalg.lstsq(W, rz, rcond=None)   # c, d, f
a, b, e = abe; c, d, f = cdf
print(f"M=[[{a:.5f},{b:.5f}],[{c:.5f},{d:.5f}]]  T=({e:.2f},{f:.2f})")
for wx, wz, rX, rZ in pairs:   # residuals should be sub-pixel
    print(rX-(a*wx+b*wz+e), rZ-(c*wx+d*wz+f))
```

**Confirm `M` (a,b,c,d) is the SAME across pages** and only `T` (e,f) differs. The strong
hypothesis (from the one fully-measured anchor) is a **90° axis-swap at scale 0.5**:
`a≈0, b≈±0.5, c≈±0.5, d≈0` — the hook resolves the exact signs (= the rotation/reflection
+ any per-page Z-flip the by-hand search missed). Deliver:
1. `M` (4 floats, once) + the per-page `T` table `{60:(e,f), 61:…, 12:…, 40:…, 41:…, 42:…, 43:…}`.
2. The `.CT` (address list + hook script + logger) used to capture, committed under
   `tools/cheat_engine/`.
3. Per-pair residuals proving the fit is sub-pixel.

The overlay then applies `render = M·world + T[page]` (already wired — `g_aff` in
`goblin_overlay.cpp`; replace the runtime solve with the baked constants).

## Sanity anchors (verify your captured pairs against these)

| grace | page | world (X, Z) | render (X, Z) |
|---|---|---|---|
| Dragonbarrow (area 21 → 61) | 61 | (12338.25, 11985) | (6018.75, 6187.28) |
| Academy Gate (area 16 → 60) | 60 | (9217.7, 13617) | (~1826, ?) |

(Native pages 60/61: `world = grid·256 + pos`, no conv. Underground 12 / DLC 40–43: same
procedure, their own `T`.)

## Handles / AOBs (from `marker_to_mapspace_re_findings.md` §5)

- `CSWorldMapPointMan` instance static **`0x143d6e9b0`**.
- build **`FUN_140a82a80`** AOB:
  `40 55 53 56 57 41 54 41 56 41 57 48 8B EC 48 83 EC 60 48 C7 45 D0 FE FF FF FF 4C 8B F9 8B 42 34`;
  reconcile **`FUN_140a832a0`**; icon update **`thunk_FUN_1457cd3df`** (VMP target).
- `CSWorldMapPointIns` vtable **`0x142b487a8`**; **param row at `inst+0x80`**; MapId getter
  vt[4] `FUN_140a81140`.
- `WorldMapArea`: render fullRect `+0x350..+0x35c` = `[0,0,10496,10496]`; pan `+0x378`,
  zoom `+0x380`. Cursor (hover cross-check) = `menu+0x2DB0`, vtable scan target
  `eldenring.exe+0x2b29a90`, render coord `+0x104/+0x108`.
- overworld-vs-underground sublayer flag `DAT_143d6cfc3` (`eldenring.exe+0x3D6CFC3`). The
  per-page open-area id is unresolved (cursor view = unified root areaNo 0) — label each
  capture's page manually.
- Offsets are version-specific; resolve the point manager/instances from the static +
  vtable (patch-robust), not hardcoded addresses.

## Notes

- Companion to `marker_mapspace_CT_recipe.md` (cursor-hover, approximate) — this is the
  **exact** variant via the placement hook. Reuse that table's cursor auto-resolve for the
  `render_out` cross-check.
- If `M` does NOT come out shared across pages (residuals stay large), the transform is
  richer than one global affine (per-region origins within a page, or a non-affine
  Scaleform step) — report that with the per-page `M`s, it changes the bake strategy.
