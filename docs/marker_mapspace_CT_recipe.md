# Recipe — recover the world→render affine `M·world + T` with the Cheat Engine table

Companion to `tools/cheat_engine/MapForGoblins_mapspace.CT`. Answers
`docs/windows_marker_mapspace_CT_re_prompt.md`. By-hand calibration failed because the
grace constellation comes out **rotated** — so the world→render step is a full affine
`render = M·world + T` (M = scale·rotation, shared; T per page), not a diagonal scale.
The placement is Scaleform/VMP-buried, so we observe it live with the CT and solve M+T
from logged `(world → render)` pairs.

```
render = M·world + T          M = [[a,b],[c,d]]  (≈ scale·rotation),  T = (e,f) per page
renderX = a·worldX + b·worldZ + e
renderZ = c·worldX + d·worldZ + f
world  = (gridXNo·256 + posX, gridZNo·256 + posZ)   // CELLSIZE 256
render = cursor +0x104 / +0x108 (live)
```

## What the CT gives you

Address list (auto-resolving; cheap per-second re-validate, heavy scan only on
NUMPAD 1 / throttled — low CPU):
- **cursor render X/Z** (`+0x104/+0x108`) and reticle `+0xFC/+0x100/+0x10C`,
- **WorldMapArea** (`[cursor+0xF0]`): pan `+0x378/+0x37C`, zoom `+0x380`, fullRect
  `+0x350..+0x35C`,
- **sublayer flag** `DAT_143d6cfc3` (0 = overworld, ≠0 = underground),
- `view+0x370/+0x374` (diag: the cursor's view is a *unified root*, areaNo `0` — **not**
  the open-page id; label each grace's page yourself, see caveat below),
- **NUMPAD 0** snapshot logger → appends both render pairs + pan/zoom to
  `MapForGoblins_mapspace_ct.log` (next to cheatengine.exe); **NUMPAD 1** = force rescan.

The cursor is found like the in-DLL probe: scan writable memory for the
`CS::WorldMapCursorControl` vtable value (`eldenring.exe + 0x2b29a90`) and keep the heap
instance whose `+0x104/+0x108` are plausible (≥1, ≤2e4) with a heap view + sane zoom —
this rejects the in-image static cursor *template* (denormal coords) that the first cut
locked onto.

### Confirmed live (2026-06-20, app 2.6.2.0, overworld open)

The hardened table locks the real cursor; sample readout:
```
fullRect = [0, 0, 10496, 10496]      zoom = 2.25      view = heap (0x254…)
render (+0x104/+0x108) = (3287.6, 8735.9)   reticle (+0xFC/+0x100) = (3904.3, 8464.6)
pan (+0x378/+0x37C) = (8091.8, 18657.5)     sublayer flag = 0 (overworld)
```
`fullRect 10496` + sane `zoom`/`pan` confirm it is the right `WorldMapArea`. **Page-id
caveat:** `view+0x370` reads `0` — this view is the unified cursor controller, not a
per-page object, so it does not carry the open page (60/61/12/40-43). Overworld 60 and 61
share this one render canvas; the 60↔61 difference is exactly the per-page `T`. Use the
`DAT_143d6cfc3` flag for overworld-vs-underground and **label the page of each grace
manually** when collecting pairs.

## Steps

1. Open **Cheat Engine**, attach to **eldenring.exe**. `File → Open` →
   `tools/cheat_engine/MapForGoblins_mapspace.CT`. The table Lua auto-runs (console prints
   "map-space probe armed"). If the console is hidden: `Table → Show Cheat Table Lua
   Script → Execute`.
2. In-game, **open the world map** and **move the cursor**, then press **NUMPAD 1** to
   lock onto the live cursor (the console prints `[MFG] locked cursor=… view=… (heap)
   render=(…)`). The heavy memory scan only runs on NUMPAD 1 / ~every 6 s while unlocked,
   so it does **not** peg the CPU; once locked the rows update cheaply each second.
3. **Hover a KNOWN grace** so the reticle sits on it (snap if needed). Press **NUMPAD 0**.
   One line is logged: both render pairs + pan/zoom. Note which grace it was (its page).
   When snapped on the grace, see which pair (`render104` or `reticleFC`) sits on it — use
   that one consistently (Dragonbarrow was measured at `+0x104/+0x108`).
4. Repeat for **≥3 graces on the SAME page** (more = better fit), then do another page.
   (After reopening the map, press NUMPAD 1 again — the cursor object is rebuilt.)
   Use the ready anchors below (native area 60/61 need no LegacyConv — their world is
   just `grid·256 + pos`). Note each snapshot's grace so you can pair render↔world.
5. **Solve** `M` + per-page `T` from the pairs (script below). Confirm `M` (a,b,c,d) is
   the **same across pages** and only `T` (e,f) differs per page. Then bake `M` + the
   per-page `T` table.

### Sanity check (you have the RIGHT cursor)

After NUMPAD 1 the resolved handles should look like the "Confirmed live" block above:
- `cursor render X/Z` = plausible map values (hundreds … ~10496), **changing** as you move
  the cursor — not denormals like `4.59E-41`.
- `fullRect` = `[0, 0, 10496, 10496]` and `zoom` a small positive (~2.25).
- the `WorldMapArea` rows (`MFG_VIEW`) resolve to a **heap** address (`0x1…/0x2…`), **not**
  an in-image address (`0x7FF6…`). An in-image view = the static cursor *template*; the
  table rejects it (it requires a heap view + plausible coords + sane zoom). If you still
  see junk, move the cursor and press NUMPAD 1.

## Ready anchors (native pages — world = grid·256 + pos, no conversion)

Pick graces you can find in-game; hover each, NUMPAD 0, and pair with this world:

**Page 60 (Lands Between overworld):**
| region | grid | pos | world (X, Z) |
|---|---|---|---|
| Limgrave | (43,37) | (75, 61) | (11083, 9533) |
| Weeping Peninsula | (42,33) | (-114, 31) | (10638, 8479) |
| Flame Peak | (50,53) | (-118, 120) | (12682, 13688) |
| Mountaintops | (53,56) | (-38, 114) | (13530, 14450) |
| Consecrated Snowfield | (50,55) | (-69, -86) | (12731, 13994) |

**Page 61 (Caelid / east grid):**
| grid | pos | world (X, Z) |
|---|---|---|
| (45,41) | (-8, 55) | (11512, 10551) |
| (46,40) | (61, 34) | (11837, 10274) |
| (47,41) | (-71, 68) | (11961, 10564) |

(Underground = page 12, DLC = pages 40–43: same procedure, their own `T`.)

## Solve M + T from the log

`render = M·world + T` splits into two independent least-squares systems
`[wx wz 1]·[a b e]ᵀ = rx` and `[wx wz 1]·[c d f]ᵀ = rz`. Per page, 3+ pairs solve it:

```python
# pairs: list of (worldX, worldZ, renderX, renderZ) for ONE page (>=3 rows)
import numpy as np
W = np.array([[wx, wz, 1.0] for (wx, wz, _, _) in pairs])
rx = np.array([rX for (_, _, rX, _) in pairs])
rz = np.array([rZ for (_, _, _, rZ) in pairs])
abe, *_ = np.linalg.lstsq(W, rx, rcond=None)   # a, b, e
cdf, *_ = np.linalg.lstsq(W, rz, rcond=None)   # c, d, f
a, b, e = abe; c, d, f = cdf
print(f"M = [[{a:.5f},{b:.5f}],[{c:.5f},{d:.5f}]]   T = ({e:.2f},{f:.2f})")
# sanity: residuals should be ~sub-pixel
for wx, wz, rX, rZ in pairs:
    print(rX - (a*wx + b*wz + e), rZ - (c*wx + d*wz + f))
```

**The pivot is `T`, and it's per-page — don't ship `M` alone.** `M·world` with `T=0`
rotates about the world origin `(0,0)`; world coords are thousands, so every marker flies
**off-screen** (the exact "rotated but off-screen" bug). The per-page translation `T=(e,f)`
re-centers each page into the render rect — it's the missing pivot, and it falls straight
out of the same lstsq (the `1` column). **Deliver `M` (shared) + `T[page]` for every page
60/61/12/40/41/42/43**, and sanity-check that `M·world + T[page]` lands inside
`[0,0,10496,10496]` for that page's anchors. A page with no `T` reproduces the off-screen
bug. (The `render_finder` CT v6 emits exactly this: one shared `M` + a `T[page]` line per
page with an on-rect YES/NO.)

`M` should come out ≈ `scale·rotation`. The strong hypothesis (from the single fully
measured anchor, Dragonbarrow page 61 → tiny T) is a **90° axis-swap at scale 0.5**:
`a≈0, b≈±0.5, c≈±0.5, d≈0` (i.e. `renderX≈0.5·worldZ + e`, `renderZ≈0.5·worldX + f`).
Confirm the signs live — that fixes the rotation/reflection the by-hand search missed.

## Bake it

Once `M` is confirmed page-independent, store `M` (4 floats) once and a per-page `T`
table `{60:(e,f), 61:…, 12:…, 40:…, 41:…, 42:…, 43:…}`, and apply
`render = M·world + T[page]` in the overlay's marker projection (replacing the failed
diagonal-scale calibration). The screen step is already solved
(`screen = (render − viewCentre)·zoom + canvas/2`, `world_map_projection_re_findings.md`).
`world` for legacy-dungeon rows still needs the LegacyConv **base-point translation** fix
(`marker_to_mapspace_re_findings.md` §2) before `M·world+T` is applied.

## Optional — hook the placement (advanced, VMP)

To capture `(world_in → render_out)` automatically per built marker, breakpoint the icon
update called from reconcile `FUN_140a832a0` / build `FUN_140a82a80`
(`thunk_FUN_1457cd3df`). CE can bp into the VMProtect region; step to where it writes the
`Scaleform::Render::Matrix2x4<float>` and read the 6 matrix floats directly. Not required
— the hover-snapshot method above recovers the same `M`/`T` without touching VMP code.

## Handles / AOBs

- cursor vtable (scan target): `eldenring.exe + 0x2b29a90`. cursor = `menu + 0x2DB0`.
- WorldMapArea = `[cursor+0xF0]`; pan `+0x378`, zoom `+0x380`, fullRect `+0x350` (confirmed
  `[0,0,10496,10496]`); `+0x370` = unified-view areaNo (`0`, not the page).
- sublayer flag `eldenring.exe + 0x3D6CFC3` (overworld vs underground). The real open-page
  id (per-area `WorldMapArea` / `CSWorldMapMenu`) is unresolved — not needed for M/T.
- Offsets version-specific; the cursor is resolved by vtable scan (patch-robust).
