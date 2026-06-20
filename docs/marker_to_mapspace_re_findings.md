# RE findings — WorldMapPointParam row → RENDER marker space

Answers `docs/windows_marker_to_mapspace_re_prompt.md`. Static Ghidra RE
(`D:\ghidra_proj2\ER`, scripts `re_v51..v55`) + the two live reticle measurements in
the prompt. App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. Builds on
`docs/world_map_projection_re_findings.md` (the screen projection — DONE).

> **CORRECTION (2026-06-20):** the `render = (world − origin)·0.5` model below is
> **incomplete** — by-hand calibration showed the constellation comes out **rotated**, so
> the world→render step is a full affine `render = M·world + T` with `M` a scale·**rotation**
> (an ~90° axis-swap fits the one good anchor with tiny `T`), not a diagonal scale. The
> per-page origin/scale here still stands as the *structure*; `M`/`T` are recovered live via
> the Cheat Engine table — see `docs/marker_mapspace_CT_recipe.md` and
> `tools/cheat_engine/MapForGoblins_mapspace.CT` (cursor render `+0x104/+0x108`, WorldMapArea
> pan/zoom/fullRect; confirmed live: `fullRect [0,0,10496,10496]`, `zoom 2.25`).

---

## 0. TL;DR

The render marker space (the one the cursor lives in, `WorldMapArea` fullRect
`[0,0,10496,10496]`) relates to the mod-internal world `(gridXZ·256 + pos)` by a
**per-page affine**:

```
render = (gridXZ·256 + pos − origin[dstPage]) · SCALE
SCALE  = 0.5      (render-units-per-tile = 128 = 256·0.5;  fullRect 10496 = 82·128)
origin[dstPage]   per-page offset (marker/world units), one per dst page
```

where `dstPage` is the overworld/sub page the row projects onto via
**WorldMapLegacyConvParam** (legacy dungeons), and is the row's own `areaNo` for native
pages. There is **no single global transform** — pages 60, 61, 12 (underground), 40–43
(DLC) each have their own `origin` (and possibly Z-flip sign), which is why a pure scale
piled everything into one region.

The exact `origin[page]` constants are **not cleanly extractable statically** — the row→
render placement happens in the Scaleform icon layout (`thunk_FUN_1457cd3df`, a thunk into
the VMProtect'd region), not in a plain C++ function with literal constants. So `origin`
per page is delivered as a **runtime-calibrated table** (1–2 known graces per page; the
overlay's existing calib UI already does per-region tuning — this just formalises it to 6
pages). SCALE = 0.5 is structurally confirmed (below); origins are small per-page offsets.

---

## 1. What the decompiler confirmed

- **Render space is per-page, `WorldMapArea` fullRect `[0,0,10496,10496]`** (ctor
  `FUN_1409cb9c0` sets `+0x350..+0x35c = [0,0,10496,10496]`, `0x46240000 = 10496.0`).
  `10496 = 82 × 128` → the overworld is 82 render-tiles of 128 units; world tile = 256
  units → **render scale 0.5**.
- **`CSWorldMapPointIns` has 7 vmethods** (vtable `0x142b487a8`): vt[0]=`FUN_140a812d0`,
  vt[1]=show-pred `FUN_140a81450`, vt[2]=`FUN_140a81180`, vt[3]=area `FUN_140a81440`,
  vt[4]=`FUN_140a81140` (**MapId getter**, not a coord — it bit-packs a MapId via
  `FUN_1401b76e0`), vt[5]=`FUN_140a811d0`, vt[6]=thunk. **No vmethod returns the render
  position** — it is set on the Scaleform icon, not stored as a readable vec on the
  instance (the ctor `FUN_140a811e0` inits the pos fields `+0xa0../+0xe0..` to defaults).
- **Build/reconcile carry no coordinate math.** `FUN_140a82a80` (build, recovered to 421B
  after clearing the noreturn misflag on `FUN_141eb9ed0`) and `FUN_140a832a0` (reconcile)
  only walk the point RB-tree and toggle visibility / call the Scaleform icon update
  `thunk_FUN_1457cd3df(point->icon, ctx)`. The coordinate conversion is inside that icon
  update → Scaleform/VMP, not a liftable constant-bearing function.
- **Per-page clamp rect table** exists: `FUN_140886770(view+0x360, view+0x370)` →
  `FUN_140256350() = [DAT_143d5df38+0x68]` + `0x18 + idx·0x20` (idx 0/1/2 from selector
  0/1/10). These are render-space clamp rects per layer, not the world→render origin.

## 2. The conversion structure (deliverable #1)

For a baked row `(areaNo, gridXNo, gridZNo, posX, posZ)`:

**Step A — project to a dst page (legacy dungeons only).** Apply
`WorldMapLegacyConvParam` as a **base-point translation**, not a dst-substitution:
```
// find the conv base row for (srcArea, srcGridX, srcGridZ)  (isBasePoint)
srcBaseWorld = srcBaseGrid·256 + srcBasePos
dstBaseWorld = dstBaseGrid·256 + dstBasePos
dstWorld     = dstBaseWorld + ( (gridXZ·256 + pos) − srcBaseWorld )   // preserve offset
dstPage      = dstAreaNo    // 60 or 61 (NOT always 60 — e.g. area 21 Dragonbarrow → 61)
```
The mod's current `project_dungeon_row_to_overworld` substitutes `dst_gx·256+dst_pos`
directly and keys only on `(src_area, src_gx)` — that drops the marker's offset-from-base
and the `srcGridZ` selection, which is the **area-16 "wrong region" bug** in the prompt.
The baked `LEGACY_CONV` already has src+dst pos, so the offset term is available; the fix
is to use the translation above. **Native pages** (area 60/61, and 12/40–43) skip step A;
`dstPage = areaNo`, `dstWorld = gridXZ·256 + pos`.

**Step B — world → render (per page):**
```
renderX = (dstWorldX − originX[dstPage]) · 0.5
renderZ = (dstWorldZ − originZ[dstPage]) · 0.5      // verify Z-flip sign per page (see §3)
```

**Step C — render → screen** (already solved, `world_map_projection_re_findings.md`):
`screen = (render − viewCentre)·zoom + canvas/2`, then ×(realW/1920, realH/1080).

## 3. Evidence (deliverable #3) — the two measured graces

| grace | dstPage | dstGrid | dstWorldX = grid·256+pos | reticle renderX | ⇒ originX (renderX=(world−o)·0.5) |
|---|---|---|---|---|---|
| Dragonbarrow (area 21) | **61** | gx 48 | 48·256+50.25 = 12338.25 | 6018.75 | **≈ 300** |
| Academy Gate (area 16) | **60** | gx 36 | 36·256+1.74 = 9217.7 | ~1826 | **≈ 5632 (≈22 tiles)** |

Both fit `render = (world − origin)·0.5` with SCALE = 0.5 shared and a **distinct per-page
origin** (61 ≈ 300, 60 ≈ 5632). This is the deliverable's core: scale is global, origin is
per dst page. (Single measurement per page can't separate a per-page scale wobble from the
origin; 0.5 is fixed by `10496 = 82·128` and both ratios sit at ~0.49/0.5.)

## 4. Calibration recipe to pin origin[page] (finish at runtime)

The overlay already reads the live reticle render coord and has a calib UI. For each dst
page (60, 61, 12, 40, 41, 42, 43):
1. In-game, hover the reticle exactly on a grace whose baked `(area,grid,pos)` you know;
   read its render coord from the probe (`get_live_view`, cursor `+0x104/+0x108`).
2. With SCALE = 0.5 fixed: `originX = dstWorldX − renderX/0.5`, `originZ` likewise
   (flip the Z sign if renderZ runs opposite world Z — test with a second grace N/S of the
   first). One grace per axis pins the origin; a second confirms scale + Z sign.
3. Bake the 6–7 `origin[page]` pairs (+ Z sign) into a small table next to `LEGACY_CONV`.

This converts the prototype's ad-hoc per-region `scale/bias` tuning into a principled
per-page origin table (scale 0.5 constant), and the Step-A translation fix removes the
transitive-projection errors.

## 5. AOBs / handles (deliverable #2)

- `CSWorldMapPointMan` instance static `0x143d6e9b0`; build `FUN_140a82a80`
  (AOB `40 55 53 56 57 41 54 41 56 41 57 48 8B EC 48 83 EC 60 48 C7 45 D0 FE FF FF FF
  4C 8B F9 8B 42 34`), reconcile `FUN_140a832a0`.
- `CSWorldMapPointIns` vtable `0x142b487a8`; param row at `inst+0x80`; MapId getter
  vt[4] `FUN_140a81140`.
- `WorldMapArea` render rect `+0x350..+0x35c` = `[0,0,10496,10496]`; pan `+0x378`,
  zoom `+0x380` (projection doc). Per-layer clamp-rect table `[DAT_143d5df38+0x68]`.
- `WorldMapLegacyConvParam` / `WorldMapPlaceNameParam` param strings at
  `0x143cab9fc` / `0x143cabbf4` (param-registry names, no direct LEA xref — looked up by
  name at runtime; the mod already bakes both tables from the regulation).

## 6. Caveats / open

- **origin[page] constants are calibrated, not lifted** — the Scaleform placement
  (`thunk_FUN_1457cd3df`) is in the VMP region; no plain function holds the per-page origin.
  If a closed-form is required, the next static step is to resolve that thunk's real target
  at runtime (bp it, read the matrix it writes) — but calibration (1–2 graces/page) is
  faster and matches the workflow.
- **Z-flip**: the player→map-UI transform (`FUN_140876140`, `re_findings_playerpos.md`)
  has a Z-flip (`0x80000000`); expect render Z may be `(originZ − world)·0.5`. Confirm sign
  per page at calibration.
- **Underground (12) and DLC (40–43)** are separate pages — same Step-B form, own origins;
  the open page is `menu+0x151` / `DAT_143d6cfc3` (projection doc §3). Draw only markers
  whose dstPage == open page.
- Offsets version-specific; resolve by AOB. SCALE 0.5 and the per-page-origin structure are
  the stable contract.
```
