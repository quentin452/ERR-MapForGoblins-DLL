# Windows RE brief — O(1) pointer chain to the world-map cursor (kill the memory scan)

**App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. Read-only.** Goal: resolve the live
world-map cursor object via a **static singleton → fixed-offset pointer chain**, so the
overlay reads it in O(1) every frame instead of scanning the whole address space for its
vtable. Read-only (we only read the cursor's coords/view).

## Why

`goblin_worldmap_probe.cpp` currently finds the cursor by AOB-scanning **all committed RW
memory** for the `CS::WorldMapCursorControl` vtable value, on a timer. That scan is multi-GB
and slow → up to ~5s "no live view" on map open (partially mitigated by gating the scan on
map-open + RW-only, but the first-open scan is still O(memory)). It also adds a tick of lag:
the overlay markers "dash"/catch up when you pan the map, because the published cursor/view
is a tick behind the game. A direct pointer chain fixes BOTH: instant resolve + per-frame
live read with no lag.

## What we already have (use these, don't re-derive)

- **`CSMenuMan` singleton**: AOB `48 8B 05 ?? ?? ?? ?? 33 DB 48 89 74 24`, rel offsets
  `{3,7}` (the mod's `world_map_open()` uses this). **`CSMenuMan + 0xCD`** is a per-screen
  state byte; **`== 7` while the world map is up** (value carried from a previous build —
  **please reconfirm 0xCD/7 on 2.6.2.0** while you're in there; log distinct values across
  open/close if it's wrong).
- **The cursor object**: first qword = the `CS::WorldMapCursorControl` vtable at **RVA
  `0x2b29a90`**. Reticle/marker coords at `+0xFC/+0x100/+0x104/+0x108/+0x10C`.
- **`cursor + 0xF0` → `WorldMapArea`** (the view): pan `+0x378/+0x37C`, zoom `+0x380`,
  fullRect `+0x350 = [0,0,10496,10496]`.
- **Crucially: the cursor sits at `menu + 0x2DB0`** — i.e. there is a "world-map menu"
  object and the cursor is embedded at its `+0x2DB0` (the probe already uses
  `CURSOR_OFF_IN_MENU = 0x2DB0`; it recovers `menu = cursor − 0x2DB0` after the scan).

So the missing piece is just: **how to reach that menu object from a static singleton**,
without scanning for the cursor vtable.

## The task — find the chain `singleton → … → menu (→ +0x2DB0 = cursor)`

Most likely path: `CSMenuMan` holds the set/stack/array of active menu screens; the
world-map screen is one entry. Find the offset(s) from `CSMenuMan` to the world-map menu
instance (the object whose `+0x2DB0` carries the cursor vtable). Candidate approaches:

- Walk `CSMenuMan`'s members for a pointer (or array of pointers) to per-screen menu
  objects; identify the world-map one by `*(menu + 0x2DB0) == cursor_vtable` (RVA
  `0x2b29a90`) — that's the ground-truth check to confirm you found the right entry/offset.
- Or find the world-map menu's own singleton/owner directly (its ctor / a global it writes
  itself into). The screen is `CSWorldMapMenu`-like; locate its registration into
  `CSMenuMan` (the menu-open path) and read back the offset.
- The state byte `CSMenuMan + 0xCD == 7` indicates the world-map screen index — that index
  may itself select the slot in a screen array (e.g. `CSMenuMan + <base> + index*8`). Worth
  checking if `0xCD` is an index into the same table that holds the menu pointer.

## Deliverable

1. The **pointer chain**: `CSMenuMan (+offsets / [index]) → worldMapMenu`, such that
   `worldMapMenu + 0x2DB0` is the cursor (verify `*(that) == base + 0x2b29a90`). Give every
   offset and whether each step is a deref. If a different singleton is cleaner than
   `CSMenuMan`, give its AOB instead.
2. Reconfirm **`CSMenuMan + 0xCD == 7`** for "world map open" on 2.6.2.0 (or the correct
   offset/value).
3. A tiny C++ resolver sketch:
   ```cpp
   // pseudo — fill the offsets
   void* mm = *(void**)menuman_slot;            // CSMenuMan
   if (!mm || *(uint8_t*)((char*)mm + 0xCD) != 7) return nullptr; // map not up
   void* menu = *(void**)((char*)mm + OFF_A);    // → world-map menu  (+ maybe [index])
   void* cursor = (char*)menu + 0x2DB0;          // embedded cursor
   if (*(uintptr_t*)cursor != base + 0x2b29a90) return nullptr;    // sanity
   // cursor+0xFC.. coords ; *(cursor+0xF0) → WorldMapArea (pan +0x378, zoom +0x380)
   ```
4. Note version-stability: prefer resolving by singleton AOB + struct offsets (offsets shift
   per patch, so document how to re-find them).

## Notes

- The cursor object can have several mirror instances carrying the vtable (the scan saw
  ~3); the **menu-embedded one (`menu + 0x2DB0`) is the canonical/active one** — that's why
  the chain is better than the scan (no "which mirror?" ambiguity).
- This unblocks: (a) instant map-open live view, (b) per-frame lag-free read (fixes the pan
  "dash"), (c) deleting the whole-memory scan from `goblin_worldmap_probe.cpp`.
