# Bloodstain read is BROKEN on exe 2.6.2.0 — `read_bloodstain` reads the wrong data (2026-07-08)

## Symptom (user-found, live)
The player's own dropped-runes bloodstain (DropSoul) shows on ER's **native map** (ER's own resident icon,
NO mod tooltip) but NOT on the mod's **vmap / minimap** (and not via the mod's native-map DropSoul block).
`death_state` / `bloodstain_probe` report **`souls=0`** even while a real bloodstain exists.

## What is actually happening (confirmed by live RPM)
`goblin::inventory::read_bloodstain` (`goblin_inventory.cpp`) resolves `GameDataMan` via the `GAME_DATA_MAN`
AOB, then reads `blk = *(gdm+0x48)` and pulls `souls@+0x34`, `mapid@+0x38`, `xyz@+0/4/8`. Live trace on the
running 2.6.2.0 exe (er_base `0x7ff7d2170000`):

- `[SIG] PASS GAME_DATA_MAN -> 0x7ff7d23c6590` (AOB match = **er+0x256590**). ⚠ The re_signatures note says
  this AOB was unique **@ er+0x255b90** on 2026-07-03 — it has **DRIFTED to er+0x256590**. Still "unique"
  (1 match), so the [SIG] health check PASSes, but a drifted-but-unique AOB can resolve a **different**
  same-prologue function/singleton.
- Ghidra: that match → slot **er+0x3d5df38**; live `*(er+0x3d5df38)` = `gdm = 0x7ff7d5c84ba8`.
- `gdm` dumps as a list of **(heap_ptr, er-static_ptr) pairs**, and **`gdm+0x48` = `0x7ff7d2483140`**, which
  dumps as **CODE** (`48 8b c4 48 81 ec 98 00 00 00…` = a function prologue), NOT a bloodstain struct.
- Yet `bloodstain_probe` returns `xyz` that **tracks the live player** across warps (e.g. (-5.5,88.6,-59.8)
  → (-5.6,92.7,-78.7)) with `souls=0`. A REAL bloodstain is FIXED at the death spot with `souls>0`.

⇒ Two certainties: (1) `souls=0` here is **garbage / the wrong field**, not "no bloodstain"; (2) the read
returns a **player-tracking** value, so it is NOT reading the fixed dropped-runes struct. The old RE
(souls=1154 verified 2026-07-04) was on an **older exe**; the GameDataMan chain and/or the `+0x48/+0x34/+0x38`
offsets are wrong for 2.6.2.0.

## Unresolved contradiction (flag for the next pass)
`gdm+0x48` traces to CODE, but the mod reads player-position-like floats from `*(gdm+0x48)+0/4/8`. Those two
can't both be true of the same address — so either the mod's live `gdm` differs from the `0x7ff7d5c84ba8`
computed here (AOB match → slot → deref), or `modutils::scan`'s buffer vs live-module mapping shifts the
resolved pointer. **Resolve this first** (dump the mod's actual resolved `gdm`/`blk` — add them to a debug
RPC) before trusting any offset.

## The fix = re-RE the RESIDENT bloodstain for 2.6.2.0 (not done)
The DropSoul the native map draws is the RESIDENT source (updates live on death, map-open). Find it, read it
map-closed for the minimap. Options, cheapest first:
1. **Cheat Engine find-what-accesses** with an ACTIVE bloodstain (die, leave runes): scan for the rune count
   (a known int) → the resident struct → walk to a stable static/singleton path. (CE comfort is on this box.)
2. **Ghidra**: trace what writes the native-map DropSoul point / the lost-runes retrieval on death →
   the resident struct + a static resolve. Classes seen: `CSEventBloodStainCtrl` (er+0x5fbc00) is a thin
   0x10-byte object (dead end); the Net/Ghost/Job `Bloodstain*` classes are MULTIPLAYER. The player's
   lost-runes is likely a `GameDataMan`/`PlayerGameData` sub-field — re-derive the correct offset.
3. Re-verify/re-find the `GAME_DATA_MAN` AOB itself (it drifted) and re-derive `PlayerGameData` (GameDataMan+0x8
   per re_signatures) → the lost-runes fields, on 2.6.2.0.

## Diagnostic already shipped
`death_state` RPC (commit ae1b40c) dumps `store[...] bloodstain[read= souls= map=] player[...]` in one line —
use it to watch the read across a load / after a death. `death_mark` is now STICKY (survives tick's clear).
