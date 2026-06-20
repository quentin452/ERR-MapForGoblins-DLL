# RE findings — device-independent world-map view centre (map-space → screen)

Answers `docs/windows_worldmap_viewcenter_re_prompt.md`. Static Ghidra RE
(`D:\ghidra_proj2\ER`, scripts `find_viewcentre.java` / `find_panwriter.java` /
`find_panset.java`, outputs `out_viewcentre.txt` / `out_panwriter.txt` / `out_panset.txt`).
App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. Resolve by AOB; RVAs are reference
for this build. Builds on `world_map_projection_re_findings.md` and
`marker_to_mapspace_re_findings.md`.

---

## 0. TL;DR — the answer is (B): the engine's projection, in plain readable floats

The map-space → screen step is **exact and device-independent**:

```
screen_local_x = markerX · zoom − panX        zoom = WorldMapArea +0x380
screen_local_z = markerZ · zoom − panZ        pan  = WorldMapArea +0x378 (panX) / +0x37C (panZ)
backbuffer_px  = screen_local · (realW/1920, realH/1080) + bias
```

- `marker` = the map-space coord (`mapX = worldX−7040`, `mapZ = −worldZ+16512`, already solved).
- `pan` (`+0x378/+0x37C`) is stored in **1920×1080 virtual-canvas pixels** and is the
  **stable, input-device-independent view state** — the cursor tick (`FUN_1409bd4b0`) NEVER
  writes it; only the pan/zoom setters do (so mouse vs gamepad reticle motion can't move it).
- `bias` is a **constant** screen offset (widget inset / letterbox), absorbed by the overlay's
  one-point calibration. No moving "centre" field is needed at all.

**Why the old code was reticle-coupled:** `cursor+0xFC/+0x100`, `+0x104/+0x108`,
`+0x10C/+0x110` are *all three* the input-driven reticle (clamped to different rects, §1) —
none is a view centre. **Why the brief's pan attempt failed:** it used
`centre = pan + (screen/2)/zoom`, so `screen = (marker − centre)·zoom = marker·zoom − pan·zoom`
— `pan` got multiplied by `zoom`. `pan` is already in screen-local px; subtract it **directly**.

If you still want an explicit "marker at screen centre" value (e.g. for a centre-relative
form), it is `viewCentre = (pan + snapRectCentre) / zoom` (§2), with the snap-rect centre =
`((+0x340++0x348)/2, (+0x344++0x34c)/2)`. But the direct form above needs neither.

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

The pan path now implements the direct form (default ON; `Y` toggles back to the reticle
centre for A/B):
```cpp
float sx = (mU * v.zoom - v.panX) * (realW / 1920.f);
float sz = (mV * v.zoom - v.panZ) * (realH / 1080.f);
return { sx*scaleX + biasX, sz*scaleY + biasY };
```

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

`pan` is the only field tracking the view under both devices, BUT the user reports `pan` is
**unusable: variant between game launches** → can't anchor the projection. **Decision (user): RE
the gamepad-driven cursor.** The menu-walk cursor is mouse-only → the gamepad must move a
DIFFERENT `WorldMapCursorControl` instance whose reticle is stable across instances.

**Shipped finder — `scan_all_cursor_instances` (goblin_worldmap_probe.cpp):** enumerates EVERY
`WorldMapCursorControl` instance in committed private RW memory (bounded VirtualQuery walk +
chunked RPM; skips the exe image and >256 MB regions — NOT the old O(GB) raw-deref scan that
crashed). Runs once per map-open. The loop logs `[ALLCURSOR-MOVE] @addr` for any instance whose
`+0xFC/+0x104` changes, tagging the menu-walk one `(MOUSE)`. **Protocol:** open the map, move ONLY
the stick → the address that logs and is NOT the menu-walk one is the **gamepad cursor**. Report it
so we can pin a deterministic resolve (offset from the dialog / CSMenuMan, or the field to read).
If NO instance moves under the stick, the gamepad reticle is a different class → trace the
stick→view path (`FUN_140757a10` consumer under gamepad).

## 6. Caveats

- `pan/zoom` are in the **virtual 1920×1080 canvas**; the `realW/1920` scale handles
  resolution. On non-16:9 the menu letterboxes → the constant offset is in `bias` (re-`C` per
  aspect if needed). Read the real size from the `ResizeBuffers` hook (already done).
- Per-page: underground / DLC are separate `WorldMapArea`s; keep gating markers to the open
  page (`viewArea` / page+layer, already wired).
- Offsets are version-specific (VMProtect drifts RVAs); the contract is vtable-scan the cursor,
  then `cursor+0xF0` → the §3 offsets. `screen = marker·zoom − pan` is the stable formula.
```
