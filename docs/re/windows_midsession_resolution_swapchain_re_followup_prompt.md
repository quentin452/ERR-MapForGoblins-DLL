# Windows RE follow-up — clean the swapchain on a mid-session resolution/mode change

Follows `windows_midsession_resolution_swapchain_re_findings.md` (commit 3ce2b18) and
`..._re_prompt.md`. App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`, MSVC `.text`.
Static Ghidra; resolve by AOB.

## What we shipped + what it fixed / didn't

We implemented the **raw-poke (fix-3)**: on a swapchain resize, write each render-output
entry's source dims (`mgr+outIdx·0x30+0x3c8/0x3cc`) and active dims (`entry+0x108/0x10c` int,
`entry+0x110/0x114` offsets=0, `entry+0x118/0x11c` float) to the new W/H, via WriteProcessMemory.
Enforced every frame from the Present hook (idempotent).

**Result (in-game, `[RENDIMS]` confirms):**
- ✅ **Borderless/same-mode, same-aspect** (1920×1080 ⇄ 1280×720): the **zoom is fixed**. The
  entry now reads `activeWH=srcWH=1280×720` matching the backbuffer.
- ❌ **Windowed**, change resolution mid-session → **still zooms** (the poke didn't fix it).
- ❌ **Fullscreen**, set 1280×720 → the screen shows **a doubled / tiled render ("two games")** —
  the game frame in roughly the top-left + a ghost/old buffer elsewhere. **Crucially: at that
  moment every dim is CONSISTENT** — `backbuffer=1280×720`, `entry#0 activeWH=1280×720`,
  `srcWH=1280×720`, `rscale=1`, `dirty(+0x140)=0`, single output entry. So the doubling is **not**
  a dims mismatch — the **swapchain back-buffers / present geometry** are stale (old buffers not
  released/recreated). A poke of the dims fields can't touch that.

So: dims-only is the wrong layer for mode/fullscreen changes. We need ER to do the **real**
re-derive (release+recreate the swapchain buffers / re-run the mode-test), which the mid-session
apply path skips for these output entries.

## The questions

### Q1 — preferred: can we make ER re-derive ITSELF by flagging the entry dirty?
The findings show the stager `FUN_1419eac90` does: stage the res descriptor to `entry+0x144..+0x164`,
set `entry+0x140 = 1` (per-output dirty) and `mgr+0xeb8 = 1` (manager re-apply), and then the
render-thread consumer `FUN_1419ed870` "performs the real swapchain resize + downstream re-derive"
on dirty entries.

- **If we replicate the stager — write the new res to `entry+0x144` (+ the rest of the ~0x24-byte
  descriptor) and set `entry+0x140=1` / `mgr+0xeb8=1` — will `FUN_1419ed870` do the COMPLETE clean
  (swapchain buffers included), fixing both the windowed zoom and the fullscreen doubling?**
- **Give the exact staged-descriptor layout** at `entry+0x144..+0x164`: what each of `res[0..4]`
  is (the stager copies them from `cfg+0xc0`). Is `+0x144` just `width|height` packed, and what do
  `+0x14c/+0x154/+0x15c/+0x164` need to be (refresh rate? format? mode? flags?) for the consumer to
  accept it? Decompile the consumer `FUN_1419ed870` (and `FUN_1419eb800` per-output apply) to list
  which staged fields it reads.
- Any guard that blocks our hand-staged entry (e.g. the consumer only runs on the render thread, or
  needs `mgr+0xeb8` set atomically, or compares against `cfg` state)? We call from the
  Present/ResizeBuffers thread.

### Q2 — fallback: the full re-apply `FUN_1419ed440(mgr, W, H)`
- Confirm it is the **complete** mode/res re-apply (updates source dims, recomputes active, and runs
  the swapchain mode-test `thunk_FUN_141e9d290` to release+recreate buffers). Does calling it with
  just `(mgr, W, H)` suffice, or does it read the screen-mode **config struct** (the `cfg` with
  `+0xAC`=mode, `+0xC0/0xC4`=W/H) — i.e. must we pass/refresh `cfg` too for fullscreen vs windowed?
- **Is it safe to call from the swapchain Present thread**, NOT during a resize (we'd call it from
  the Present hook, edge-triggered once per detected change)? Any re-entrancy with DXGI
  (it may itself call `ResizeBuffers`/`SetFullscreenState` → re-enter our hooks) or a lock that
  deadlocks if called outside its normal driver context (`FUN_140e898d0`)?
- Does it need the render-thread-id guard (`[mgr+0x6d0]+0x14 == GetCurrentThreadId()`) to be true?

### Q3 — why does WINDOWED still zoom after the dims poke?
We poke ALL output entries to the same W/H. In **windowed** mode the world still zooms, so something
else is res-dependent there:
- Is there a **separate** windowed render-output entry / viewport whose dims live elsewhere (a
  different field than `+0x118/+0x11c`), or a window-client-rect the GFx fit `FUN_140d84990` reads
  in windowed mode? (`FUN_140e8f910` had distinct windowed/fullscreen/borderless res sources.)
- Or does windowed use a different `outIdx`/entry we're writing the wrong value into (we write the
  backbuffer `GetDesc` W/H — in windowed is the render res different from the swapchain buffer size,
  e.g. a render-scale `+0x3ec ≠ 1`)? Tell us the windowed-specific field to read/write.

### Q4 — the doubled "two games" render specifically
At the doubling moment all dims read 720 but the present is wrong. Identify the field/state that
controls the **present source rect / swapchain buffer count / scaling** that stays stale on a
mid-session mode change (e.g. a `DXGI_SWAP_CHAIN_DESC`-mirror the engine caches, a present source
sub-rect, or the buffer-count/format). That's the field whose staleness tiles the old buffer under
the new frame.

## Deliverable
1. **The recommended approach** (Q1 dirty-flag re-derive preferred if it cleans the swapchain;
   else Q2 full re-apply) with the exact writes/call + thread/safety constraints.
2. Q1: the staged-descriptor layout `entry+0x144..+0x164` + which fields `FUN_1419ed870` consumes.
3. Q3: the windowed-specific stale field.
4. Q4: the present/buffer field behind the doubling.
5. A C++ sketch (writes or call) for our Present-hook enforcer (edge-triggered), and how to detect
   "needs re-apply" cheaply (we already read `entry+0x118` vs the live `GetDesc`).
6. AOBs for any new functions/slots; note version-stability.

## Context we already have (don't re-derive)
- renderMgr `DAT_1447ef360` (= `base+0x47ef360`); output list `mgr+0x128`(begin)/`+0x130`(end),
  stride `0x170`, key `entry+0x128=outIdx`.
- active dims `entry+0x108/0x10c`(int) `+0x110/0x114`(off) `+0x118/0x11c`(float); source
  `mgr+outIdx·0x30+0x3c8/0x3cc`, rscale `+0x3ec`; per-output dirty `entry+0x140`; staged res
  `entry+0x144..+0x164`; mgr dirty `mgr+0xeb8`; render-thread id `[mgr+0x6d0]+0x14`.
- stager `FUN_1419eac90`; consumer `FUN_1419ed870` (AOB `40 55 57 48 8D 6C 24 B1 …`); recompute
  `FUN_1419ebb40` (`48 8B C4 55 41 54 41 55 41 56 41 57 48 8D A8 B8`); full re-apply `FUN_1419ed440`
  (`40 53 55 56 57 48 81 EC B8 00 00 00 48 8B 05 5D`); per-frame display driver `FUN_140e898d0`;
  swapchain mode-test `thunk_FUN_141e9d290`; map GFx fit `FUN_140d84990` (reads `entry+0x118`).
- Our hook point: `hk_resize_buffers` + per-frame `hk_present` (goblin_overlay.cpp); fix lives in
  `goblin_worldmap_probe.cpp::fix_render_dims` (currently the raw-poke).
