# Bloodstain read on 2.6.2.0 — SOLVED: offsets were RIGHT, the existence GATE was wrong (2026-07-08)

**Status: root-caused in Ghidra + fix shipped; one live confirm pending (next boot).**
Supersedes the 2026-07-08 first pass of this file — its two premises (AOB drift, stale offsets)
were both WRONG; kept below under "Corrected premises" because the *way* they were wrong is a
recurring trap.

## Symptom (user-found, live)
The player's own dropped-runes bloodstain (DropSoul) shows on ER's **native map** (ER's own resident
icon) but NOT on the mod's vmap / minimap. `death_state` / `bloodstain_probe` reported `souls=0`
while the bloodstain visibly existed.

## Root cause (Ghidra, exe 2.6.2.0, project `D:\ghidra_proj2\ER`)
**A 0-rune death leaves a REAL engine bloodstain with `souls == 0`, and the engine's icon is gated
on a separate EXISTS flag — not on `souls > 0`.** The mod's `read_bloodstain` used `souls > 0` as
the existence test (`goblin_custom_markers.cpp tick()` cleared the marker on `souls <= 0`), so ER
drew its icon while the mod suppressed its own marker. The offsets never moved.

The death writer `er+0x5fc0c0` (a `CSEventBloodStainCtrl` method) stamps the record then computes
`flag = (souls >= 0)` — true even for 0 — and stores it via the setter `er+0x259060`
(`*(u8*)(GameDataMan+0x40) = flag`). The icon/VFX placer `er+0x5fbc70` gates on that flag byte
(getter `er+0x256bc0`), NOT on the amount.

Secondary red herring: the probe's xyz seemed to "track the player across warps". The record's
xyz is block-local and **the engine REBASES the stored value** when the streaming origin moves —
`er+0x5fbc70` converts the position through the current frame and **writes it back** into the
record (`*puVar2 = local_108 …`). A fixed world point, re-expressed per origin: reads before/after
a warp differ without anything tracking the player. (The user's probe values were ALSO ~1.5 m from
the live player because they died right there — coincidence stacked on mechanism.)

## The 2.6.2.0 bloodstain map (all Ghidra-verified, er-relative RVAs)

**GameDataMan** = `*(er+0x3d5df38)` (the GAME_DATA_MAN AOB's getter `er+0x256590` reads this slot;
the slot is the true GameDataMan — readers include the player-data save serializer `er+0x257f20`,
`+0x8` PlayerGameData, `+0x10` PlayerGameData ARRAY (0xAE8 stride — PGD object size), `+0xA0` IGT
in ms, `+0x120` death counter (clamped 9999 by `er+0xb0e780`)).

| Where | What |
|---|---|
| `gdm+0x40` (u8) | **bloodstain EXISTS flag** — setter `er+0x259060`, getter `er+0x256bc0`; set on ANY death (even 0 runes), restored from the save, cleared on retrieve (`er+0x5fbc40` path) |
| `gdm+0x48` (ptr) | **the 0x40-byte bloodstain record**, streamed VERBATIM to/from the save (deserialize `er+0x256be0`: 1 flag byte → if set, 0x40 bytes into the record + 4 into `gdm+0x50`; else clear) |
| `gdm+0x50` (i32) | companion id from the dying chr (`FUN_1403ef9f0(chr)+8`), `-1` when none |
| record `+0x00/04/08` | xyz (block-local physics frame; REBASED by `er+0x5fbc70` on origin change) |
| record `+0x0c/10/14` | second vec3 (from `FUN_1406552b0(chr)` at death) |
| record `+0x30` (u32) | second dropped value — `PlayerGameData+0x60` at death, then pgd field zeroed |
| record `+0x34` (i32) | **dropped RUNES** — `PlayerGameData+0x6C` at death (minus a param-driven keep-back via `er+0x606d50`), pgd souls zeroed after; **-1 = cleared/none, 0 = valid 0-rune stain** |
| record `+0x38` (u32) | mapId (`m{AA}_{BB}_{CC}_{DD}` dword) |
| record `+0x3c` (u8) | condition byte (`FUN_1403f2a20(chr)` — multiplayer-ish flag) |

Engine functions: record getter `er+0x256370` (`return *(gdm+0x48)`); `gdm+0x50` addr getter
`er+0x256380`; record CLEAR `er+0x255930` (default xyz from statics + zeros @ +0xc/+0x14 + `-1` @
+0x30/34/38, and `gdm+0x50 = -1` — note it does NOT touch the flag byte); death-drop writer
`er+0x5fc0c0`; icon placer/rebaser `er+0x5fbc70`; ctrl reset `er+0x5fbc40`; on-death cleanup
`er+0xb0e780` (calls the clear, bumps the death counter, then the writer re-stamps);
icon arm/disarm `er+0x6ee210`. `CSEventBloodStainCtrl` itself (ctor `er+0x5fbc00`) is a 0x10-byte
shell — confirmed dead end, the state all lives on GameDataMan.

## Corrected premises (the traps from this file's first pass)
1. **"The AOB drifted 0x255b90 → 0x256590" — FALSE.** 0x255b90 was a **FILE offset** from the
   2026-07-03 offline scan; the .text raw→VA delta on this exe is **+0xA00**, so file 0x255b90 ==
   RVA 0x256590 — the SAME site (SAVE_FN's own note spells out the same delta: file 0x240f370 =
   RVA 0x240FD70). When comparing an offline-scan note to a live `[SIG]` address, convert first.
   re_signatures.hpp's GAME_DATA_MAN comment now records both forms.
2. **"gdm+0x48 points at CODE / gdm dumps as (heap, static) pairs" — irreproducible artifact.**
   Against the disk exe + Ghidra, slot `er+0x3d5df38` is the real GameDataMan and `+0x48` is the
   record pointer; the mod's own EQUIP chain resolved a heap gdm the same evening (18:49 log). The
   manual RPM that "saw code" was almost certainly aimed with a stale/wrong `er_base` (per-boot
   ASLR) or read at the wrong moment. **Lesson: don't hand-compute absolute addresses across
   sessions — use `er_base` from the SAME session, or better, dump the mod's own resolved pointers
   (`bloodstain_probe dbg` now exists for exactly this).**
3. "souls=1154 verified 2026-07-04 was on an older exe" — moot either way: the layout is identical
   on 2.6.2.0; the 07-04 read was simply a >0-rune stain.

## Fix shipped (2026-07-08, both builds green, deployed)
- `goblin::inventory::read_bloodstain(bool &exists, x, y, z, mapid, souls)` — `exists` =
  `flag@gdm+0x40 != 0 && souls >= 0` (the engine's own gate, belt-and-braces'd by the -1-clear
  convention). Callers updated; `death_marker::tick` clears on `!exists` instead of `souls <= 0`,
  so 0-rune stains now mirror onto vmap/minimap like ER's own icon.
- `bloodstain_probe` prints `exists=`; **`bloodstain_probe dbg`** dumps the resolved chain
  (match/slot er-RVAs, gdm, flag, aux50, raw 0x40-byte record hex) — the standing triage lens.
- `death_state` bloodstain section gains `exists=` + xyz.

## Live-verify checklist (next boot, ~2 min)
1. `mfg.py rpc mfg_build` — confirm the fresh DLL (freshness-first rule).
2. Load the save; if the old bloodstain still exists: `bloodstain_probe` → expect `exists=1`,
   plausible xyz/map; vmap/minimap show the DropSoul marker where ER's native map shows its icon.
3. `bloodstain_probe dbg` — record hex: +0x34 (runes) == the probe's souls, +0x38 mapid sane.
4. Die once WITH runes → `exists=1 souls>0`; retrieve them → `exists=0` (flag cleared) + marker
   gone. Die once with 0 runes → `exists=1 souls=0` + marker present (the fixed case).
5. ⚠ Linux/Proton box (older exe, same patch116 source lineage): re-run step 2-4 once — the flag
   byte @ +0x40 is expected identical, but it was only ever Ghidra-verified on 2.6.2.0.

## Diagnostics available
`death_state` (store vs game vs player one-liner), `bloodstain_probe [dbg]`, `death_mark` /
`death_clear` (manual sticky marker, commit 32cd57b).
