# Task: RE the marker param-row → world-map RENDER coordinate transform (Windows, Ghidra)

You are on **Windows** with **Ghidra/IDA** + a debugger attached to the running game (Cheat
Engine + x64dbg). Repo: **ERR-MapForGoblins-DLL** (Elden Ring world-map icon mod), app
**2.6.2.0 / ERR 2.2.9.6**. Imagebase `0x140000000`. **Resolve by AOB; RVAs are reference for
this build only.** Analyse the MSVC `.text` at VA `0x140001000`, NOT the VMProtect `.text`.

Read first: `docs/world_map_projection_re_findings.md` (the VIEW transform + cursor — DONE),
`docs/re_findings_playerpos.md` (marker world = gridX·256+pos, CELLSIZE 256), `docs/windows_live_refresh_re_prompt.md`
+ `docs/windows_csworldmapmenu_re_prompt.md` (icon-build subsystem map).

## Why — the missing piece

We are building **overlay-rendered markers**: draw markers ourselves in our ImGui/DX12
overlay, projected onto the open map. The **screen projection is SOLVED** (per
`world_map_projection_re_findings.md`: `screen = (markerCoord − viewCentre)·zoom + canvas/2`,
verified to sub-pixel on the live reticle). The cursor/reticle position lives in a **render
"marker space"** (the fullRect is `[0,0,10496,10496]`; cursor `+0x104`/`+0x108`).

**The blocker:** converting a baked `WorldMapPointParam` row `(areaNo, gridXNo, gridZNo, posX,
posZ)` into THAT render marker space. The naive `gridXNo·256 + posX` is the mod's INTERNAL
world coord (used for clustering/labels) but it is **NOT** the render space — verified live:

- **Dragonbarrow Cave** (area 21, Caelid): our `gridX·256+pos`-derived world `(12400, 11985)`;
  the reticle hovered on the real grace reads `+0x104 = 6018.75`, `+0x108 = 6187.28` → ratio
  ≈ **0.50**.
- **Main Academy Gate** (Raya Lucaria, area 16): reticle reads `+0x104 ≈ 1826`, but our
  transitive-conv world comes out `~13500` (should be ~3650) → **wrong region entirely**.
- **Native overworld (area 60)** rows: `gridXNo` starts ~40 (not 0), so `gridX·256` is offset
  → a pure scale can never align them (they all pile into Caelid).

So the render-space transform is **per-page and offset-bearing**, exactly the thing the game
computes internally and we cannot guess: **overworld (60/61)** uses tile/region origins,
**legacy dungeons (10/11/16…)** project onto the overworld via `WorldMapLegacyConvParam`
**transitively** (m35 → area 11 → area 60), **underground (12)** and **DLC (40–43)** are
separate pages with their own conversions.

## Goal — find the conversion the game uses to PLACE a marker

The game's world-map icon build reads each `WorldMapPointParam` row and computes a position in
the same render space the cursor uses. Find that computation and deliver it as a formula we can
replicate in C++ (read-only; we never call it live).

Strongest leads (the build path, from the live-refresh RE):
- **`CS::CSWorldMapPointManImplement`** (FD4Singleton) owns the placed `CSWorldMapPointIns`.
  Build/refresh routine `FUN_140a832a0` iterates the point list; `_DiscoverMapPoint`
  (`FUN_140a84080`, string VA `0x142b48c1c`) reveals one point. The icon (re)build that NEWs
  `CSWorldMapPointIns` from a param row is the place a row's render position is set.
- **`CSWorldMapPointIns`** instance: find the field(s) holding the computed render/marker-space
  position (likely a vec2/vec3). If the build stores the final render coord on the instance,
  reading it for one row + diffing vs the row's `(area,grid,pos)` reveals the transform per page.
- **`WorldMapLegacyConvParam`** (baked as `LEGACY_CONV`): the dungeon→overworld projection. The
  game applies it — confirm whether it's applied ONCE or TRANSITIVELY (chain `src_area`→…→60),
  and the exact arithmetic (our single-hop `dst_gx·256 + dst_pos` is wrong for transitive areas).
- **`WorldMapPlaceNameParam`**: candidate source of per-region/page ORIGINS on the assembled
  world-map (the offset that native area-60 needs). Check if the build looks a row's region up
  here to get a base point, then adds the local `(grid,pos)`.
- Cross-ref the params `worldMapCursorSpeed`/`worldMapSnapRadius` consumers + the cursor tick
  (`FUN_1409bd4b0`) — the cursor and markers share the render space, so the cursor clamp/bounds
  (`WorldMapArea +0x350` fullRect 10496) define the target coordinate frame.

## Deliverables

1. **The transform**, as a formula `(areaNo, gridXNo, gridZNo, posX, posZ) → (renderX, renderZ)`
   in the cursor's marker space (the one where `screen = (render − viewCentre)·zoom + canvas/2`
   holds), for EACH page class:
   - overworld native (area 60 / 61): the tile/region origin + scale (explain the ~0.5 factor and
     the gridXNo≈40 offset — is render = `(gridXNo − originX)·cell` with cell=128? confirm cell &
     origin from the data, don't assume),
   - legacy dungeons (10/11/16/35…): the LegacyConv projection, including the TRANSITIVE chain,
   - underground (12) and DLC (40–43): their page conversions (or confirmation they render on a
     separate page, not the overworld).
2. **Where it comes from** (AOBs): the build function + the param singleton getters
   (`CSWorldMapPointMan`, `WorldMapLegacyConvParam`, `WorldMapPlaceNameParam`) it reads, and which
   fields/columns feed the formula. RVAs as reference, AOBs as the contract.
3. **Runtime proof**: pick 2–3 graces across pages (e.g. Dragonbarrow area 21, Academy Gate area
   16, a native area-60 Limgrave grace), run your formula on their baked `(area,grid,pos)`, and
   show the result matches the reticle's `+0x104/+0x108` when hovered on that grace (the values
   above are real measurements to check against: Dragonbarrow → 6018.75 / 6187.28).
4. If the cleanest answer is **read the field off `CSWorldMapPointIns`** rather than a closed-form
   formula, deliver that offset chain instead (with the caveat that it requires the game to have
   built the icons — note it; we prefer the formula since overlay-markers aims to avoid the build).

## Notes
- The mod already bakes `LEGACY_CONV` (WorldMapLegacyConvParam) and `WorldMapPlaceNameParam`
  data — so if the transform is "look up region origin + add local", we can bake the same tables;
  we just need the EXACT arithmetic + the transitive chaining rule.
- Marker world (mod-internal) = `gridXNo·256 + posX`, signed pos, CELLSIZE 256
  (`re_findings_playerpos.md`). The RENDER space differs by a per-page origin + scale — that delta
  is the deliverable.
- Resolve-by-AOB; offsets are version-specific. Reuse the cursor vtable-scan handle
  (`0x142b29a90`, `goblin_worldmap_probe.cpp`) for live verification.
