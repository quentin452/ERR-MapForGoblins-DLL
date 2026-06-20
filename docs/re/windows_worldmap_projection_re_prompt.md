# Task: RE the world-map VIEW/PROJECTION transform (Windows, Ghidra) — overlay-rendered markers

You are on **Windows** with **Ghidra/IDA** + a **debugger attached to the running game**
(Cheat Engine + x64dbg). Repo: **ERR-MapForGoblins-DLL** (Elden Ring world-map icon mod),
app **2.6.2.0 / ERR 2.2.9.6**. Imagebase `0x140000000`. **Resolve everything by AOB, not RVA**
(VMProtect drifts RVAs every patch; RVAs below are reference for THIS build only). Analyse the
MSVC `.text` at VA `0x140001000`, NOT the VMProtect `.text` at `0x144c0e000`.

Read first (don't re-derive what they already pin): `docs/re_findings_questbrowser_cursor.md`
(the cursor object — DONE), `docs/windows_csworldmapmenu_re_prompt.md` + `docs/windows_live_refresh_re_prompt.md`
(subsystem map, anchor strings, CSMenuMan+0xCD open byte).

## Why — the new architecture

The mod currently shows markers by injecting `WorldMapPointParam` rows. The map-open freeze
scales with on-page row count, and there's no live refresh. **New direction: inject ZERO
rows and draw ALL goblin markers ourselves in our ImGui/DX12 overlay (Present hook, already
shipped), projected onto the open game map.** That kills freeze + live-refresh + phantom at
once. The **one blocker** is the **world→screen projection**: each frame we must map a marker
at `(areaNo, gridXNo*256+posX, gridZNo*256+posZ)` (marker space) to a **screen pixel** so our
icon lands exactly where the game draws its own. This task = deliver that transform.

## What we ALREADY proved (use it — do NOT redo)

- **Cursor object = CONFIRMED.** `CS::WorldMapCursorControl`, vtable RVA `0x2b29a90`. Fields:
  `+0xFC` = **X**, `+0x104` = **Z**, both in **marker space** (== `WorldMapPointParam`
  posX/posZ + grid). Live-confirmed via our in-DLL probe: panning swept `+0xFC` 3203→4338,
  `+0x104` 2804→4521. (`+0x10C` mirrors Z too; not Y.) Cursor is embedded at **`container+0x2DB0`**.
- **The object at `cursor−0x2DB0` is NOT `CSWorldMapMenu`.** It's an intermediate container
  (RE doc §3). We dumped a per-frame float-DELTA scan of the **cursor object (0x400 window)**
  and that **container (0x3200 window)** while panning + zooming:
  - cursor window: ONLY the coord floats `+0xFC/+0x100/+0x104/+0x108/+0x10C` change. Nothing else.
  - container window: only a `+0x750..+0x910` band changes, and it is **garbage scratch**
    (one offset jumped `0.079 → 86 → −199 → 217371 → 365742 → 1.000` frame-to-frame).
  → **The view transform (center / zoom / screen-rect) is NOT inline in either object.** It
  lives in the real `CSWorldMapMenu` (separate, unresolved) or behind a pointer to a
  camera/view sub-object. **That chain is exactly what you must resolve.**
- **World-map subsystem FD4Singleton ptr** at RVA `0x3d5dea8` (VA `0x143d5dea8`); cursor tick
  guards on `[ptr]+0x883` (candidate map-open/ready byte). NOT yet proven to be the menu.
- **CSMenuMan+0xCD** == 7 when the world map is open (`0→3→7`). Singleton AOB
  `48 8B 05 ?? ?? ?? ?? 33 DB 48 89 74 24`.

## The strongest lead — the edge-scroll pan function

`FUN_1409bdc50` (RVA `0x9bdc50`, AOB
`40 53 55 56 57 41 56 48 81 ec 80 00 00 00 48 8b 05 fb 45 e4 03 33 f6 49 8b e8`) **computes an
edge-scroll factor from the cursor coordinate vs the on-screen rect** (RE doc §2). To do that
it MUST read: the cursor marker coord (have it), the **map view rect in screen space**, and the
**marker↔screen scale (zoom)**. Static-decompile this function and trace which object/offsets
it loads for the screen rect and the scale. Likewise the cursor **tick** `FUN_1409bd4b0`
(vt[2], AOB `48 8b c4 55 53 56 57 41 54 41 56 41 57 48 8d 68 a1 48 81 ec c0 00 00 00`) clamps
the cursor to a bounds rect via `FUN_1409cd790` — follow its bounds-rect pointer (`cursor+0xF0`
→ `[+0x340..0x34c]` = minX/minZ/maxX/maxZ in MARKER space). If that **visible-extent rect
shrinks on zoom-in / grows on zoom-out**, then `screen(marker)` is a trivial affine from
(visible marker rect) → (screen widget rect) and we may not need a separate zoom field at all —
**verify this rect is live and tracks zoom + pan** (our flat dump missed it because it's behind
the +0xF0 pointer).

## Deliverables

1. **The projection transform**, as an offset chain from a resolvable singleton (the
   `CSWorldMapMenu` instance, or the camera/view sub-object) to:
   - **view center** in marker space (the marker coord at the screen-rect center), OR the
     live **visible-extent rect** [minX,minZ,maxX,maxZ] if that's what the engine keeps;
   - **zoom / scale** (pixels per marker unit), if separate from the extent rect;
   - **map widget screen rect** [left, top, right, bottom] — and **state its coordinate space**:
     actual swapchain/backbuffer pixels, or a virtual 1920×1080 canvas the game scales? (Our
     overlay draws in backbuffer space via the DXGI Present hook — we need the conversion.)
   Plus the **singleton getter AOB** and the full pointer chain.
2. **A C++ `world_to_screen` snippet** (AOB-resolved, SEH-guarded, `modutils::scan`-style) that
   takes `(areaNo, markerX, markerZ)` and returns screen `(px, py)` for the CURRENTLY displayed
   page — matching where the game draws that marker. Read-only; we never write engine state.
3. **Current-page / area field:** which field on the menu holds the currently-displayed map
   page/area (overworld vs underground vs each DLC page), so we draw only markers whose
   `areaNo` belongs to the open page. Note the area→page mapping if non-trivial (underground +
   DLC are separate pages; the transform is per-page).
4. **Runtime evidence** (debugger/screenshot): pick a known grace from
   `data/grace_position_index.json` (fields areaNo/gridX/gridZ/x/z; marker = `grid*256 + pos`),
   run your transform on its marker coord, and show the predicted screen pixel lands on the
   game's own grace icon — and that it **tracks through pan AND zoom**. This is the make-or-break.
5. If the engine keeps a 3×4/4×4 view matrix instead of discrete center/zoom/rect, deliver the
   matrix offset + the row/column order + how to apply it (marker→clip→screen). State which form
   you delivered.

## Notes / caveats

- Confirm against the running game; offsets are version-specific. Give RVAs as reference, AOBs
  as the contract.
- Map is **not rotated** (axis-aligned), so an affine (scale_x, scale_z, tx, tz) suffices per
  page/zoom if there's no matrix.
- Beware DPI / resolution scaling and ultrawide: state whether the screen rect is fixed-virtual
  or live-resolution. The overlay knows the real backbuffer size (`ResizeBuffers` hook).
- The cursor probe (`debug_worldmap_probe` + `debug_wm_transform_scan`) is in
  `src/goblin_worldmap_probe.cpp` — reuse its vtable-scan handle to the live cursor/menu for
  your dynamic verification, and extend it if you need to dump a pointer-followed sub-object.
