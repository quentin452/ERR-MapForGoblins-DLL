# RE findings — device-independent world-map view centre (map-space → screen)

Answers `docs/windows_worldmap_viewcenter_re_prompt.md`. Static Ghidra RE
(`D:\ghidra_proj2\ER`, scripts `find_viewcentre.java` / `find_panwriter.java` /
`find_panset.java`, outputs `out_viewcentre.txt` / `out_panwriter.txt` / `out_panset.txt`).
App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. Resolve by AOB; RVAs are reference
for this build. Builds on `world_map_projection_re_findings.md` and
`marker_to_mapspace_re_findings.md`.

---

## 0. TL;DR — device-independent view centre = `(pan + snapMid) / zoom`

The marker that sits at the screen centre — **stable across input device AND game launch**:

```
viewCentreX = (panX + snapMidX) / zoom        pan    = WorldMapArea +0x378 / +0x37C
viewCentreZ = (panZ + snapMidZ) / zoom        zoom   = WorldMapArea +0x380
snapMidX = (view+0x340 + view+0x348) / 2      snapMid = midpoint of the snap rect +0x340..+0x34c
snapMidZ = (view+0x344 + view+0x34c) / 2
screen   = (marker − viewCentre) · zoom + screenCentre        (+ scale/bias residual)
```

- Derived from the engine pan setter `FUN_1409cd100`: `pan = zoom·viewCentre − snapMid` (§2),
  inverted. **Under mouse it EQUALS the reticle** (drop-in for the known-good baseline); **under
  gamepad the reticle freezes but `pan` is still updated** (confirmed by `[INPUT-DELTA]`, §5b),
  so the centre tracks the stick.
- **`snapMid` is the term that was missing.** Bare `pan` looked "instance-variant" because the
  centre was off by `snapMid/zoom`, and `snapMid` is **per-page** — it changes with the open
  page/view, so a baked pan offset never transferred. Add it and pan becomes stable.
- All three terms live on the ONE deterministically-resolved `WorldMapArea` (menu-walk →
  dialog → `cursor+0xF0`) → **no separate "gamepad cursor" is needed** (the all-instance scan in
  §5b is now an opt-in fallback only).

**Why the cursor fields all failed:** `cursor+0xFC/+0x100`, `+0x104/+0x108`, `+0x10C/+0x110` are
*all three* the input-driven reticle (clamped to different rects, §1), and the reticle freezes
under gamepad. **Why earlier pan attempts failed:** `centre = pan + (screen/2)/zoom` multiplied
pan by zoom; and bare `pan` omitted the per-page `snapMid`. The correct centre is `(pan+snapMid)/zoom`.

---

## 1. The cursor coords are all reticle (why every cursor-field centre failed)

`FUN_1409bd4b0` (cursor tick, vt[2], RVA `0x9bd4b0`) writes all three coord pairs from the
**input-moved reticle**:

```c
A = *(cursor+0xFC)                       // current reticle (X,Z)
A += inputDir · speed · dt · layoutScale / zoom   // FUN_1409bdc50 edge-scroll + stick, /zoom
B = (no hover-snap) ? A : <Scaleform snap delta via FUN_1409cd0a0>   // +0x104
...
*(cursor+0xFC)  = clamp_to_snap_rect(A)            via FUN_1409cd790
*(cursor+0x104) = clamp_to_full_rect(B)            clamp to +0x350..+0x35c then snap rect
*(cursor+0x10C) = C                                 // snap-lerp target / reticle
```

So `+0xFC`, `+0x104`, `+0x10C` differ only by which rect they're clamped to — **all track the
cursor**. Using any as the view centre makes markers follow the reticle (mouse *and* gamepad).
The tick reads `zoom` (`view+0x380`) but **never reads or writes `pan` (`+0x378`)** → pan is
decoupled from the reticle = the device-independent quantity we want.

## 2. The Rosetta stone — the pan SETTER `FUN_1409cd100` (RVA `0x9cd100`)

```c
// param_2 = desired view-centre point (marker space); 0.5 = DAT_14329e660
*(view+0x378) = {                                            // pan vec2
    view+0x380 * param_2[0] - (view+0x348 + view+0x340)*0.5,   // panX = zoom·centreX − snapRectCentreX
    view+0x380 * param_2[1] - (view+0x34c + view+0x344)*0.5,   // panZ = zoom·centreZ − snapRectCentreZ
};
```
`FUN_1409cd1c0` (RVA `0x9cd1c0`) is the combined zoom+pan setter — it writes `view+0x380`
(zoom, `MOVSS`) then the same `pan = zoom·centre − snapRectCentre` to `+0x378`. Callers:
menu setup `FUN_1409be5e0`, zoom/scroll handlers `FUN_1409c1fc0` / `FUN_1409c32f0`.

The inverse transform `FUN_1409cd0a0` (RVA `0x9cd0a0`) is used by the tick to turn a
**screen-space** Scaleform delta into a **marker-space** delta:
```c
out = (in + pan) / zoom        // maps SCREEN → MARKER  ⇒  marker = (screen + pan)/zoom
```
Invert ⇒ **forward `screen = marker·zoom − pan`**. Cross-check against the setter: feed the
view centre `param_2` through the forward map →
`centre·zoom − pan = centre·zoom − (zoom·centre − snapRectCentre) = snapRectCentre` ✓.
So the snap rect `+0x340..+0x34c` is the **viewport in screen-local px**, its centre is the
screen position of the view-centre marker, and `pan` lives in that same screen-local space.

This is why `screen = marker·zoom − pan` is exact and why `pan` must be subtracted directly.

## 3. Offsets / handles (on `CS::WorldMapArea`, reached via `cursor + 0xF0`)

| field | offset | meaning |
|---|---|---|
| pan (vec2) | `+0x378` (x) / `+0x37C` (z) | view pan, **screen-local (1920×1080-canvas) px** — device-independent |
| zoom (f32) | `+0x380` | canvas-px per marker-unit |
| snap rect | `+0x340/+0x344/+0x348/+0x34c` | viewport min/max in screen-local px (centre = screen centre of view) |
| full rect | `+0x350..+0x35c` | static `[0,0,10496,10496]` map clamp (marker space — NOT screen) |
| areaNo | `+0x6e` | open page area |

Pan/zoom setters: `FUN_1409cd100` (`0x9cd100`), `FUN_1409cd1c0` (`0x9cd1c0`). Forward/inverse
transform: `FUN_1409cd0a0` (`0x9cd0a0`). The mod already reaches all of these from the
vtable-scanned cursor → `cursor+0xF0` (no new AOBs needed); `LiveView` already exposes
`panX/panZ/zoom`.

## 4. Shipped — `project_uv` (src/goblin_overlay.cpp)

Default ON; `Y` toggles the raw reticle for A/B. Uses the device-independent centre in the
proven reticle-form (same screen-centre anchor + scale/bias as the mouse baseline):
```cpp
float centerU = (v.panX + v.snapMidX) / v.zoom;   // == reticle under mouse; tracks pan under gamepad
float centerV = (v.panZ + v.snapMidZ) / v.zoom;
return { (mU-centerU)*v.zoom*scaleX + realW*0.5f + biasX,
         (mV-centerV)*v.zoom*scaleY + realH*0.5f + biasY };
```
`snapMid = midpoint(view+0x340..+0x34c)`, exposed via `LiveView.snapMidX/snapMidZ`.

## 5. Live verification (quentin) — the make-or-break

1. **Self-test (should pass by construction):** with the map open + menu closed, the cyan ring
   (projection of the reticle's own `+0x104/+0x108`) must sit on the magenta mouse cross
   through **pan, zoom, and gamepad** — `err≈0`. Because `screen = marker·zoom − pan` and the
   reticle's screen pos *is* `reticle·zoom − pan`, this holds for any view/device. If it drifts
   only when zoomed, the residual is `scaleX/scaleY` (keep ~1.0).
2. **Press `C` once** (reticle on mouse) to pin the constant `bias` (widget inset / letterbox).
3. **Grace lock:** a known grace's map-space coord must land on its native icon and **stay**
   while panning with mouse AND gamepad, across zoom.
4. **Axis:** `U=+0x104` should pair with `panX=+0x378`. If the constellation is transposed,
   press `G` (axis swap) — confirm which way live and bake it.

## 5b. GAMEPAD reticle (open, 2026-06-20) — runtime test added

Runtime feedback: markers update on MOUSE motion but NOT on gamepad-stick motion; the
canonical (menu-walk) cursor's reticle `+0x104/+0x108` tracks the mouse but seemingly not the
stick. Two static facts bound the search:

- **The tick DOES read the stick.** `FUN_1409bd4b0` calls the analog reader `FUN_140757a10`
  (RVA `0x757a10`) at `0x1409bd66a` and adds it to the edge-scroll vector → moves reticle `+0xFC`.
  Both `FUN_140757a10`/`FUN_140757af0` gate on the input-enable check `FUN_140758050` (reads
  CSMenuMan `DAT_143d6b7b0` state) — so stick input *can* be suppressed by menu sub-state.
- **The per-frame view updater is `FUN_1409c32f0`** (RVA `0x9c32f0`, called from `FUN_1409cfb60`):
  it drives the pan setter `FUN_1409cd100`/`FUN_1409cd1c0` on `WorldMapArea` (`dialog+0x4fb`)
  toward the target at `dialog+0x2eac/+0x2eb4`. NOTE `0x2eac − 0x2DB0 = 0xFC`, so that target IS
  the cursor reticle `+0xFC/+0x104` — pan centres on the (mouse) reticle. With the stick the
  reticle is frozen, yet pan still moves → the gamepad pans via a path that does NOT go through
  this reticle (a separate cursor/controller).

**Runtime result (`input_delta_scan`, 2026-06-20, quentin):**
- **Gamepad (stick only):** `view+0x378` (panX), `+0x37C` (panZ), `+0x380` (zoom) change; the
  menu-walk cursor reticle `+0xFC/+0x104` stays **frozen**.
- **Mouse:** reticle `+0xFC/+0x104` changes **and** `view+0x378` (pan).

`pan` tracks the view under both devices. The user first reported bare `pan` as "variant between
game launches" — **root cause now understood: the missing per-page `snapMid` term** (§0). The
correct device-independent centre `(pan + snapMid)/zoom` is stable AND tracks the gamepad, all from
the deterministic `WorldMapArea` → **no separate gamepad cursor required.**

**Shipped (primary):** `LiveView.snapMidX/snapMidZ` (read from `view+0x340..+0x34c`) and
`project_uv` now uses `centre = (pan+snapMid)/zoom` in the reticle-form (default on; `Y` toggles
the raw reticle for A/B). Self-test: under mouse the centre == reticle (cyan ring stays on the
mouse); under gamepad it should now track the stick.

**Fallback (opt-in) — `scan_all_cursor_instances`:** if `(pan+snapMid)/zoom` still fails, set env
`MFG_GAMEPAD_CURSOR_SCAN` to enumerate EVERY `WorldMapCursorControl` instance (bounded VirtualQuery
+ chunked RPM; skips the exe image and >256 MB regions — NOT the old O(GB) raw-deref scan). It logs
`[ALLCURSOR-MOVE] @addr` for any instance whose `+0xFC/+0x104` changes, tagging the menu-walk one
`(MOUSE)`. Move ONLY the stick → an address that logs and is NOT the menu-walk one would be a
genuine separate gamepad cursor. Off by default (heavy).

## 6. Caveats

- `pan/zoom` are in the **virtual 1920×1080 canvas**; the `realW/1920` scale handles
  resolution. On non-16:9 the menu letterboxes → the constant offset is in `bias` (re-`C` per
  aspect if needed). Read the real size from the `ResizeBuffers` hook (already done).
- Per-page: underground / DLC are separate `WorldMapArea`s; keep gating markers to the open
  page (`viewArea` / page+layer, already wired).
- Offsets are version-specific (VMProtect drifts RVAs); the contract is vtable-scan the cursor,
  then `cursor+0xF0` → the §3 offsets. `screen = marker·zoom − pan` is the stable formula.
```
