# RE prompt — suppress the grace pin's DRAW only, keep the teleport-to-grace callback (+ map the global pin/callback registry)

## Regression to fix

Native discovered-grace pin suppression now WORKS (commit d8d2125): zeroing the
WarpPinData **state bitmask `pin+0x60`** makes the draw-gate vt[3] `FUN_14087afa0`
yield invisible (it recomputes the cached visible byte `pin+0xC` as
`vis &= (state>>layerbit)&1`). See `windows_grace_warppin_suppression_re_findings.md`.

**But `pin+0x60` is overloaded:** it also gates the **"teleport to Site of Grace"
interaction**. With suppression on, you can no longer fast-travel by clicking a
grace on the map — the warp callback is gone too. We need to hide the pin's ICON
while keeping it WARP-CLICKABLE.

## What we need answered

1. **Where is the warp/teleport handler wired, and what does it read?** Find the
   input/click path that, when you select a grace pin on the world map, triggers
   the fast-travel. Does it read `pin+0x60` (the same state bitmask), a separate
   per-pin "warp enabled" field, or look the pin up in a separate warp/teleport
   LIST keyed by id? (find-what-READS `pin+0x60` and the WarpData id during the
   map's cursor/confirm handling.)

2. **Is there a DRAW-ONLY suppression** that leaves the warp interaction intact?
   Candidates to identify by offset:
   - a per-pin "render/visible" flag distinct from the warp-enable bitmask,
   - an alpha / icon-id field the draw reads but the click path doesn't,
   - removing the pin from the DRAW list while keeping it in the warp/teleport list.
   Report the exact field + suppress value that hides the icon but keeps teleport.

3. **(User's preferred direction) Map the GLOBAL pin/callback registry.** Rather
   than per-grace field-poking, find the structure(s) that own ALL map pins and
   their callbacks (graces/warps, category pins, objectives, player markers):
   - the container(s) and where they're built (the WorldMapViewModel / dialog warp
     list `FUN_140885500` copies `+0x60` into a VM warp list — is THAT list what the
     renderer AND the teleport handler both walk?),
   - the per-entry layout: draw fields vs interaction/callback fields vs id,
   - so suppression can target the DRAW pass alone and interaction (teleport,
     hover, tooltip) keeps working — generally, not just for graces.
   This is the durable fix: one map of "what draws a pin" vs "what makes a pin
   clickable" lets us suppress/relayer any pin type cleanly.

## Deliverable

`windows_grace_warppin_teleport_re_findings.md`: the teleport handler + the field/
list it reads; ONE recipe to suppress the grace ICON while keeping fast-travel
(exact offsets); and the global pin/callback registry map (containers + per-entry
draw-vs-interaction fields) so the suppression generalises beyond graces.

## Handles / known state
- WarpPinData builder `FUN_14088b7b0` @ er+0x88b7b0 (hooked: `warp_pin_detour`, config `grace_suppress_native`).
- state bitmask `pin+0x60` (currently zeroed for suppression — ALSO kills teleport = the bug); cached visible byte `pin+0xC`; draw-gate vt[3] `FUN_14087afa0` (`vis &= (state>>layerbit)&1`).
- VM warp-list copy `FUN_140885500` (copies `+0x60`).
- source WarpData entry `*(warpData+0x8)`: state byte `+0x1E`, iconId `+0x08`.
- category-pin mgr `[er+0x3D6E9B0]+0x390` (NOT graces); sibling mgr `[er+0x3D6F558]` (player/objective).
- App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`; resolve live via AOB (ASLR).

## Interim
Until this lands, `grace_suppress_native` trades teleport for no-doubling. Keep it
OFF by default (hybrid: native draws+teleports discovered graces, overlay draws
undiscovered) so fast-travel always works.
