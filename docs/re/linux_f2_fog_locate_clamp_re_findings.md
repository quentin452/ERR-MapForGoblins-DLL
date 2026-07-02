# F2 fog locate-clamp — live RE findings (Linux/Proton, 2026-07-02)

Status: **fix attempts REVERTED** (user call — the intermediate behaviors were worse: forced
zoom applied to every locate, and the 90-frame reticle fight produced visible flicker over fog).
This doc preserves what was learned live so the next attempt starts from the real constraints,
not from scratch. Session driven entirely via the debug RPC + wmprobe stall dumps.

## The actual clamp chain (what stops a fog locate)

- The locate drive writes the cursor RETICLE pairs pre-step (`hk_c32f0`, cursor+0xFC/+0x104/
  +0x10C); the engine's per-frame step easer pans toward the reticle. That part works.
- **Inside the step, the engine CLAMPS the reticle to the discovered-extent bounds** before the
  easer reads it. Measured live (Morgott, Leyndell deep fog, target (4498.9, 3736.6)): read-back
  after the original step = **(4271.5, 5922.0)** — V bound 5922 was IDENTICAL at zoom 0.5 and
  2.25 (zoom-independent, so not a rect±half-viewport derivation), X bound differed by zoom.
  The bounds VALUES are not stored in view+0x2C0..0x400 or cursor+0xE0..0x1C0 (full float dumps
  taken at stall) — they're computed inside the clamp code. **Next real step = RE the reticle
  clamp function** (who writes cursor+0xFC inside FUN_1409c32f0's call tree; find its bounds
  source — likely a discovered-map-fragment bbox). Ghidra work.
- So the engine pans to the closest ALLOWED point and stops — the original F2 symptom.

## Dead ends (all tried live, all reverted — do NOT retry)

1. **Widening the snap rect (view+0x340..0x34C).** It is NOT the pan clamp bound and is NOT
   recomputed per frame. Its MIDPOINT feeds the pan semantics (`pan = centre·zoom − snapMid`,
   both engine + our projection) → writing it teleports the view off-map on the next locate and
   the corruption persists. (Live: every search-result click "lost the whole map".)
2. **Direct pan writes (view+0x378/+0x37C) post-step.** Two failure layers: (a) a lerp
   restarted from the live pan stalls forever at `clamped + lerp·(target−clamped)` because the
   engine re-snaps the pan to the clamped-reticle target every frame (measured: equilibrium at
   exactly 18% with kPanLerp=0.18); (b) even easing from our own state, the engine COMPOSITE
   reads the pan the easer wrote inside the step, not our post-step value — while OUR overlay
   (present thread) may read ours → canvas and markers diverge. Probe evidence: wmprobe
   INPUT-DELTA showed pan/zoom/reticle flip-flopping between our values and the engine's every
   ~frame.
3. **Zoom-in via live zoom write only (view+0x380)** flip-flops at max zoom-out: the engine
   eases live zoom toward its own target field — **cursor+0x180 = the zoom easer target**
   (identified by value in the stall dump; writing it makes the engine itself do the zoom-in
   and unpins the pan). This WORKS mechanically but was reverted with the rest: forcing
   kLocateZoom on every locate changes the user's zoom uninvited, and on clamped targets the
   whole hold turns into a visible 90-frame flicker fight.

## Struct-map gains (keep — independent of the revert)

- `cursor+0x180` = zoom easer TARGET (f32). `cursor+0x168/+0x174/+0x178/+0x17C` nearby: 0.03 /
  2.97-or-55.7 / 0.40 / 2.21 — plausibly zoom min/max/step/prev; unconfirmed.
- `cursor+0x134/+0x138` = another world-space coord pair (value ≈ pre-clamp cursor pos).
- `view+0x330..0x33C` = a live rect-ish float quad that changes with view state; semantics
  unidentified (does NOT match pan bounds under any [minX,minZ,maxX,maxZ] reading tried).
- `view+0x340..0x34C` ("snap rect") ≈ 1920×1080-sized box in marker units, NOT per-frame
  recomputed, midpoint = the pan-space origin offset. Treat as READ-ONLY forever.

## What a future fix must deal with

- Defeating the clamp = RE the reticle-clamp bounds source (Ghidra, `FUN_1409c32f0` subtree).
  Widening those bounds during the locate hold is the only path that lets the ENGINE do the
  pan itself (composite + overlay stay coherent by construction).
- Even if the pan reaches deep fog: the canvas there is undiscovered (pale void) — decide the
  UX first (ring on void? fragment-less canvas?). The user explicitly rejected: forced zoom
  changes, any per-frame write fight (flicker), and behavior changes on non-fog locates
  (godrick must behave EXACTLY as before).
- Two cheap UX ideas prototyped then reverted with the rest (re-considerable in isolation,
  behind their own toggle, only if wanted): search-hit markers bypassing the per-marker
  visibility gates (`map_renderer.cpp` — one-line `!is_hit &&` on the `marker_passes_gates`
  cull), and a worldmap edge direction-indicator for off-screen search hits (clamped to
  canvas∩screen; both rendered fine in-game).

## Repro recipe (unchanged, still deterministic)

Map open + F1 → search `morgott` (AZERTY: RPC `type` translation of 'm' is flaky across
boots — typing the substring `orgott` sidesteps it entirely) → click "Morgott, the Omen King
(x1) - Surface" → pan clamps at the revealed-area edge. `godrick the` → "Godrick the Grafted"
= negative control (discovered, centres fine).
