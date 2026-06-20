# Windows RE follow-up — EXECUTE the capture, deliver the M + T numbers

**App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. Cheat-Engine + running game.**
This is the execution step after `marker_affine_hook_re_findings.md` (re_v56). The hook
site is already pinned; **all that's left is to run the capture and return the solved
constants** so they can be baked. No further static RE needed unless the capture surprises.

## Why this matters (current state)

The overlay places goblin markers via `render = M·world + T` but `M`/`T` are still a SEED
GUESS (90° axis-swap @0.5) → in base mode the constellation is visibly **rotated/inverted**
(e.g. graces that should sit vertical/low appear horizontal/upper-right). Guessing the
orientation by eye has repeatedly failed — it's a genuine rotation, not 0/90/mirror. We
need the measured matrix. The game itself places its native markers correctly, so capturing
its own `(world → render)` per marker gives the exact transform.

## What re_v56 already pinned (don't re-derive)

- **`world_in`**: hook `CALL thunk_FUN_1457cd3df` in reconcile `FUN_140a832a0` @ RVA
  `0xa839a6` (AOB `66 48 0F 7E C9 48 85 C9 74 08 49 8B D5 E8`, call at found+13). At the
  call **RCX = `CSWorldMapPointIns*` (`point`)**, RDX = ctx. Param row at **`point+0x80`**
  (`areaNo` u8, `gridXNo` u8, `gridZNo` u8, `posX/posZ` f32, mod's `WORLD_MAP_POINT_PARAM_ST`
  layout) → `world = (gridXNo·256+posX, gridZNo·256+posZ)`. `areaNo` here is **post the
  game's legacy-conv** = the dst page (60/61/12/40-43) → use it directly as the page label.
- **`render_out`**: on the icon's `Scaleform::Render::Matrix2x4<float>`, written inside the
  VMP thunk (entry `0xa805e0`). The translation components `(tx, ty)` = render position
  (render space `[0,0,10496,10496]`, ≈0.5× world).

## The task — capture, then solve

1. **Set the world_in breakpoint** at `0xa839a6`; log `point`, then read `point+0x80` →
   `(areaNo, world)`. (areaNo gives the page; no MapId needed.)
2. **Capture render_out.** Either:
   - bp into the VMP thunk (`0xa805e0`) and find the two adjacent `float` stores that write
     render-space values (hundreds … ~10496) to the icon's `Matrix2x4` translation — log
     `(tx, ty)` paired with the `point` from step 1 (same frame); **or**
   - if the in-VMP bp is awkward, locate `point → icon` and the icon's matrix offset so
     `render_out` is a plain post-call memory read (and report those two offsets — that
     also unlocks a fully-automated in-DLL self-calibration later).
3. **Collect ≥3 pairs per page** for **60, 61, 12, 40, 41, 42, 43** (spread across each page;
   more = better fit). The reconcile bp fires for every on-page marker each frame, so a few
   seconds with the map open on each page yields plenty — dedup by `point`/world.
4. **Solve** per page (`render = M·world + T`):
   ```python
   import numpy as np
   W  = np.array([[wx, wz, 1.0] for (wx, wz, _, _) in pairs])  # one page
   rx = np.array([rX for (_, _, rX, _) in pairs])
   rz = np.array([rZ for (_, _, _, rZ) in pairs])
   a,b,e = np.linalg.lstsq(W, rx, rcond=None)[0]
   c,d,f = np.linalg.lstsq(W, rz, rcond=None)[0]
   ```
5. **Confirm `M=[[a,b],[c,d]]` is the SAME across pages** (only `T=(e,f)` differs). Report
   residuals (should be sub-pixel).

## Deliverable (the numbers, not the method)

1. `M` — 4 floats `a, b, c, d` (one shared matrix).
2. `T[page]` — the `(e, f)` translation for each of 60, 61, 12, 40, 41, 42, 43.
3. Per-page mean residual (proof the fit is sub-pixel).
4. If step 2 used the post-call read: the `point → icon` offset and the icon → `Matrix2x4`
   translation offset.
5. The `.CT` used, committed under `tools/cheat_engine/`.

We bake `M` + `T[page]` as constants in `goblin_overlay.cpp` (replace the `g_aff` runtime
solve) → base mode renders correctly with no user calibration.

## ALSO REPORT — does the game place UNDISCOVERED markers? (architecture-critical)

The overlay's whole point is showing **undiscovered** graces/markers. If the game only
builds an icon (and thus computes a render position) for **visible/discovered** markers,
then undiscovered ones have no render pos to read → we MUST compute them via the baked
`M·world+T` (the math is mandatory). But if the game builds icons for **all** markers and
merely hides the undiscovered ones (visibility flag off, position still computed), then we
could read every render pos directly and the math becomes optional. **Determine which.**

Concretely, while the map is open on a page:
1. Does the built-icon map at `CSWorldMapPointMan + 0x398` contain entries for graces the
   player has **NOT** discovered (i.e. icons not drawn on screen)? Count built nodes vs
   visible icons.
2. The instance's show-predicate is vt[1] `FUN_140a81450` — is it evaluated **before** the
   point is inserted into the `+0x398` map (→ hidden points never built, no pos), or
   **after** (→ all points built with a pos, just not drawn)?
3. If built-but-hidden points exist, do they carry a valid render position (the
   `Matrix2x4` translation in render range), or is the matrix only written for visible ones?

Report a yes/no: **"undiscovered markers have a readable render position: YES/NO"** — plus
the evidence. (YES would let us skip the affine entirely and read positions live; NO
confirms the M/T bake is required. Either way the captured pairs above still pin M/T.)

## Sanity anchors (your captured pairs should match)

| grace | page | world (X, Z) | render (X, Z) |
|---|---|---|---|
| Dragonbarrow | 61 | (12338.25, 11985) | (6018.75, 6187.28) |
| Academy Gate | 60 | (9217.7, 13617) | (~1826, ?) |

If `M` does NOT come out shared across pages, report the per-page `M`s — that means the
transform is richer than one global affine and changes the bake strategy.
