# Cluster-depleted (GREEN) glyph — worldmap GFX (Windows, all profiles)

**Status: DONE on `feat/cluster-done-glyph` (off `master`).** GFX/asset side complete,
baked + verified on all 4 profiles. The DLL-side depleted-detection + live iconId swap
lands on the Linux side (see "Remaining").

## What it is

A GREEN sibling of the teal cluster glyph: the DLL swaps a cluster's `iconId` to
`CLUSTER_DONE_ICON_ID` once **all its members are collected**, so a depleted pile reads
as done instead of showing a stale count. Same "stack of dots" family as the teal
cluster, recoloured green (cleared/done) — distinct from teal cluster / blue quest-NPC /
gold grace / red hostile.

## Implemented (4th appended frame)

Frame layout on sprite 171, offset-0 bases (vanilla/erte/err); +408 on convergence:

| frame (0-idx) | iconId | shape | glyph |
|---|---|---|---|
| 440 | 441 | 1099 | anon "?" |
| 441 | 442 | 1100 | cluster (teal) |
| 442 | 443 | 1101 | quest-NPC (blue bust) |
| **443** | **444** | **1102** | **cluster-depleted (green)** |

`CLUSTER_DONE_ICON_ID = ANON_ICON_ID + 3` = **444** (offset-0) / **852** (convergence).

- `tools/make_cluster_done_icon.py` — green "stack of dots" (FILL=(60,190,95)),
  `assets/badges/cluster_done_glyph.png` (160×160).
- `tools/build_vanilla_gfx.py` — `add_cluster_done_icon` (shape 1102), wired into
  `main()` + `build_err_anon_gfx()` after quest-NPC, landing assert (443 + base offset).
- `tools/generate_data.py` — emits `CLUSTER_DONE_ICON_ID = anon+3`;
  `src/generated/goblin_item_icons.{hpp,cpp}` declare/define it (444). Regen of the
  cpp reproduces the committed consts exactly.

## Verified

All 4 profiles rebaked: shapes 1099/1100/1101/1102 all embedded; frameCount 444
(vanilla/erte/err), 852 (convergence); cluster-done frame lands at index 443 / 851;
no ERR-only charId leak; frame-count asserts pass. `goblin_map_data.cpp` unchanged
(cluster-done is a runtime DLL swap, not a baked marker).

## Remaining (Linux / DLL)

Count remaining members per cluster (e.g. `collected::is_row_collected`); when 0
remain, swap the cluster row's `iconId` to `goblin::generated::CLUSTER_DONE_ICON_ID`
(`iconId` is a mutable param field, like the existing `CLUSTER_ICON_ID` write — no RE).
Then runtime-test: a fully-looted cluster shows the green glyph.
