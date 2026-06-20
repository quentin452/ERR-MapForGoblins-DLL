# Windows RE — the missing piece is the per-page T (the rotation pivot), not just M

Short clarification on top of `windows_marker_affine_capture_run_re_prompt.md` /
`marker_affine_hook_re_findings.md`. You said the calibration is fully specced and you're
now reading `render_out` — correct, that's the last input. This nails down WHY, so the
deliverable is unambiguous.

## The symptom

In base mode the overlay applies `render = M·world + T` with `M` = the seed guess and
**`T = 0`**. Result: markers come out **rotated but flung OFF-SCREEN**. The user's exact
observation: "just rotate the map 90° — that's what's needed, but at 90° it goes off-screen."

## Why — the pivot IS `T`

A rotation alone rotates about the **world origin (0,0)**. World coords are large
(`grid·256+pos` ~ thousands), so `M·world` with no translation sends every point thousands
of units away → off-screen. To keep the constellation in the render rect `[0,0,10496,10496]`
you need the per-page translation:

```
render = M·world + T          (M = scale·rotation, SHARED;  T = (e,f) PER PAGE)
```

`T` is exactly the "rotation pivot / re-centering" the user is missing. Equivalently, if you
prefer a pivot form `render = M·(world − P)`, then `T = −M·P` and `P = −M⁻¹·T` — same thing.
**The overlay needs `T`, per page.**

`T` is NOT a separate unknown to hunt for — it **falls straight out of the per-page
least-squares fit** the moment you have `render_out`. With ≥3 `(world_in → render_out)`
pairs on one page, solving `render = M·world + T` yields `a,b,c,d` (→ M) **and** `e,f`
(→ T) together. The `1` column in the design matrix `[wx wz 1]` IS the translation/pivot
term:

```python
W  = np.array([[wx, wz, 1.0] for (wx,wz,_,_) in pairs])   # the '1' solves T
a,b,e = np.linalg.lstsq(W, [rX for ...], rcond=None)[0]
c,d,f = np.linalg.lstsq(W, [rZ for ...], rcond=None)[0]
# M = [[a,b],[c,d]]   (shared)        T = (e,f)  (THIS page)
```

## What this changes about the deliverable (the one thing to get right)

Deliver **BOTH**, do not stop at M:

1. `M` — 4 floats `a,b,c,d` (one shared matrix across pages).
2. **`T[page]` — `(e,f)` for EVERY page: 60, 61, 12, 40, 41, 42, 43.** This is the missing
   pivot. M alone (with T=0) reproduces exactly the current off-screen bug. A page with no
   `T` will fly off-screen.

Sanity (proves T is right, not just M): for each page, `M·world_anchor + T[page]` must land
**inside** `[0,0,10496,10496]`, matching the measured render:
- Dragonbarrow (page 61): world `(12338.25, 11985)` → render `(6018.75, 6187.28)`
- Academy Gate (page 60): world `(9217.7, 13617)` → render `(~1826, ?)`

If a page's points still leave the rect after applying its own `T`, the fit for that page is
wrong (too few pairs, mismatched world_in/render_out pairing, or wrong page label) — report
it rather than shipping a partial table.

## Recap

- `M` (rotation+scale) you may already have — that's the orientation.
- `T[page]` (the pivot/offset) is the piece whose absence throws markers off-screen, and it
  comes free from the same lstsq once `render_out` is captured.
- Deliver `M` + `T` for all 7 pages, with the on-rect sanity check per page.
