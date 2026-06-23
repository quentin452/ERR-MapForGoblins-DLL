# Windows RE brief — mid-session resolution change corrupts the world map (no-restart fix)

**App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. MSVC `.text` (not VMP for the render
config path). Mostly read-only RE; the deliverable MAY end in a small write/call from our
`ResizeBuffers` hook.** Goal: find WHY changing the game resolution **mid-session** corrupts
the world-map view (zoom math goes wrong) **and** breaks ImGui/minimap **input**, then give a
way to force ER to re-derive its stale render/viewport state **without restarting the game**.

## The symptom (reproduced, user-confirmed)

Launch ER at one resolution, open the game, then change the resolution in the in-game
options **without restarting** (e.g. 1920×1080 → 1280×720). After that:

1. **World-map ZOOM is corrupted** — our overlay markers (which project through the live
   `CS::WorldMapArea` pan/zoom, see below) land wrong, and the native map itself zooms
   wrong. A **fresh launch at 1280×720 is fine**; only the *mid-session change* corrupts it.
2. **ImGui + minimap INPUT break** — mouse hit-testing is off (clicks/hover land at the
   wrong place), as if the input→viewport mapping kept the OLD resolution's dimensions.

**This is an ER-internal staleness bug, NOT ours.** Our DX12 overlay's own `ResizeBuffers`
hook correctly releases + rebuilds its RTVs against the new back buffers (verified — our
overlay draws fine at the new res). The corruption is in **ER's own** cached render/viewport
state, which it does not recompute on a mid-session resize. Current workaround in the handoff
notes: **restart ER after any resolution change**. We want to kill that restart.

## What we already know about the render/viewport math (use these, don't re-derive)

- **The engine renders the world map to a FIXED virtual `1920×1080` GFx/Scaleform canvas**,
  then scales that to the live backbuffer. The `1920.0` is a literal, confirmed in decompiled
  **`FUN_1409bdc50`** and **`FUN_1409ce190`**. Our projection multiplies by `realW/1920` and
  `realH/1080` to map the virtual canvas to backbuffer px (this is the canvas factor we
  recently added; it fixed a static placement bug). So the math depends on the **real
  backbuffer width/height** — if ER caches those at startup/first-map-open and never refreshes
  them on resize, every downstream scale is stale → exactly symptom (1).
- **`CS::WorldMapArea`** (the live view) is reached via `cursor + 0xF0`:
  pan vec2 `+0x378/+0x37C`, zoom f32 `+0x380`, static fullRect `+0x350 = [0,0,10496,10496]`.
- **Cursor** vtable RVA `0x2b29a90` (`CS::WorldMapCursorControl`); reticle/marker coords at
  `+0xFC/+0x104/+0x108/+0x10C`. The cursor sits at `WorldMapDialog + 0x2DB0`.
- **`CSMenuMan` static slot `0x143d6b7b0`** (RVA `0x3d6b7b0`), AOB
  `48 8B 05 ?? ?? ?? ?? 33 DB 48 89 74 24` disp@3/len 7. `+0xCD == 7` while the map is up.
- Input hit-testing for map icons goes through the generic **FD4 GUI hit-test `FUN_140d7ff40`**
  (Scaleform/VMP layer) — the input symptom (2) likely shares whatever viewport dims that path
  reads.

## The task

Find the **cached render/viewport state that ER fails to refresh on a mid-session resize**,
and a way to force the refresh. Two concrete hypotheses to confirm/refute:

1. **Stale backbuffer dimensions.** ER almost certainly stores the current render width/height
   (and/or the canvas→backbuffer scale derived from `1920×1080`) in a global render-config
   object, read each frame by the map projection (`FUN_1409bdc50`/`FUN_1409ce190`) and by the
   input mapping. On a fresh launch it's correct; on a mid-session resize ER either (a) never
   re-reads the new backbuffer size, or (b) re-reads it in a path that the map/menu code
   doesn't re-trigger. **Find the global(s) that hold render W/H (or the scale), where they're
   written (the resize/apply-settings path), and why the map view + input mapping read a stale
   copy.**
2. **Map view not re-initialised.** `CS::WorldMapArea`/`WorldMapDialog` may compute its
   zoom-fit / canvas scale **once at map-open** from the then-current dims and cache it; a
   resize while the map is open (or the dims changing under it) leaves that cached fit wrong.
   If so, closing+reopening the map *should* fix it — **check whether reopening the world map
   after the resize already clears symptom (1)** (if yes, the fix is to force a map re-init;
   if no, it's the global in hypothesis 1).

### Where to look

- **Does ER hook/observe its own `ResizeBuffers` (or `WM_SIZE` / a DXGI mode-change)?** Find
  the function that runs on an in-game resolution apply (the options-menu "Apply" path that
  resizes the swapchain). Trace what it updates — and crucially what it **doesn't** (the map
  canvas scale / the input viewport). The delta between "what a fresh launch initialises" and
  "what the resize path updates" is the bug.
- **The render-config global**: find where `FUN_1409bdc50`/`FUN_1409ce190` read the real
  backbuffer W/H (a CS render-system singleton field, or straight off the swapchain `GetDesc`).
  Is that value a cached global written only at startup, or live each frame?
- **The input viewport**: find where the FD4 GUI / Scaleform mouse mapping reads screen dims;
  confirm it's the same stale value (explains symptom 2 sharing the same root cause).

## Deliverable

1. **Root cause**: the specific cached field/global (give its RVA / owning singleton + offset,
   and how to re-find it by AOB since offsets shift per patch) that ER leaves stale after a
   mid-session resize, and why the map view + input read it.
2. **A no-restart fix**, one of (in preference order):
   - a) the **field(s) we can rewrite** from our `ResizeBuffers` hook (we already intercept
        `IDXGISwapChain3::ResizeBuffers` = vtable[13]) to push the new W/H/scale into ER's
        stale global so the map + input re-derive correctly; **or**
   - b) an **ER function we can call** (a "recompute viewport / re-apply render settings /
        re-init map" routine) from that hook to make ER refresh itself; **or**
   - c) confirmation that **closing+reopening the world map** (or some other in-game action) is
        sufficient — then we just prompt the user / trigger it.
3. A tiny C++ sketch of the chosen fix wired into `hk_resize_buffers` (see
   `goblin_overlay.cpp:2091`), with the offsets/AOBs filled in.
4. Note version-stability: resolve by singleton AOB + struct offsets and document re-finding.

## Notes

- Our `ResizeBuffers` hook is at `goblin_overlay.cpp:2091` (`hk_resize_buffers`) and already
  fires on the resize — it's the natural injection point for any write/call fix. We get the
  new `w/h` as params and the live swapchain.
- Scope guard: we only want the **mid-session** path fixed. Fresh-launch-per-resolution
  already works, so the missing step is purely "what ER does on apply-resize that it should
  also do to the map/input state."
- If the canvas→backbuffer scale turns out to be read **live each frame** (not cached), then
  hypothesis 1 is wrong and the bug is a stale *map-fit / input-viewport* cached at map-open or
  swapchain-create — pivot to hypothesis 2 and report which.
- Read-only first; only the final fix (2a/2b) writes/calls into the game.
