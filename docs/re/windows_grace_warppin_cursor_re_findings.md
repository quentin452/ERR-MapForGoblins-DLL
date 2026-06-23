# RE findings — selection gates on pin+0xC, draw copies it to the GFx row → toggle pin+0xC around SetTo (RUNTIME-CONFIRMED)

Answers the "does selection need _visible?" question (piste 2). Static Ghidra RE
(`D:\ghidra_proj2\ER`, scripts `re_v126..v129`) + runtime `[SETTO]` log. App 2.6.2.0 /
ERR 2.2.9.6, imagebase `0x140000000`.

---

## 0. TL;DR — the working decouple (confirmed in-game)

The map cursor's pin selection is **game-side**: `WorldMapItemControl` `vt[6] FUN_1409cab60`
("nearest selectable pin") walks the VM warp list and, **for each pin where `pin+0xC != 0`**
(+ a zoom predicate), reads the pin's position and keeps the nearest to the cursor reticle.
The confirm then warps to that pin.

⇒ **Selection reads `pin+0xC` directly from the `WorldMapWarpPinData` object** — NOT the GFx
row's `_visible`. The draw, by contrast, is the per-pin **GFx row** bound by `SetTo`, whose
`_visible` is *copied from* `pin+0xC` at bind time. Same source byte, two different consumers
at two different moments → decouplable by **timing**:

> **In the `SetTo` hook: set `pin+0xC = 0` BEFORE `orig`, restore it AFTER.**
> `SetTo` then binds the row `_visible = 0` (draw gone), but every later read of `pin+0xC`
> (selection `vt[6]`, `vt[3]` recompute) sees the restored value → **fast-travel survives.**

**Why the obvious "hide the row after orig" FAILED (runtime-proven):** `widgetRoot` is a
**per-call stack GFx proxy** (same address for every pin), and `SetTo` **RELEASES it at its end**
(`FUN_140d7f850(proxy+0x28)`). So calling `set_visible(widgetRoot, 0)` *after* `orig` hits a
**dead proxy** → `FUN_140733340` gets a null underlying → silent no-op (the "effet d'optique";
the old `Icon_0`-child hide failed for the same reason). The fix must act **through `orig`'s own
live-proxy `set_visible`**, hence the pre-`orig` `pin+0xC=0`.

> Why tactic A failed twice: it hid the **`"Icon_0"` child** (and earlier crashed on the
> proxy release). Hiding a child left the rest of the row drawn ("effet d'optique"). Hiding
> the **root row** removes the whole pin sprite, and needs none of the proxy dance.

---

## 1. The selection path (game-side, reads pin+0xC)

`WorldMapDialog`/control ctor `FUN_1409be5e0` binds three GFx lists to VM lists via the
generic controller `FUN_1409ca380` → `CS::WorldMapItemControl` (also `CS::WorldMapSelectable`):
- `"Body/_/Base/WarpList"` ⇄ **VM warp list `VM+0x2d8`** → controller `control+0x61d`.
- (also `ItemList`, `AreaList`).

Controller fields: `+0x18` = the **VM list** (count vt `[+8]`, at(i) vt `[+0x28]`), `+0x20` =
shared state, `+0x28` = GFx, two vectors `+0xa8/+0xc8` (rows).

Selection = `CS::WorldMapItemControl::vftable` (`0x2b2c7e8`) **`vt[6] FUN_1409cab60`** — nearest
pin to a reticle point `param_2`:
```c
for (i=0; i < list.count(); i++) {
    pin = list.at(i);
    if (*(char*)(pin + 0xc) != 0                     // <-- pin VISIBLE flag (the gate)
        && pin->vt[10](zoom) != 0) {                 // vt[10] FUN_14088bb20: [pin+0x240]+0x13 <= zoom
        pos = pin->vt[4]();                           // vt[4] FUN_14087c210 -> pin+0x10 (marker pos)
        d2 = dist2(param_2, pos);
        if (d2 < best) { best = d2; nearest = i; }
    }
}
return nearest;   // the pin the cursor snaps to / confirm warps to
```
Same `pin+0xC` gate appears in the geometry/refresh passes `vt[5] FUN_1409cacc0` and
`vt[8] FUN_1409cae50`. None of them read the GFx row's `_visible` — they read the **pin
object**. (`param_2` is the cursor reticle in marker space = `WorldMapCursorControl+0xFC/+0x104`.)

## 2. The draw path (separate: the GFx row)

`vt[1] SetTo FUN_14087ae20(pin, widgetRoot)` binds each pin to its **per-pin GFx row**
(`widgetRoot`, with children `"Icon_0"`/`"Cleared"`). Its first action:
`FUN_140733340(widgetRoot, *(byte*)(pin+0xC))` → sets the **row `_visible` = `pin+0xC`**.

So `pin+0xC` is the *source*; the row `_visible` is a *copy* made at bind time. `SetTo` then
**releases** the row proxy, so the copy can only be influenced *through `SetTo` itself*.

## 3. Recipe — toggle pin+0xC around SetTo (RUNTIME-CONFIRMED working)

In `warp_setto_detour` (hook on `FUN_14087ae20`), for a discovered grace pin, zero `pin+0xC`
**before** `orig` and restore it **after**:
```cpp
bool suppress = (pin[0] == WorldMapWarpPinData::vftable)      // er+0x2ad8228 (shared w/ point pins)
             && ((*(u32*)(pin + 0x60) & 7) != 0);            // discovered grace
uint8_t saved;
if (suppress) { saved = *(u8*)(pin+0xC); *(u8*)(pin+0xC) = 0; }   // SetTo binds row _visible = 0
ret = orig(pin, widgetRoot, a3, a4);                              // hides via its OWN live-proxy set_visible
if (suppress) *(u8*)(pin+0xC) = saved ? saved : 1;               // restore -> selection vt[6] keeps it
```
No GFx proxy handling at all → the `0xC000001D` surface is gone. `SetTo` re-runs each refresh so
the toggle re-applies. Safe because the engine map UI is single-threaded: `SetTo` (bind) and
`vt[6]` (selection) are separate passes, so `pin+0xC` is always restored before selection reads it.

**Why not poke after `orig`:** the row proxy is dead by then (released in `SetTo`) → no-op (see §0).
**Generalises** to other pin types in the same controller family (Point/Area/Sign): same toggle,
swap the vtable filter and the discovered gate (the `+0x60` bitmask is warp-specific).

## 4. Handles / RVAs (resolve by AOB; ASLR)
- control ctor `FUN_1409be5e0` `0x9be5e0` (binds `"…/WarpList"` ⇄ `VM+0x2d8`); binder `FUN_1409ca380` `0x9ca380`.
- controller class `CS::WorldMapItemControl` vtable `0x2b2c7e8` (`+0x18` VM list, count `[+8]`, at `[+0x28]`).
  - **selection `vt[6] FUN_1409cab60` `0x9cab60`** (nearest pin; gate `pin+0xC != 0`).
  - geometry `vt[5] FUN_1409cacc0` `0x9cacc0`; refresh `vt[8] FUN_1409cae50` `0x9cae50` (call `SetTo` per row).
- pin pos getter `vt[4] FUN_14087c210` `0x87c210` (→ pin+0x10); zoom pred `vt[10] FUN_14088bb20` `0x88bb20`.
- draw bind `vt[1] SetTo FUN_14087ae20` `0x87ae20` → row `_visible = pin+0xC` via `FUN_140733340` `0x733340` (inner `FUN_140d844d0` `0xd844d0`).
- cursor `CS::WorldMapCursorControl` vtable `0x2b29a90`, reticle `+0xFC/+0x104`.
- gate field `pin+0xC` (read by BOTH draw-copy and selection); state `pin+0x60`; warp id `pin+0x54`; sub-record `pin+0x240`.
```
