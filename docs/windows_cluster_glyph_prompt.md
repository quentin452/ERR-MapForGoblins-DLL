# Task: add a distinct cluster glyph to the worldmap GFX (Windows, all profiles)

You are on a **Windows** box with **ERR-MapForGoblins-DLL** cloned and the gfx
pipeline working (FFDEC available — `FFDEC_CLI` env var or the bundled install;
`tools/config.ini` with valid game/mod dirs; the per-profile base
`menu/02_120_worldmap.gfx` reachable). The gfx bake runs here.

## Background

Marker **clustering** (Thread 5 v1, shipped) collapses dense marker piles into one
synthetic "cluster" row. That row currently borrows the **anonymous gray "?" frame**
as a placeholder icon — it reads as "unknown", not "a pile of N markers". This task
gives clusters their **own distinct glyph**.

How worldmap icons work in this repo (read `tools/build_vanilla_gfx.py` — it's the
authority):
- Icons are Scaleform **GFX**; **sprite 171** is the icon frame-sheet. Each `iconId`
  on a `WorldMapPointParam` row selects a frame (1-based).
- Our custom frames live in `assets/menu/02_120_worldmap_new.gfx` (OURS_GFX),
  appended after the base frames (vanilla base = 348 frames, ours start at 349).
  Each frame is a self-contained `RemoveObject2 + PlaceObject3 + ShowFrame` group,
  referencing a `DefineExternalImage2` texture (our added charIds 1000-1024 = vanilla
  texture names; ERR-only charIds 13500-13506 must NOT leak).
- `build_vanilla_gfx.py` rebuilds the icon set on the target game's own base per
  profile (vanilla/convergence/erte), shifting iconIds by `config.ICON_FRAME_OFFSET`.
- The **anon "?" frame** is the last of our added frames → `ANON_ICON_ID = 441` on a
  vanilla base (`src/generated/goblin_item_icons.cpp`; written by
  `tools/generate_data.py:404`; kept in sync by `build_vanilla_gfx.py:435`).

DLL side (do NOT confuse the two uses of ANON):
- `src/goblin_inject.cpp:850` — `cd->iconId = ANON_ICON_ID` on **cluster** rows
  (`// v1 placeholder glyph`). **THIS is what the new glyph replaces.**
- `src/goblin_inject.cpp:766` — `live_icon_override[...] = ANON_ICON_ID` for
  anonymized **live-loot** items. **Leave this as the "?" frame.**

## Goal

Add ONE new frame after the anon frame, holding a glyph that reads as "cluster / many
markers here", and point cluster rows at it via a new `CLUSTER_ICON_ID`.

## Steps

### 1. Author the glyph frame in OURS_GFX (sprite 171, after the anon frame)

Two options — prefer (A):

- **(A) Reuse an existing vanilla worldmap texture, recolored/badged.** Cheapest and
  carries no ERR-only refs: clone the anon frame's `PlaceObject3` group, swap the
  matte/tint (e.g. a filled disc or a "stacked" look) so it's visually distinct from
  both the "?" and the loot/grace icons. Distinct **shape+color** matters more than
  detail — at map zoom it's a few px. **User requirement: clusters must be visually
  distinct and are never themselves clustered**, so don't reuse 370(grace)/371(loot)/
  374(hostile)/the "?" look.
- **(B) New texture.** Add a `DefineExternalImage2` (charId in 1000-1024 range, vanilla
  texture name only) + DDS, then a frame referencing it. More work; only if (A) can't
  read clearly.

Append it as the next frame (one past the anon frame) and bump sprite 171's
`frameCount`. Keep it self-contained (RemoveObject2+PlaceObject3+ShowFrame) like the
others. Must reference NO ERR-only charId (13500-13506) — `build_vanilla_gfx.py`
verifies this and will fail the bake if violated.

### 2. Wire CLUSTER_ICON_ID

- `tools/generate_data.py` (~line 404, beside the `ANON_ICON_ID` write): emit
  `const uint16_t CLUSTER_ICON_ID = <anon+1>u;` and declare it in
  `goblin_item_icons.hpp`. It's `ANON_ICON_ID + 1` (the frame right after anon), so it
  inherits the same per-profile `ICON_FRAME_OFFSET` shift automatically.
- `tools/build_vanilla_gfx.py` (~line 435, the ANON sync check): extend the
  frame-count assertion to expect the extra frame (anon + cluster = 2 appended tail
  frames now), so a base-frame-count drift is still caught.
- DLL: change `src/goblin_inject.cpp:850` to
  `cd->iconId = goblin::generated::CLUSTER_ICON_ID;` (leave line 766 = ANON).

### 3. Bake all three profiles + sanity-check

```bat
py tools\build_vanilla_gfx.py --profile vanilla
py tools\build_vanilla_gfx.py --profile convergence
build.bat generate            :: re-bakes goblin_map_data.cpp + the *_ICON_ID consts
```
(erte uses the vanilla base = 348 frames.) Check: bake passes the no-ERR-charId +
frame-count asserts; `CLUSTER_ICON_ID` lands = `ANON_ICON_ID + 1` per profile;
enabling clustering in-game shows the new glyph on cluster rows (not the "?").

### 4. Deliver

Return:
1. The updated **`assets/menu/02_120_worldmap_new.gfx`** + the rebuilt per-profile
   `assets/menu/02_120_worldmap_{vanilla,convergence}.gfx`.
2. The regenerated **`src/generated/goblin_item_icons.{hpp,cpp}`** (+ `goblin_map_data.cpp`)
   showing `ANON_ICON_ID` and the new `CLUSTER_ICON_ID`.
3. The exact frame XML you added (so the Linux side can review the glyph choice) and a
   screenshot of a cluster in-game with the new icon.

Note: this is GFX/asset only — the DLL one-line swap (step 2, line 850) lands on the
Linux side. If FFDEC runs headless on this box, the whole thing is scriptable; if the
glyph needs hand-art, (A) reuse-and-recolor avoids a DDS authoring round-trip.
