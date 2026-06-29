---
name: overlay-gamepad-cursor-bugs
description: "RESOLVED 2026-06-20 (commit be1bd9c): the gamepad/no-mouse map-drift bug = projection centre was cursor-coupled + missing canvas scale. Now centre = (pan+snapMid)/zoom (= +0xFC view centre, CE-confirmed) + canvas factor realW/1920 → map stays fixed without touching the mouse"
metadata: 
  node_type: memory
  type: project
---

## ✅ RESOLVED 2026-06-20 (commit be1bd9c) — user-confirmed "map reste fixe même sans bouger la souris"
The "map drifts when the cursor/stick moves (OK when mouse still)" bug is FIXED. It was the
projection, two parts: (1) project_uv used the reticle `v.raw[0/1]` (+0xFC) as the view centre
= cursor-coupled → switched to `centerU=(pan+snapMid)/zoom` (g_pan_center=true, commit fd0e455);
(2) this session a Cheat-Engine measurement PROVED `+0xFC` IS the view centre (`reticleFC·zoom−pan`
= snapMid exactly, constant across 11× zoom) so the cursor-independent centre is correct, AND
that project_uv was missing the canvas scale `realW/1920` (engine renders in a fixed virtual
1920×1080 GFx canvas → scaled to backbuffer; without it markers over-scaled 1.5× at 1280×720).
With both, the map is stable under gamepad / mouse-still. See [[overlay-rendered-markers]] handoff
#8 for the canvas-factor detail. REMAINING (separate, NOT this bug): the motion "dash" = overlay
redraw rate vs the native GFx map-layer cadence (temporal, not projection). The OLD gamepad
sub-bugs below (blue-cursor-snap, P/M calibration) are moot now that projection is from
pan/zoom+canvas, not the snapped cursor.

## ⭐ ROOT CAUSE FOUND (2026-06-20, user) — it's ONE bug, not "gamepad" [now RESOLVED, see above]
The "gamepad breaks the map" bug AND the "blue cursor doesn't lock to icons" bug are the
SAME root cause: **marker placement is coupled to CURSOR/MOUSE MOVEMENT.** Key signal: if the
mouse is held STILL, the points place CORRECTLY; the moment the mouse (or gamepad stick)
MOVES, the projection breaks. So it's not gamepad-specific — gamepad just moves the cursor
differently. Suspect: `project_uv` (goblin_overlay.cpp ~line 378) uses `v.raw[0]/v.raw[1]`
(cursor +0xFC/+0x100) as the view centre — but that field is cursor/reticle-coupled (history
flagged +0xFC as a frozen snap-target ~4824, contradicting the "view centre" comment), so it
moves with the mouse and shifts every marker. PAN-CENTRE FIX TRIED + FAILED (2026-06-20): set `centre = pan + (screen/2)/zoom` (pan =
WorldMapArea+0x378/+0x37C) → **WRONG position for ALL markers** (pan→centre relation isn't
that simple, or pan isn't in the same space/sign). So neither `+0xFC` (reticle-coupled) nor
pan works → the true device-independent view centre needs **RE**. Wrote
**docs/windows_worldmap_viewcenter_re_prompt.md** (find the stable view-centre field that pans
under BOTH mouse+gamepad but isn't the reticle, OR the engine world→screen projection
FUN_14117d180/FUN_140d82070 localToGlobal). Overlay has a **`Y` hotkey** that A/B-toggles
centre = PAN vs RETICLE(+0xFC) for testing RE candidates; default = reticle (mouse-usable).
F1 "Marker proto" now shows reticle-ctr(+0xFC) + pan + which centre is active.

## Page status (2026-06-20, real DLL finally deployed — earlier deploys were sandboxed, see [[mapforgoblins-linux-build]])
- **Overworld (60)**: icons + coordinates MATCH (correct) when mouse still. Some graces miss
  data (pre-existing). ✅ via the agent's exact formula `mapX=wx-7040, mapZ=-wz+16512`.
- **DLC (61)**: correct ICON but mispositioned — expected (still on eyeball; converter for
  page 10/DLC not dumped yet).
- **Underground (12)**: GATE is correct (right graces shown) but the CALC is wrong
  (mispositioned). User hypothesis: underground needs the `*256` grid conversion handled
  differently ("remove the 256 conv for underground") — the old map code applies a different
  underground treatment that the exact-formula path is missing. (Area-12 keeps native local
  coords, not unified — feeding `wx-7040` to area-12-local coords is wrong.)
- Injection crash on map-open (esp. underground) = game icon-build FUN_141eb9ed0 on an
  out-of-range injected row, 100%-save-exposed; NOT the overlay. Workaround: F1 master OFF
  (parks all injected rows). See [[mapforgoblins-map-open-freeze]].

---

Found 2026-06-20 once the DLC map became testable (via the 100% save swap, see
[[err-save-file-format]]). The overlay marker prototype works in MOUSE mode but has two
GAMEPAD-mode bugs:

1. **Joystick pans "Map Prototype" wrongly.** Moving the gamepad joystick moves OUR overlay
   markers, instead of behaving like the mouse (where markers track the game's view via the
   live-view probe). The proto must use the SAME view-tracking behavior for gamepad pan as
   for mouse pan — currently the joystick input drives our proto differently.

2. **Blue cursor doesn't snap/lock to icons in gamepad mode.** In mouse mode our blue
   reticle locks onto minimap icons exactly (matches the ER cursor). With gamepad there's an
   OFFSET between the ER cursor and our blue cursor — it doesn't lock. Likely the snapped-
   cursor read (cursor +0x104/+0x108) used for projection behaves differently under gamepad
   (different snap path / the menu cursor we resolve isn't the gamepad-driven one), so the
   render coord we read ≠ the visible cursor position.

**Why it matters:** the affine M/T calibration (P/M solver) relies on the snapped cursor =
exact icon render pos. If gamepad snap differs, calibration via gamepad would be wrong —
must calibrate with MOUSE, or fix the gamepad cursor read. Both bugs point at the cursor/
view probe (get_live_view, cursor resolve) not the affine math. See [[overlay-rendered-markers]].

**NOT caused by the DLC-reveal DLL changes** (flag writer + R key + reveal_dlc_map) added
this session — those are inert testing scaffolding; reverting them would not affect these
gamepad bugs, which live in the pre-existing overlay cursor/affine code.

## ROOT narrowed (2026-06-20): the resolved cursor tracks MOUSE, not GAMEPAD
Marker positions update only on MOUSE movement, not gamepad-stick movement. Per-frame cursor
resolve (added to get_live_view — menu-walk is O(1) cached) did NOT help → it's not staleness,
it's the FIELD/OBJECT: the menu-walk canonical cursor (WorldMapDialog+0x2DB0, reticle
+0x104/+0x108) tracks the mouse reticle but not the gamepad one. The gamepad drives a different
cursor mirror or a different offset. NEEDS RE — appended to
docs/windows_worldmap_viewcenter_re_prompt.md ("the GAMEPAD reticle field"): delta-scan the
cursor + WorldMapArea while moving the stick to find the f32 pair that changes under BOTH
devices; project_uv must read THAT. Quick check: F1 "U(+0x104)" — does it change on gamepad?
