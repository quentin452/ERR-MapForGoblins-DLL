# RE findings — world-map world→screen PROJECTION (Windows, Ghidra)

Answers `docs/windows_worldmap_projection_re_prompt.md`. Static Ghidra RE
(project `D:\ghidra_proj2\ER`, scripts `re_v45..v50.java`, outputs `out_v45..v50.txt`).
App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. **Resolve by AOB; RVAs below are
reference for this build only.** Static-only so far — the runtime confirmation step
(probe extension + the make-or-break pan/zoom test) is described in §6/§7; quentin
runtime-tests (see workflow notes).

---

## 0. TL;DR — the headline result

**There is NO flat C++ view-matrix / center / zoom you can read at a fixed offset for
the final marker→pixel step. The world map is rendered by Scaleform (GFx/Flash).** The
forward transform is a `Scaleform::Render::Matrix2x4<float>` (twips, ×20 px) living
inside the GFx movie — that is exactly why the earlier flat float-delta dump of the
cursor + container objects never found a center/zoom.

**BUT** the pan/zoom of the map view *is* exposed as plain floats on a C++ object —
just behind the `cursor+0xF0` pointer (which the flat dump didn't follow). The view
transform decomposes into two stages:

```
stage 1 (C++, readable):   view_local = (marker + pan) / zoom        FUN_1409cd0a0
                              pan  = WorldMapArea + 0x378  (vec2, marker space)
                              zoom = WorldMapArea + 0x380  (f32 scalar)
stage 2 (Scaleform):       screen_px = M2x4 · view_local             (movieclip → 1920×1080 stage)
stage 3 (our overlay):     backbuffer_px = screen_px · (realW/1920, realH/1080)
```

Because the map is axis-aligned (no rotation) and stage 2's movieclip placement on the
stage is fixed (the panning/zooming is done in stage 1, not by moving the movieclip),
**the whole chain collapses to one per-frame affine** `pixel = A·marker + B` whose A,B
depend only on (`pan`,`zoom`) and the fixed stage placement. Two viable
implementations, §5.

Screen space is a **virtual 1920×1080 canvas** (hard-coded `1920.0`/`1080.0` in the pan
fn and the pan-bounds fn). Our overlay draws in real backbuffer pixels → scale by
`realW/1920`, `realH/1080` (state the ultrawide caveat, §7).

---

## 1. Object graph (confirmed)

```
WorldMapCursorControl  (vtable RVA 0x2b29a90)   ← scanned by the existing probe
  +0xFC  f32  cursor X   (marker space == WorldMapPointParam.posX + gridXNo·256)
  +0x100 f32  cursor X-partner (tick writes the pair +0xFC/+0x100)
  +0x104 f32  cursor Z
  +0x10C f32  cursor Y
  +0x90  ptr  → CS::CSScaleformValue / SceneObjProxy  ("Body/PointCursor" gui node) ── stage-2 projector
  +0xF0  ptr  → CS::WorldMapArea                        (the map VIEW object)        ── stage-1 transform
         (cursor+0xF0 == menu+0x4fb; ctor FUN_1409cb9c0)

CS::WorldMapArea  (at cursor+0xF0)
  +0x340 f32  snapMinX   ┐ snap-target bounds (marker space)
  +0x344 f32  snapMinZ   │   used by tick clamp path FUN_1409cd790 when DAT_143d6cfc3==0 it
  +0x348 f32  snapMaxX   │   instead derives via FUN_140886770(+0x360,+0x370)
  +0x34c f32  snapMaxZ   ┘
  +0x350 f32  mapMinX  = 0.0      ┐ FULL-MAP clamp extent (marker space), STATIC per area.
  +0x354 f32  mapMinZ  = 0.0      │ ctor sets [0,0,10496,10496]  (0x46240000 = 10496.0f).
  +0x358 f32  mapMaxX  = 10496.0  │ NOT the live viewport — do not use for zoom.
  +0x35c f32  mapMaxZ  = 10496.0  ┘
  +0x374 i32  param_6   (area/layout sub-id, ctor arg)
  +0x378 vec2 PAN     (marker-space offset)   ← LIVE viewport (stage-1)   *** verify tracks pan ***
  +0x380 f32  ZOOM    (marker units per view unit) ← LIVE viewport        *** verify tracks zoom ***
  +0x5d4 f32  = 3.0 (0x40400000) default
  +0x6e  i32  areaNo  (ctor param_5)
```

`cursor` is embedded at `menu+0x2DB0` (`CURSOR_OFF_IN_MENU`). The cursor ctor
`FUN_1409bc5b0` inits +0xFC/+0x104/+0x10C and `+0x26 = 0x3dcccccd` (0.1f lerp); it does
**not** set +0x90/+0xF0 — those are wired by the menu setup `FUN_1409be5e0`
(`FUN_1409bc5b0(menu+0x5b6, "Body/PointCursor"-gui, menu+0x4fb)` → 3rd arg lands at
cursor+0x1e qword = +0xF0).

---

## 2. The transform functions (decompiled)

### stage 1 — `FUN_1409cd0a0` (RVA 0x9cd0a0) — marker → view-local  **(the key affine)**
```c
out.x = (viewObj[+0x378].x + in.x) / viewObj[+0x380];   // (markerX + panX) / zoom
out.y = (viewObj[+0x378].y + in.y) / viewObj[+0x380];   // (markerZ + panZ) / zoom
```
Called from the cursor tick `FUN_1409bd4b0` as `FUN_1409cd0a0(*(cursor+0xF0), &out, &markerDelta)`.
This is the live pan/zoom. **pan=+0x378, zoom=+0x380.**

### stage 2 — `FUN_140d84990` (RVA 0xd84990) — the Scaleform fit (screen↔extent)
Letterbox-fit between a movieclip rect and the fixed canvas:
```c
canvas = [DAT_1447ef360+0x128];  W=canvas+0x118; H=canvas+0x11c;  origin=canvas+0x110/+0x114
rect   = movieclip.localBounds (via ctx+0x18→[+8]→vt[1]()→vt[10](&rect))  // left,top,right,bottom
aspect = (rect.r-rect.l)/(rect.b-rect.t)
if aspect <= W/H:  scale=(rect.b-rect.t)/H;  offX=(W-aspect*H)*0.5; offY=0
else:              scale=(rect.r-rect.l)/W;  offX=0; offY=(H-W/aspect)*0.5
out.x=(in.x-offX)*scale;  out.y=(in.y-offY)*scale
```
Wrapper `FUN_140d82770(ctx,&out,&in)` just copies the vec then calls this.

### forward Scaleform localToGlobal — `FUN_140d82070` / `FUN_140d7ff40` (RVAs 0xd82070 / 0xd7ff40)
Use `Scaleform::Render::Matrix2x4<float>` with `SetInverse` + a `×20.0` **twips→px**
factor. `FUN_140d7ff40` is a hit-test (maps a screen point into a movieclip's
normalized rect, then calls the clip's pick vmethod +0x200). `FUN_14117d180`
(RVA 0x117d180) = the GFx **cumulative-matrix getter** (`Prepend` up the display
tree; identity = `[1,0,0, 0,1,0]`).

### clamp helper — `FUN_140133f90(v,lo,hi)` = `clamp` (RVA 0x133f90). Confirmed.

### pan-bounds — `FUN_1409ce190` (RVA 0x9ce190)
Computes the clamped pan range from the full-map rect (+0x350.. or +0x340.. depending
on `DAT_143d6cfc3`), the canvas `1920.0`, and a zoom factor `FUN_1409e67e0()`. Confirms
the engine keeps pan within the map as you zoom. (`100.0/zoom` margin term.)

### edge-scroll pan — `FUN_1409bdc50` (RVA 0x9bdc50)
Confirms the **virtual 1920×1080 canvas** and that the map widget rect is
`[left, top, 1920-rightMargin, 1080-bottomMargin]`, margins = u16 at the layout object's
`+0xE8/+0xEA/+0xEC/+0xEE` (object from `FUN_140d2e710`, singleton `DAT_143d81ee8`).

---

## 3. Current page / area (deliverable #3)

- **`menu+0x151` (i32)** = current page/layout id, computed by `FUN_1409c4900(menu_data+0x24, DAT_143d6cfc0)`.
  For page id 1 it special-cases via `_DefaultMapNo` + the layout object (`FUN_140d2e710`).
- **`DAT_143d6cfc3` (u8, RVA 0x3d6cfc3)** = sub-layer / underground flag. Written by
  `FUN_1408855b0`; read by the clamp `FUN_1409cd790` and the pan-bounds `FUN_1409ce190`
  to switch which rect (snap +0x340 vs full +0x350) and which derived rect source.
- **`DAT_143d6cfc0` (u8, RVA 0x3d6cfc0)** = the map-fragment/overlay reveal toggle (the
  old "Show All Graces" byte — terrain pieces, NOT points; see ghidra-worldmap-re memory).
- **`WorldMapArea+0x6e` (i32)** = the area this view object renders (ctor param_5);
  `+0x374` = its layout sub-id. Underground + each DLC is a separate page → the transform
  is **per-page**; draw only markers whose `areaNo` belongs to the open page. The
  area→page mapping is the same one `tools/extract_markers.py` already encodes.

---

## 4. AOBs (entry prologues; one resolved RVA each, reference build)

| RVA | fn | role | AOB (entry) |
|---|---|---|---|
| 0x9cd0a0 | FUN_1409cd0a0 | **marker→view-local (pan/zoom)** | (short fn — prefer resolving via the tick xref) |
| 0xd84990 | FUN_140d84990 | Scaleform extent-fit | resolve via FUN_140d82770 call site |
| 0xd82770 | FUN_140d82770 | project wrapper | — |
| 0xd82070 | FUN_140d82070 | fwd localToGlobal (norm) | — |
| 0x117d180 | FUN_14117d180 | GFx cumulative-matrix getter | — |
| 0x9bd4b0 | FUN_1409bd4b0 | cursor tick vt[2] | `48 8b c4 55 53 56 57 41 54 41 56 41 57 48 8d 68 a1 48 81 ec c0 00 00 00` |
| 0x9bdc50 | FUN_1409bdc50 | edge-scroll pan | `40 53 55 56 57 41 56 48 81 ec 80 00 00 00 48 8b 05 fb 45 e4 03 33 f6 49 8b e8` |
| 0x9bc5b0 | FUN_1409bc5b0 | cursor ctor | `48 89 4c 24 08 57 48 83 ec 30 48 c7 44 24 20 fe ff ff ff 48 89 5c 24 48` |
| 0x9cb9c0 | FUN_1409cb9c0 | WorldMapArea ctor | (resolve via menu setup xref) |

The **robust handle** the mod already has is the cursor **vtable scan** (`0x142b29a90`,
in `goblin_worldmap_probe.cpp`). From a live cursor you reach everything else by the
offsets in §1 — no extra AOBs needed for the read path.

---

## 5. world_to_screen — two implementations

### Option A (RECOMMENDED) — analytic affine from pan/zoom (pure reads, any thread)
`view_local = (marker + pan)/zoom`, and `view_local` is (to within the fixed stage
placement) canvas pixels. So:
```
canvasX = (markerX + panX) / zoom
canvasZ = (markerZ + panZ) / zoom
pxX = canvasX * (realW / 1920) + biasX
pxY = canvasZ * (realH / 1080) + biasY
```
`pan`/`zoom` read live from `WorldMapArea` (cursor+0xF0) +0x378/+0x380. The fixed
`bias`/scale residual (stage-2 movieclip placement) is calibrated ONCE at runtime from
known points (the cursor itself is a perfect calibrator: its marker coord +0xFC/+0x104
and the canvas pixel where the game draws the reticle). See §6. SEH-guarded; read-only.

```cpp
// goblin::worldmap: AOB-resolve nothing new — reuse the cursor vtable scan handle.
struct WmView { float panX, panZ, zoom; bool ok; };
static WmView read_view(uintptr_t cursor) {           // cursor = vtable-scanned instance
    WmView v{}; uint64_t viewObj = 0;
    if (!seh_read8((void*)(cursor + 0xF0), &viewObj) || !viewObj) return v;
    if (!seh_read4((void*)(viewObj + 0x378), &v.panX)) return v;
    if (!seh_read4((void*)(viewObj + 0x37C), &v.panZ)) return v;   // +0x378 vec2 => x@+378, z@+37C
    if (!seh_read4((void*)(viewObj + 0x380), &v.zoom) || v.zoom == 0.f) return v;
    v.ok = true; return v;
}
// pixel (backbuffer) for a marker, given calibration {scaleX,scaleY,biasX,biasY}
ImVec2 world_to_screen(const WmView& v, float markerX, float markerZ,
                       float realW, float realH, const Calib& c) {
    float canvasX = (markerX + v.panX) / v.zoom;
    float canvasZ = (markerZ + v.panZ) / v.zoom;
    return { canvasX * (realW/1920.f) * c.scaleX + c.biasX,
             canvasZ * (realH/1080.f) * c.scaleY + c.biasY };
}
```

### Option B (fallback / ground-truth) — call the engine's own projection
Resolve `FUN_140d82770` + the `cursor+0x90` Scaleform value; per marker, feed its coord
through `FUN_14117d180`(get matrix)/`FUN_140d82070`. Guaranteed pixel-exact but must run
on the **render/GUI thread (the Present hook)** and is fragile across patches. Use it
only to *validate* Option A's calibration, or if A proves insufficient.

**Draw site:** the overlay's `hk_present` (`src/goblin_overlay.cpp`) — ImGui DisplaySize
== real backbuffer; use a foreground draw list to plot markers; cull by open page (§3).

---

## 6. Runtime confirmation (deliverable #4) — the make-or-break

A read-only **transform-scan** has been added to the cursor probe
(`src/goblin_worldmap_probe.cpp`, opt-in `debug_worldmap_probe`, see §7). With the world
map open it logs, for the live cursor, on every change:
- marker coord `+0xFC/+0x104`,
- view object (`+0xF0`) fields `+0x340..+0x35c` (rects), **`+0x378` (pan), `+0x380` (zoom)**,
- the canvas `[DAT_1447ef360+0x128]` W/H/origin.

**The test:** open the map, then **(a) PAN** — expect `+0x378` to sweep, `+0x350..` to stay
fixed `[0,0,10496,10496]`; **(b) ZOOM** — expect `+0x380` to change. If pan/zoom track,
Option A is locked. Then stand on a known grace from `data/grace_position_index.json`
(marker = `grid·256 + pos`), compute `world_to_screen`, and confirm the predicted pixel
lands on the game's own grace icon, through pan AND zoom.

Calibration of the residual `{scale,bias}`: with pan/zoom known, the cursor gives a free
(marker, canvasPx) pair every frame — read the reticle's canvas pixel via Option B's
matrix getter once, or fit `{scale,bias}` from two graces whose icons you can see.

---

## 7. Caveats

- **1920×1080 is a fixed virtual canvas.** Convert to backbuffer with `realW/1920`,
  `realH/1080`. On ultrawide/non-16:9 the game letterboxes the *menu*; whether the map
  fills or pillarboxes must be checked live (the stage-2 fit in `FUN_140d84990` does
  aspect-preserving letterbox → expect pillarbox bias on ultrawide). The overlay knows
  the real size via the `ResizeBuffers` hook.
- **Per-page.** Underground + each DLC is a separate `WorldMapArea`/page; resolve the
  open page via `menu+0x151` / `DAT_143d6cfc3` and only project markers of that page.
- **Offsets are version-specific** (VMProtect drifts RVAs). The contract is: vtable-scan
  the cursor (already patch-robust), then the §1 offsets; pin functions by AOB.
- **+0x378 is a vec2** — x at +0x378, z at +0x37C. Confirm the x/z order live (mirror of
  the +0xFC/+0x104 X/Z confirmation).
- Static-only until the §6 test runs. If pan/zoom at +0x378/+0x380 do **not** track live
  (e.g. the engine keeps them only inside the GFx movie and these are scratch), fall back
  to Option B (engine projection) — the function chain there is fully mapped.
```
