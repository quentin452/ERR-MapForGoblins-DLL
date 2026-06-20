# Windows RE — intercept the game's marker placement getter, deliver M + T[page]

**App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. Cheat-Engine + running game.**
**Sequencing:** run this AFTER the open-map Region getter task (same `CS::WorldMapDialog`
object, same cursor-chain you already resolved — see below, they compose). Output of THIS
task = the per-page render offset, which is exactly the value that drifts between sessions
in our overlay today.

## The premise (why this is capturable, not magic)

Our overlay places goblin markers via `render = M·world + T`. Today `M`/`T` are an eyeball
SEED → the marker constellation is rotated/offset and **`T` (the pan offset) varies between
sessions** because we re-derive it from a recomputed centroid pivot. That is a hack.

The game itself places its OWN native markers in exactly the right spot every time. So the
game HAS this transform — there is a **getter**: it reads a `WorldMapPointParam` row and
writes the icon's render position. It is **not magic and not a per-session value** — it is a
fixed function with a per-page constant offset. We just need to intercept its input/output
and read off `M` (shared) + `T[page]` (the offset). One-time measure → bake as constants →
zero session drift, zero user calibration.

The getter's output lives in a Scaleform matrix written inside a VMP thunk, so it is NOT
statically liftable — it must be read **live at a breakpoint**. That is the whole task.

## Already pinned (re_v56, `marker_affine_hook_re_findings.md`) — do NOT re-derive

- **Getter call (world_in side):** `CALL thunk_FUN_1457cd3df` inside reconcile
  `FUN_140a832a0` @ RVA **`0xa839a6`**.
  AOB `66 48 0F 7E C9 48 85 C9 74 08 49 8B D5 E8` (call at found+13).
  At the call: **RCX = `CSWorldMapPointIns* point`**, RDX = ctx.
  Param row at **`point+0x80`**: `areaNo` u8, `gridXNo` u8, `gridZNo` u8, `posX`/`posZ` f32
  (mod's `WORLD_MAP_POINT_PARAM_ST` layout). →
  `world = (gridXNo·256 + posX, gridZNo·256 + posZ)`.
  **`areaNo` here is POST legacy-conv = the dst page (60/61/12/40-43)** → use directly as the
  page label; no MapId/region lookup needed for labeling.
- **Getter output (render_out side):** the icon's `Scaleform::Render::Matrix2x4<float>`
  translation `(tx, ty)`, written inside the VMP thunk (entry **`0xa805e0`**). Render space
  `[0,0,10496,10496]`, ≈0.5× world.
- **Structure (re_v59/v61):** only built/visible markers get a render pos; the C++
  per-marker scan is a dead end (only ~4 instances). The matrix is the only render-pos
  source, reachable only live.

## Task — capture, then solve

1. **bp at `0xa839a6`** (world_in). Per hit: read `point`, then `point+0x80` →
   `(page=areaNo, wx=gridXNo·256+posX, wz=gridZNo·256+posZ)`. Dedup by `point`/world (the bp
   fires every frame for every on-page marker).
2. **Capture render_out**, pick the cheaper of:
   - **(a) in-VMP bp at `0xa805e0`** — find the two adjacent `float` stores writing
     render-range values (hundreds … ~10496) into the `Matrix2x4` translation; log `(tx, ty)`
     paired with the step-1 `point` (same frame). This grabs ALL on-page pairs at once. OR
   - **(b) post-call read** — resolve `point → icon` and the icon→`Matrix2x4` translation
     offset so `render_out` is a plain memory read after the call. **Report both offsets** —
     they unlock fully-automated in-DLL self-calibration later (no CE needed).
3. **Collect ≥3 spread pairs per page** for **60, 61, 12, 40, 41, 42, 43** (open map on each,
   a few seconds each). More = better fit.
4. **Solve per page** (`render = M·world + T`):
   ```python
   import numpy as np
   W  = np.array([[wx, wz, 1.0] for (wx, wz, _, _) in pairs])   # one page
   rx = np.array([rX for (_, _, rX, _) in pairs])
   rz = np.array([rZ for (_, _, _, rZ) in pairs])
   a, b, e = np.linalg.lstsq(W, rx, rcond=None)[0]
   c, d, f = np.linalg.lstsq(W, rz, rcond=None)[0]
   # M = [[a,b],[c,d]]  shared;  T[page] = (e, f)  per page
   ```
5. **Verify `M=[[a,b],[c,d]]` is the SAME across all pages** (only `T=(e,f)` differs). Report
   per-page mean residual (must be sub-pixel). If `M` is NOT shared, report each page's `M` —
   that means the transform is richer than one global affine and changes the bake strategy.

## Deliverable — the numbers

1. **`M`** — 4 floats `a, b, c, d` (one shared matrix).
2. **`T[page]`** — the `(e, f)` offset for each of 60, 61, 12, 40, 41, 42, 43. **This is the
   per-page offset that fixes the session drift.**
3. Per-page mean residual (sub-pixel proof).
4. If step 2(b): the `point → icon` offset + icon → `Matrix2x4` translation offset.
5. The `.CT` used, committed under `tools/cheat_engine/`.

We bake `M` + `T[page]` as constants in `goblin_overlay.cpp` (replace the `g_aff` runtime
solve + centroid pivot) → markers land correctly, no pan, no per-session variance.

## Also report (one yes/no) — do undiscovered markers have a readable render pos?

The overlay's purpose is showing UNDISCOVERED markers. While map open on a page:
1. Does `CSWorldMapPointMan + 0x398` (built-icon map) hold entries for graces the player has
   NOT discovered? Count built nodes vs visible icons.
2. Show-predicate is vt[1] `FUN_140a81450` — evaluated BEFORE insert into `+0x398` (hidden →
   never built → no pos) or AFTER (all built with a pos, just not drawn)?
3. If built-but-hidden points exist, do they carry a valid `Matrix2x4` translation in render
   range?

Answer: **"undiscovered markers have a readable render position: YES/NO"** + evidence.
(YES → we could skip the affine and read positions live. NO → the M/T bake is mandatory.
Either way the captured pairs still pin M/T.)

## Page → region (use `areaNo` from `point+0x80` to label pairs)

`areaNo` read at the bp is POST legacy-conv = the dst page. Map it:

| areaNo | page | region |
|---|---|---|
| **60** | overworld, base | Limgrave / Liurnia / Caelid / Altus / Mountaintops (tabIds 61000–65000) |
| **61** | overworld, DLC | Realm of Shadow / Shadow Keep (tabIds 6800–6940, 21000) |
| **12** | underground, base | Nokron, Deeproot, Ainsel, Siofra, Lake of Rot, Mohgwyn (tabIds 12000–12002) |
| **40–43** | underground, DLC | Realm of Shadow (tabIds 6800–6940) |

Capture pairs on the matching map: base overworld for 60, DLC overworld for 61, base
underground for 12, DLC underground for 40–43.

## Sanity anchors (your pairs should match)

| grace | page | world (X, Z) | render (X, Z) |
|---|---|---|---|
| Shadow Keep (area 21 → conv) | 61 | (12338.25, 11985) | (6018.75, 6187.28) |
| Academy Gate | 60 | (9217.7, 13617) | (~1826, ?) |

NB: the page-61 anchor is a DLC Shadow Keep grace (source area 21, tabId 21000), NOT
Dragonbarrow (Dragonbarrow is base-game Caelid → page 60). Earlier notes mislabeled it.

Anchor check: `(12338.25 − originX[61])·0.5 = 6018.75` → `originX[61] ≈ 300`; i.e. `T` and
the per-page origin are the same constant in different form (`T = −origin·0.5`).
