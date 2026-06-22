# RE brief — world-map PAGE-TRANSITION animation state (sync the overlay, kill the flicker)

**Goal:** find the engine's world-map page-transition state — whether a swap between pages
(overworld ⇄ base-underground ⇄ DLC) is in progress, its 0→1 progress/alpha, and the
FROM/TO page — so our ImGui overlay can ride the transition exactly (fade/slide our markers
with the native map) instead of popping the new page's markers over the still-animating old
map. Same read-live philosophy as the just-landed world→map projection.

App 2.6.2.0 / ERR 2.2.9.6, DLL in-process. Resolve as `[er_base+RVA]+offsets` / AOBs,
runtime-verified. Read-only.

## The problem (what we see today)
On a page change, the page id flips at ONCE but the engine ANIMATES the swap (~0.4–0.5s).
Our overlay derives the open page from two dialog fields and switches markers the instant they
flip → the new page's icons flash over the out-going map. We tried two stopgaps (blank during
the swap; lag the swap by a delta-time window) — both visible/wrong because we're GUESSING the
transition timing from pan/zoom deltas, which fails for a cross-fade (the view doesn't move).
We need the engine's REAL transition signal.

## What we already resolve (from the SOLVED projection RE)
- **WorldMapDialog** = `cursor − 0x2DB0`; cursor resolved via CSMenuMan walk
  (`resolve_cursor_via_menu`, goblin_worldmap_probe.cpp). CSMenuMan slot = `[er+CSMENUMAN_SLOT_RVA]`.
- Open-page fields we read each frame (probe `get_live_view` → `LiveView`):
  - `openDlc` ← `dialog + 0xA88` (page id; 1 ⇒ DLC map / page 10)
  - `underground` ← `dialog + 0x2B68` deref `+ 0xB8` (layer byte: 0 surface, 1 underground)
  - `viewArea` ← `WorldMapArea + 0x6E` (areaNo of the open page); WorldMapArea = `cursor + 0xF0`
  - pan/zoom ← WorldMapArea `+0x378 / +0x37C / +0x380`
- `CS::WorldMapViewModel` (vtable RVA `0x2ad82e0`) holds the live projection converters.

## What we NEED (decompile + runtime-confirm)
1. **An "in transition" signal.** A flag/timer in the WorldMapDialog (or the menu/anim system)
   that is set while a page swap animates and clears when it settles. RVA/offset + how it reads.
2. **A 0→1 PROGRESS or alpha.** The animation interpolant (e.g. a float that ramps 0→1 over the
   ~0.45s, or per-layer alpha for the cross-fade). Lets us cross-fade our marker layers in step.
3. **FROM-page and TO-page during the transition.** While animating, which page is fading OUT
   and which IN (the two `openDlc`/`underground`/`viewArea` snapshots, or a from/to pair the
   dialog keeps). So we can draw the old set fading out + the new set fading in.
4. **Transition KIND.** Is it a cross-fade (layers' alpha, view static) or a pan/zoom move
   (view animates)? If it animates the view, do `dialog`/WorldMapArea expose the from/to view
   params (so our project_screen rides the same easing)?
5. The exact moment `openDlc`/`underground`/`viewArea` UPDATE relative to the animation — at the
   START (so they lead the visual) or END. This explains our flip-too-early symptom.

## Leads
- The page-switch is driven from the WorldMapDialog input handler (D-pad/tab to change layer/map).
  Find the function that writes `dialog+0xA88` / the layer byte and look for an adjacent timer/
  alpha it kicks off.
- ER menus use a Scaleform/anim layer; a cross-fade is usually a GFx alpha tween on the two map
  movie layers. The layer objects likely hang off the dialog (near `+0x2B68`).
- `WorldMapArea` (cursor+0xF0) eases pan/zoom (the 0.1 lerp we already see) — if the page swap
  also moves the view, the target vs current here is the from/to.

## Deliverable
- The transition flag + progress/alpha field(s): RVA/AOB + offset chain, runtime-confirmed by
  logging them across a live overworld→underground→DLC swap (values 0→1 over the animation).
- The from/to page during the swap.
- A tiny C++ read snippet (offsets off the dialog we already have) yielding `{transitioning,
  progress, fromGrp, toGrp}` each frame.

## Plan once answered
Drive the overlay from `{transitioning, progress, fromGrp, toGrp}`: while transitioning, draw the
fromGrp markers at alpha `1−progress` and the toGrp at `progress` (cross-fade) — or, if it's a
view move, project both with the eased view — so our icons track the native transition pixel-for-
pixel. Falls back to the current instant-swap when the signal is unavailable. Kills the flicker
without a guessed timer.

## Why
The projection/page work made marker POSITIONS exact; this makes the page SWAP exact too. A real
engine signal beats any delta-time heuristic (which can't tell a fade from a pan), and it's the
last visible rough edge in the overlay map.
