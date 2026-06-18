# Cluster glyph for the worldmap GFX (Windows, all profiles)

**Status: IMPLEMENTED on `feat/cluster-glyph` (off `feat/clustering`).** Code + the
glyph asset are done and self-verified; the GFX **bake is still pending** (FFDEC was
not installed on the dev box at implementation time — see "Bake" below). This doc
records what was built and how the worldmap-icon pipeline *actually* works (the
original task prompt described a stale "append from OURS_GFX" model and pre-refactor
line numbers — corrected here).

## Background

Marker **clustering** (Thread 5 v1, shipped) collapses dense marker piles into one
synthetic "cluster" row. That row used to borrow the **anonymous gray "?" frame** as a
placeholder — it reads as "unknown", not "a pile of N markers". This change gives
clusters their **own distinct glyph**.

How worldmap icons actually work in this repo (read `tools/build_vanilla_gfx.py` — it's
the authority):
- Icons are Scaleform **GFX**; **sprite 171** is the icon frame-sheet. Each `iconId` on
  a `WorldMapPointParam` row selects a frame (1-based).
- Our 92 custom icons live in `assets/menu/02_120_worldmap_new.gfx` (OURS_GFX); the bake
  appends them after the base frames (vanilla base = 348, ours = 349..440), referencing
  `DefineExternalImage2` textures (our charIds 1000-1024 = vanilla texture names;
  ERR-only charIds 13500-13506 must NOT leak — the bake fails if they do).
- The **anon "?" frame is NOT in OURS_GFX.** `build_vanilla_gfx.py` *synthesises* it at
  bake time: clone the single-figure badge shape **172** → a free charId, append one
  self-contained `RemoveObject2 + PlaceObject3(clone) + ShowFrame` frame (a standalone
  round icon, no bg, centred), then `ffdec -replace` that shape with a 160px PNG raster
  (FFDEC can't import detailed vectors). It lands at 0-indexed `ANON_FRAME_INDEX = 440`
  → 1-based `ANON_ICON_ID = 441` on a vanilla base (`+ICON_FRAME_OFFSET` on bases that
  add their own frames, e.g. Convergence). Same trick as the cleared badge.
- `ANON_ICON_ID` is written by `tools/generate_data.py` and the landing is asserted in
  `build_vanilla_gfx.py`'s anon block; the canonical value lives in
  `src/generated/goblin_item_icons.{hpp,cpp}`.

DLL side — the two uses of `ANON_ICON_ID` are NOT the same (do not conflate):
- `src/goblin_inject.cpp:813` — `cd->iconId = ...` on **cluster** rows. **This is what
  the new glyph replaces** → now `CLUSTER_ICON_ID`.
- `src/goblin_inject.cpp:731` — `live_icon_override[...] = ANON_ICON_ID` for anonymized
  **live-loot** items. **Left as the "?" frame.**

## What was implemented

The cluster frame is synthesised exactly like the "?" — a second cloned-shape round
icon, appended **one frame past the anon "?"**, so `CLUSTER_ICON_ID = ANON_ICON_ID + 1`
and it inherits the same per-profile offset automatically.

1. **Glyph asset** — `assets/badges/cluster_glyph.png` (160×160 RGBA), drawn
   procedurally by **`tools/make_cluster_icon.py`** (mirrors `make_anon_icon.py`): three
   overlapping filled discs ("stack of dots") in saturated teal with a dark outline +
   light inner rings. Distinct from the gray "?", grace, loot, and hostile — and
   clusters are never themselves clustered, so it never needs to nest. No hand-art /
   DDS round-trip. (Re-run `py -3.13 tools\make_cluster_icon.py` to tweak shape/color.)

2. **`tools/build_vanilla_gfx.py`** —
   - Refactored `add_anon_icon` into a shared `_append_round_glyph(vroot, vsprite,
     shape_id)`; `add_anon_icon` / `add_cluster_icon` are thin wrappers.
   - Added `CLUSTER_PROFILES`, `CLUSTER_IMG`, `CLUSTER_FRAME_INDEX = 441`,
     `CLUSTER_GLYPH_SHAPE = 1100` (anon used 1099).
   - `add_cluster_icon` is called **after** `add_anon_icon` in both `main()` and
     `build_err_anon_gfx()`, with a landing assert (`441 + base offset`) so base-frame
     drift is still caught; then the PNG is `-replace`d into shape 1100.

3. **`tools/generate_data.py`** — emits `const uint16_t CLUSTER_ICON_ID = anon+1` beside
   the `ANON_ICON_ID` write.

4. **`src/generated/goblin_item_icons.{hpp,cpp}`** — declared/defined `CLUSTER_ICON_ID`
   (`= 442u` on a vanilla/erte/err base, offset 0). Verified: re-running the generator
   reproduces the committed `.cpp` byte-for-byte.

5. **`src/goblin_inject.cpp:813`** — cluster rows now use `CLUSTER_ICON_ID` (line 731
   live-loot anon left on `ANON_ICON_ID`).

## Bake (remaining — needs FFDEC)

```bat
set FFDEC_CLI=java -jar D:\path\to\ffdec-cli.jar   :: or fix the path in ffdec_cmd()
py tools\build_vanilla_gfx.py --profile vanilla
py tools\build_vanilla_gfx.py --profile convergence
py tools\build_vanilla_gfx.py --profile err
build.bat generate            :: re-bakes goblin_map_data.cpp + the *_ICON_ID consts
```
(erte uses the vanilla base = 348 frames.) Each bake prints `cluster glyph: appended
frame at index 441/849/441` and embeds the PNG. Checks: no-ERR-charId + frame-count
asserts pass; `CLUSTER_ICON_ID == ANON_ICON_ID + 1` per profile; with clustering enabled
in-game, cluster rows show the teal glyph (not the "?").

## Deliver (after bake)

1. The rebuilt per-profile `assets/menu/02_120_worldmap_{vanilla,convergence,err}.gfx`.
2. `src/generated/goblin_item_icons.{hpp,cpp}` showing `ANON_ICON_ID` + `CLUSTER_ICON_ID`
   (already committed).
3. A screenshot of a cluster in-game with the new icon.
