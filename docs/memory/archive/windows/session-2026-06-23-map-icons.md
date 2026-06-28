---
name: session-2026-06-23-map-icons
description: Recap of the 2026-06-23 session — grace native-pin suppression chain + map-point iconId→rect from RAM; where things stand + next steps
metadata: 
  node_type: memory
  type: project
---

Session on branch `docs/worldmap-projection-re-brief` (ERR-MapForGoblins-DLL). Two threads, all
RE on `<ghidra_project>\ER` (process-mode headless, see [[ghidra-worldmap-re]]) + live RPM.

## Thread A — native grace pin: draw-only suppression that keeps teleport (DONE, shipped & committed)
- Draw-gate = byte `pin+0xC` (cached visible), recomputed each frame by `vt[3] FUN_14087afa0` from the
  per-layer STATE bitmask `pin+0x60`. (Ghidra `param_1` is undefined8* → its "param_1+0xc" = byte 0x60.)
- Draw AND cursor selection are COUPLED via the GFx widget `_visible` (= pin+0xC): selection
  `WorldMapItemControl vt[6] FUN_1409cab60` reads pin+0xC. So zeroing +0x60/+0xC kills teleport too.
- WORKING fix (runtime-confirmed): hook `vt[1] SetTo FUN_14087ae20`, toggle pin+0xC=0 **before** orig
  (SetTo's own set_visible hides the row while its proxy is live) and **restore after** (so vt[6]
  selection keeps the grace clickable). Committed (`06e78bf`, `5c15d2e`, log removed `6ef30ce`).
- ALSO committed: page-transition view-delay snap on ANY page change (`c6dcc13`); SetTo proxy-ABI
  findings (`bd1ac18`/`93efb75`). Full docs in docs/re/windows_grace_warppin_*.md.
- `grace_suppress_native` defaults false. Sub-dialog gate via `dialog+0xa58` = DEAD END (reads const
  64, wrong object) — reverted.

## Thread B — map-point icon iconId→rect (DATA DONE, render TODO)
- Texture pipeline already solved upstream (Oodle IAT-hook grabs ER's transient DDS → own texture mgr
  → sheet-as-atlas; item iconId→rect via find hook). Gap was MAP-POINT icons.
- **SOLVED 2026-06-23:** map-point icons resolve from the RESIDENT image repo. `MENU_MAP_<NN>` (NN =
  WORLD_MAP_POINT_PARAM.iconId) + `MENU_MAP_ERR_*`/`Church`/… live in `repo+0x80` (the by-name tree,
  er+0x3d82510), NOT the +0xb0 twin (that holds MENU_MapTile_*). rect = `img+0x74..0x80` = (x0,y0,x1,y1),
  sheet = `[img+0x10]+0x70` (ID3D12Resource). 3 sheets (SB_MapCursor, SB_MapCursor_ERR, +1).
- SHIPPED (uncommitted at recap time → being committed now): added a `MENU_MAP_` branch to
  `harvest_repo_icons` (repo+0x80 walk) → `store_map_icon_rect` → `g_map_icon_rects`(iconId)/
  `g_map_icon_named`(+sheet). Getters `goblin::map_icon_rect(iconId,x,y,w,h,sheet&)`,
  `map_icon_rect_by_name`, `map_icon_layout_count`. Auto-fills on map open. RUNTIME-CONFIRMED via
  `[MAPRECT]` (since removed): MENU_MAP_Church (1660,150,1800,338) etc.
- Dead ends proven & removed this session: the sblytbnd Oodle/CSFile hook approach — the decompressed
  BND4/.layout XML is FREED after boot parse (`<SubTexture` nowhere in heap; force_load returns a
  cached file-wrapper, not the BND4). Offline tools kept: tools/dump_layout_names.py, tools/probe_dcx.py;
  RPM probes in <ghidra_scripts> (walk_map_repo.py, probe_sblyt_res.py).

## NEXT (not done)
1. **Render**: wire a MapPointProvider in the overlay — per marker, `map_icon_rect(iconId)` →
   `copy_sheet_cached(sheet)` → draw with uv = rect/sheetDims (one draw-call/sheet, like item icons).
2. ERR-custom points: map their iconIds → `MENU_MAP_ERR_*` names (per profile).
3. Open: minimap vs ER bottom-right HUD overlap → user wants a GENERAL HUD clipper (can't z-order
   behind ER; emulate via config exclusion rects or just reposition). Still undecided.
