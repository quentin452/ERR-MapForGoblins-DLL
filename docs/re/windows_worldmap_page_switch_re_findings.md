# RE findings — DRIVE the world-map page switch (instrumentation pass 1)

Answers part of `docs/re/windows_worldmap_page_switch_re_prompt.md`. Method: the [PAGESW]
instrumentation probe (config `debug_page_switch`, goblin_worldmap_probe.cpp) — minhook each candidate
handler, log `{fn, args rcx/rdx/r8/r9, page+layer before→after}`. App 2.6.2.0 / ERR 2.2.9.6.
Runtime-captured 2026-06-26 (overworld⇄underground⇄DLC, mouse input).

## 1. Which handlers actually fire (corrects the page-transition doc's guesses)
Of the 8 hooked (the 6 "map siblings" + `c40f0` + page-apply `c8120`), only **THREE** ever fired:
`c1fc0`, `c7900`, `c8120`. The other five (`c40f0`, `c5d20`, `c23d0`, `c2c00`, `c3280`) **never fired**
on overworld⇄UG⇄DLC — so the prior doc's "`FUN_1409c40f0(dialog,layer,force)` = surface↔UG" label was
WRONG (c40f0 is dead for this path; maybe a different input mode / dead code).

## 2. The two real switch entries (PINNED)
- **`FUN_1409c1fc0` @ RVA `0x9c1fc0` = base ⇄ DLC page switch.**
  - `arg2` (rdx) = **TARGET PAGE**: `0x0` = overworld, `0xA` (10) = DLC. Confirmed both directions
    (`page 0→10` when arg2=0xA; `page 10→0` when arg2=0x0).
  - `arg4` (r9) = `0x1` every call (likely a force/animate flag).
  - `arg3` (r8) = inconsistent (`0x7ff7…` image addr one way, `0x20` the other) → **not a meaningful
    arg** (leftover register; c1fc0 likely takes `(dialog, page)` + the force flag).
- **`FUN_1409c7900` @ RVA `0x9c7900` = surface → underground layer switch.**
  - `arg2` (rdx) = **TARGET LAYER**: `0x1` = underground (`layer 0→1` confirmed, 3×).
  - `arg3` = `0x40`, `arg4` = `0x1` (consistent).
  - ⚠️ **UG→surface (`arg2=0`) NOT yet captured** — the test toggled UG ON then changed page, never
    toggled UG OFF explicitly. So it's UNCONFIRMED whether c7900(dialog,0) returns to surface, or
    whether the surface direction is a different fn (possibly the never-fired `c40f0`, matching the old
    doc's (dialog,layer,force) signature). **Re-capture: open map, toggle underground ON then OFF.**
- **`FUN_1409c8120` @ RVA `0x9c8120` = page-apply** — a SUB-CALL invoked by c1fc0/c7900 (writes the 9
  lists, per the old doc), not a top-level entry. arg2 = the applied page/layer value. Don't drive this.

`dialog` (rcx, the WorldMapDialog) was stable at `0x1b79f598080` across the session = `cursor − 0x2DB0`.

## 3. Resulting recipe (provisional — pending §4 thread-safety + the UG→surface confirm)
```cpp
// target group bits: bit1 = DLC, bit0 = underground.  current via get_live_view (openDlc/underground).
void request_switch(int targetGroup) {
    void* dialog = active_cursor - 0x2DB0;
    int wantPage  = (targetGroup & 2) ? 10 : 0;   // c1fc0 arg2
    int wantLayer = (targetGroup & 1) ? 1  : 0;   // c7900 arg2
    if (curPage  != wantPage)  c1fc0(dialog, wantPage,  /*r8*/0, /*force*/1);
    if (curLayer != wantLayer) c7900(dialog, wantLayer, /*r8*/0x40, /*force*/1);  // UG-dir confirmed; surface dir TBD
}
```

## 4. STILL OPEN (next passes)
1. **UG→surface direction** — re-capture toggling underground OFF (see §2): does `c7900(dialog,0)`
   do it, or `c40f0`? Needed before driving the layer.
2. **THREAD SAFETY (the blocker).** These handlers run on the game's menu/UI thread (the input path).
   We must NOT call them from the Present/render-thread hook (races the per-frame UI step
   `FUN_1409c32f0` on the same dialog/9-lists). Options to RE/decide: (a) find the CALLER of
   c1fc0/c7900 (the input handler) and feed it / set a request field it drains on the UI thread;
   (b) marshal our call onto a game-thread hook; (c) empirically test a render-thread call behind a
   flag (crash ⇒ need a/b). To inform this, extend the probe to log `GetCurrentThreadId()` in the
   detour + the caller return address.
3. **`page_selectable(group)` predicate** — so we never switch to a locked page (no DLC tab pre-SOTE,
   no UG pre-discovery). The map's own tab-enable state (near the dialog) is the target; reuse the fog
   oracle for per-region reveal. Untouched by this pass.

## 5. Handles
- Probe: `goblin_worldmap_probe.cpp` `install_page_switch_probe()` / `ps_detour<N>` (config
  `debug_page_switch`). RVAs in `PS_HANDLERS[]`. Logs `[PAGESW]` to MapForGoblins_wmprobe.log.
- Confirmed entries: `c1fc0` `0x9c1fc0` (page/DLC), `c7900` `0x9c7900` (layer/UG); sub-call `c8120`
  `0x9c8120`. dialog = `cursor − 0x2DB0`; cursor via the CSMenuMan walk.
