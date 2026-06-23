# RE findings — world-map page-transition animation state (sync the overlay)

Answers `docs/re/windows_worldmap_page_transition_re_prompt.md`. Static Ghidra RE
(`D:\ghidra_proj2\ER`, scripts `re_v71..v73`, output `out_v71.txt`..`out_v73.txt`).
App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. Read-only. Builds on the OW/UG layer +
projection RE (`dialog = cursor−0x2DB0`; layer byte `[dialog+0x2B68]+0xB8`; view = `cursor+0xF0`,
pan `+0x378`, zoom `+0x380`).

---

## 0. TL;DR — there is NO clean `{transitioning, progress, from, to}` struct; here's what is real

The brief hoped for a stored transition flag + a 0→1 progress + from/to page on the dialog. The
engine doesn't keep that. What actually exists:

1. **The page id flips INSTANTLY at input** (root of "flip too early"). The switch handlers
   (`FUN_1409c40f0` layer, `FUN_1409c5d20`/`FUN_1409c7900`/`FUN_1409c1fc0`/… map) write
   `dialog+0xA88` and the 9 list page fields (`FUN_1409c8120`) the moment you press the button —
   no easing on the *id*. The **visual** then animates to catch up.
2. **The "page changed this frame" flag `dialog+0xA44` is per-frame TRANSIENT, not a 0.45s level.**
   The handlers set `dialog+0xA44=1`; the per-frame step `FUN_1409c32f0` reads it, propagates it to
   the 9 lists, then **clears `+0xA44/+0xA45/+0xA46` at the end of the same step** (`MOV word
   [+0xA44],0` @ `0x9c37e8`/end). So `+0xA44` is an **edge** ("a swap was requested since last
   step"), usable to *detect* a swap — but it is NOT the multi-frame "still animating" signal.
3. **The real multi-frame animation is the VIEW pan/zoom ease + two countdown timers** (below).
4. **The surface↔underground cross-fade alpha is Scaleform-side** (per-list `vt[0x38]` fed the
   `+0xA44` byte; layer pick by the *sign* of `dialog+0xE00`) — no flat 0→1 alpha float to read.

So: detect the swap (edge), then drive the overlay off the view ease / timers — don't expect a
single engine progress float. Details + the exact read recipe in §3/§5.

## 1. The switch handlers — what fires on a page/layer change

`FUN_1409c40f0(dialog, layer, force)` (surface↔UG), and the map-switch siblings
`FUN_1409c5d20` / `FUN_1409c7900` / `FUN_1409c1fc0` / `FUN_1409c23d0` / `FUN_1409c2c00` /
`FUN_1409c3280`, all do the same shape:
```c
[dialog+0x2B68]->[+0xB8] = layer;                 // layer byte (instant)
FUN_1409c8120(dialog, page (+1 if underground));  // write page to dialog+0xA88-family + 9 lists
FUN_140814ed0(&{400,5,1,&DAT_142adaaf0});          // page-flip SOUND (deferred SE id 400) — NOT visual
FUN_1409bc9d0(dialog+0x2DB0);                       // reset cursor snap (+0x114/+0x118/+0x128/+0x140)
dialog+0xA44 = 1;                                   // "swap requested" edge
```
- `FUN_1409c8120` writes the new page to **9 list objects** (`dialog+0x30DC, +0x31D4, +0x32CC,
  +0x33C4, +0x34BC, +0x35B4, +0x36AC, +0x37A4, +0x389C`, stride ~0xF8) — the 9
  `WorldMapItemControl` lists — plus a per-list page-keyed value via `FUN_1408867d0`
  (`+0x39C`=page0, `+0x3A4`=page10 → base/DLC select). **All instant; no per-list easing of the id.**
- `FUN_140814ed0` is only the **sound** (a deferred SE request to `_DAT_143b355e0`); it is not the
  cross-fade. Don't mistake the `400` for a duration.

## 2. The per-frame step `FUN_1409c32f0(dialog, dt, …)` — the real animation

This is the WorldMapDialog view/transition step (called each frame; `dt` = `param_2`). Relevant
state (all offsets are raw byte offsets off `dialog`):

- **`dialog+0xE00` (f32)** — a countdown reset to **0.2** when the view is *not* animating
  (`FUN_1409cd020(view)==0`) and **decremented by `dt`** while it is. Its **sign also selects the
  layer** in `FUN_1409c38d0` (`0 ≤ +0xE00` → surface side; `+0xE00 < 0` → underground) → it doubles
  as the **surface↔UG cross-fade interpolant/timer** (~0.2 s).
- **`dialog+0x3E74` (f32)** — another countdown: `if (>0) -= dt; else FUN_1409cd840(view)` (settle).
  A second dt-decremented transition timer.
- **View pan/zoom ease toward a TARGET:** the step eases the view toward **`dialog+0x2EAC` (pan
  vec2 target)** / **`dialog+0x2EB4` (zoom target)** via `FUN_1409cd1c0(view,…,+0x2EAC,+0x2EB4)`.
  The *current* pan/zoom live on the view (`cursor+0xF0` → `+0x378` pan / `+0x380` zoom). So a
  page swap that moves the view (notably **base↔DLC**, different canvas region) **animates pan/zoom
  from current → target** over several frames — fully C++-readable.
- **`dialog+0xA45` (u8)** — recomputed every step as "**not settled**": set `1` while pan
  (`+0x2EBC` cur vs target) or zoom differ from target **or `+0xA44`set**, `0` once at target. (Then
  cleared with `+0xA44` at end of step — so it reflects *this step's* settle test.)
- **`FUN_1409cd020(view)` (RVA `0x9cd020`)** — returns whether the view pan/zoom anim is active.
  This is the cleanest engine "view is moving" boolean (call it, or replicate: cur vs target on the
  WorldMapArea).

`FUN_1409c38d0` (called from the step) propagates `dialog+0xA44` to each of the 9 lists
(`list->vt[0x38](+0xA44)`) and uses `+0xE00`'s sign to toggle the surface/UG layer objects — the
cross-fade is executed **inside the list/Scaleform layer**, not as a readable float.

## 3. Answering the brief's five questions

1. **"In transition" signal** — no persistent bool; **derive** it as
   `transitioning = FUN_1409cd020(view) != 0  ||  dialog+0x3E74 > 0  ||  dialog+0xE00`-still-running`,
   or simplest: **`view pan/zoom != target`** (`cursor+0x378/+0x380` vs `dialog+0x2EAC/+0x2EB4`,
   eps). `dialog+0xA44` is only the **swap edge** (1 the frame a swap is requested) — use it to
   *trigger*, not to *sustain*.
2. **0→1 progress / alpha** — not stored as one float. Two usable proxies: **(a) view ease** —
   `progress = 1 − dist(cur,target)/dist(start,target)` (overlay snapshots `start` at the edge);
   **(b) the countdown** `dialog+0xE00` (~0.2 s) or `dialog+0x3E74` normalized by their reset value.
   The literal per-layer cross-fade alpha is Scaleform-internal (unreadable as a flat field).
3. **FROM / TO page** — **TO** = current `dialog+0xA88` + the 9 lists (set instantly at the edge).
   **FROM is not kept as an int** — it's the *visual* (the easing-out view + the still-shown old
   layer). The overlay must remember its own previous page as FROM.
4. **KIND** — two superimposed: **(a) view pan/zoom move** (C++-readable, dominant for base↔DLC) and
   **(b) surface↔UG cross-fade** (Scaleform layer alpha, signalled by `dialog+0xE00` sign + the
   per-list `vt[0x38]` flag; the view may barely move). Distinguish live: if
   `dist(cur,target) > eps` it's a move; else it's a fade (read `+0xE00`).
5. **When do the page fields update** — at the **START** (instantly, in the switch handler), before
   the visual animates → exactly the "flip too early" symptom. The overlay should **not** switch its
   marker set on the `dialog+0xA88` flip; it should hold the FROM set and cross-fade over §2's
   ease/timer.

## 4. Why the markers already half-work, and the precise remaining bug

The overlay projects markers from the **live** view (`cursor+0x378/+0x380`) every frame, so during a
view-move swap the marker **positions already ride the ease** (they pan/zoom with the map). The
visible defect is purely **which page's set** is drawn: the overlay swaps sets the instant the page
id flips, while the engine is still fading the old page's tiles/icons in/out. So this is a
**set-membership + alpha** problem, not a position problem — which is why the delta-time stopgaps
(blank / lag) looked wrong.

## 5. Overlay recipe (deliverable — offsets off the dialog we already resolve)

```cpp
// dialog = cursor - 0x2DB0 ; view = *(void**)(cursor+0xF0)
struct WmTransition { bool active; float progress; int fromGrp, toGrp; bool isMove; };

// per frame:
int curPage = read_open_group(dialog);                 // existing: 0xA88 + layer [0x2B68]+0xB8
float panX=view+0x378, panZ=view+0x37C, zoom=view+0x380;
float tgtPanX=dialog+0x2EAC, tgtPanZ=dialog+0x2EB0, tgtZoom=dialog+0x2EB4;   // ease targets
bool edge = read_u8(dialog+0xA44) != 0;                // swap-requested this frame (transient)

// detect a swap: page id changed since our last frame, OR the edge fired
if (curPage != s_prevPage || edge) { s_fromGrp=s_prevPage; s_toGrp=curPage;
    s_startDist = dist({panX,panZ,zoom},{tgtPanX,tgtPanZ,tgtZoom}); s_timer = read_f32(dialog+0xE00); }

float d = dist({panX,panZ,zoom},{tgtPanX,tgtPanZ,tgtZoom});
bool moving = d > EPS;                                   // view still easing
bool fading = read_f32(dialog+0x3E74) > 0.f || /*+0xE00 active*/;   // timer still running
out.active = moving || fading;
out.isMove = moving;
out.progress = moving && s_startDist>EPS ? clamp01(1.f - d/s_startDist)
                                         : clamp01(1.f - read_f32(dialog+0xE00)/0.2f);
out.fromGrp = s_fromGrp; out.toGrp = s_toGrp;
s_prevPage = curPage;
// draw: fromGrp markers at alpha (1-progress), toGrp at alpha (progress); both projected from the
// LIVE view (so they ride the pan/zoom). Falls back to instant swap when !active.
```
Use `FUN_1409cd020(view)` (RVA `0x9cd020`) instead of the `dist()` test if you prefer the engine's
own "view animating" verdict (one call, no eps tuning).

## 6. Handles / AOBs

- switch handlers: layer `FUN_1409c40f0` `0x9c40f0`; map `FUN_1409c5d20` `0x9c5d20`,
  `FUN_1409c7900` `0x9c7900`, `FUN_1409c1fc0` `0x9c1fc0`, `FUN_1409c23d0`, `FUN_1409c2c00`,
  `FUN_1409c3280`. page-apply `FUN_1409c8120` `0x9c8120` (9 lists). flip sound `FUN_140814ed0`.
- per-frame step `FUN_1409c32f0` `0x9c32f0` (eases pan/zoom → `dialog+0x2EAC/+0x2EB4`, runs timers
  `+0xE00`/`+0x3E74`, sets/clears `+0xA44/0xA45/0xA46`). layer/list driver `FUN_1409c38d0`
  `0x9c38d0`. view-animating predicate `FUN_1409cd020` `0x9cd020`.
- dialog fields: page `+0xA88`; layer `[+0x2B68]+0xB8`; **swap edge `+0xA44` (u8, transient)**;
  not-settled `+0xA45`; pan target `+0x2EAC` (vec2), zoom target `+0x2EB4`; cross-fade timer/sign
  `+0xE00` (f32, ~0.2 s); settle timer `+0x3E74` (f32). 9 list page fields `+0x30DC` … `+0x389C`
  (stride ~0xF8).
- view (`cursor+0xF0`): pan `+0x378/+0x37C`, zoom `+0x380`.
- Resolve fns by AOB, the dialog via the existing CSMenuMan cursor walk; offsets version-specific.

## 7b. RUNTIME CONFIRMED (2026-06-22, Linux/Proton, [WMTRANS] probe in get_live_view)

Logged overworld→underground→DLC→back. Results:
- **`dialog+0xA44` swap edge = ✓** pulses `1` for ~1 frame at each press, else 0.
- **page id flips at the START = ✓** the edge frame already shows the new `openDlc`/`underground`
  (the "flip too early" cause, confirmed).
- **`dialog+0xE00` fadeTimer = ✓ THE usable progress signal**: resets to **0.200** at the swap,
  ramps down by dt to 0 over ~6–7 frames (~0.2 s), crosses zero, **settles at ~-0.023**. Its SIGN
  selects the layer (≥0 surface, <0 UG). → `progress = clamp01(1 − fadeTimer/0.2)`.
- **pan/zoom-target offsets (`+0x2EAC/+0x2EB0/+0x2EB4`) = ✗ WRONG**: read garbage (tgtPan≈5129,7000;
  tgtZoom≈5129 — absurd for a ~0.2 zoom); current pan SNAPS between fixed per-page values rather
  than easing. **Not needed** — §4: marker POSITIONS already ride the live view, so the fix is
  set-membership + alpha only. Dropped these reads.
- `+0x3E74` settle timer stayed 0 in these transitions — not load-bearing here.

**Fix recipe (revised):** detect swap by `open_grp` change (or `+0xA44`), then cross-fade the marker
SET via `fadeTimer` (`progress = 1 − fade/0.2`) — or a self-timed ~0.2 s `io.DeltaTime` ramp
(equivalent, no offset dependence). LiveView now exposes `swapEdge` + `fadeTimer`.

## 7. Runtime confirm (quentin — game not running during this RE)

Log per frame across **overworld→underground→DLC→back**:
`dialog+0xA44/0xA45`, `dialog+0xE00`, `dialog+0x3E74`, `view +0x378/+0x380` and `dialog+0x2EAC/
+0x2EB4`, plus `dialog+0xA88`/layer. Confirm: (1) `+0xA44` pulses 1 for ~1 frame at the press;
(2) on **base↔DLC** the pan/zoom sweeps cur→target over ~0.4 s (`isMove`); (3) on **surface↔UG**
the view barely moves but `+0xE00` ramps and its sign flips (the cross-fade); (4) `dialog+0xA88`
flips at the *start*. Then calibrate the cross-fade duration (the brief's ~0.45 s vs the `0.2`
reset — `+0xE00` may be one phase; `+0x3E74` the other) and pick which timer normalizes `progress`.
```
