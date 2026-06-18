# Task: add a GREEN "cluster depleted" glyph to the worldmap GFX (Windows, all profiles)

You are on a **Windows** box with **ERR-MapForGoblins-DLL** cloned and the gfx
pipeline working (FFDEC/JPEXS — `FFDEC_CLI` env or `.env.local`; per-profile base
`menu/02_120_worldmap.gfx` reachable). The gfx bake runs here.

## Background

Clustering collapses dense marker piles into one cluster icon (`CLUSTER_ICON_ID`,
the teal "stack of dots" you already baked as the 3rd appended frame). Now we want
a SECOND cluster glyph — a **green** "done/depleted" variant — so the DLL can swap a
cluster's icon to green once **all its members are collected** (instead of the icon
lying with a stale count). This task produces only that green frame + its id; the
DLL-side depleted-detection + live iconId swap lands on the Linux side.

How the icon frames work (you built the last three — same mechanism):
- Custom frames append to **sprite 171** of `02_120_worldmap_new.gfx`; the bake
  (`tools/build_vanilla_gfx.py`) synthesises each by cloning the badge shape, adding
  a self-contained `RemoveObject2 + PlaceObject3 + ShowFrame` frame, then
  `ffdec -replace`-ing the shape with a 160px PNG raster.
- Current tail frames (offset-0 bases vanilla/err/erte): anon **441** (shape 1100…
  actually the "?" frame), cluster **442** (teal, shape 1100, `make_cluster_icon.py`),
  quest-npc **443** (blue bust, shape 1101, `make_quest_npc_icon.py`). On convergence
  add `ICON_FRAME_OFFSET` (408).
- Each id is `ANON_ICON_ID + N`, written by `tools/generate_data.py`, landing
  asserted in `build_vanilla_gfx.py`, canonical value in
  `src/generated/goblin_item_icons.{hpp,cpp}`.

## Goal

Add the green cluster-done glyph as the **4th** appended frame → `CLUSTER_DONE_ICON_ID`
= `ANON_ICON_ID + 3` (= **444** on offset-0 bases; **852** on convergence).

## Steps
1. **`tools/make_cluster_done_icon.py`** (new) — copy `make_cluster_icon.py`; same
   "stack of dots" silhouette but **green** so it reads as "cleared/done" and is
   distinct from teal cluster / blue quest-npc / gold grace / red hostile. Suggested
   `FILL = (60, 190, 95, 255)` (saturated green), keep the dark outline + light ring.
   Output `assets/badges/cluster_done_glyph.png` (160×160 RGBA).
2. **`tools/build_vanilla_gfx.py`** — add an `add_cluster_done_icon` (shape **1102**)
   mirroring `add_quest_npc_icon`; wire it into `main()` + `build_err_anon_gfx()` so it
   lands as frame index 443 (offset-0) / 851 (conv); update the landing assert
   (tail frames now = anon + cluster + quest-npc + cluster-done = 4).
3. **`tools/generate_data.py`** — emit `const uint16_t CLUSTER_DONE_ICON_ID = <anon+3>u;`
   + declare in `goblin_item_icons.hpp` (same pattern as CLUSTER_ICON_ID).
4. **Bake all 4 profiles** + sanity: `py tools\build_vanilla_gfx.py --profile vanilla`
   / `--profile convergence`; `build.bat generate`. Verify no ERR-only charId leak,
   frame-count asserts pass, `CLUSTER_DONE_ICON_ID == CLUSTER_ICON_ID + 2` per profile
   (442→444 / 850→852), and shape 1102 embedded.

## Deliver
1. `assets/badges/cluster_done_glyph.png` + the 4 rebuilt `assets/menu/02_120_worldmap_*.gfx`.
2. Regenerated `src/generated/goblin_item_icons.{hpp,cpp}` showing all four ids
   (ANON 441, CLUSTER 442, QUEST_NPC 443, CLUSTER_DONE 444).
3. The new make script + the frame XML you added, and a screenshot of the green glyph.

Note: GFX/asset only. The DLL change (count remaining members per cluster via
`collected::is_row_collected`, swap the cluster row's `iconId` to CLUSTER_DONE_ICON_ID
when 0 remain) lands on Linux — no RE, `iconId` is a mutable param field like areaNo.
Don't push a branch beyond what's needed; deliver the files + ids.
