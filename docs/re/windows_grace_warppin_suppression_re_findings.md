# RE findings — the grace warp-pin DRAW-GATE (why `pin+0xC` failed; zero `pin+0x60` instead)

Answers `docs/re/windows_grace_warppin_suppression_re_prompt.md`. Static Ghidra RE
(`D:\ghidra_proj2\ER`, scripts `re_v119..v121`). App 2.6.2.0 / ERR 2.2.9.6, imagebase
`0x140000000`. Read-only.

---

## 0. TL;DR — the real draw-gate

**`pin+0xC` IS the draw flag, but it is a CACHED value the engine recomputes every map update.**
The visibility virtual **`vt[3] = FUN_14087afa0`** runs each tick and writes the boolean result
into **byte `pin+0xC`** (1 = draw, 0 = hide). One of its AND-inputs is the **state bitmask at
`pin+0x60`** (the value copied from the source `FUN_140d25540`). The old hook zeroed `pin+0xC`
once at build; the very next update recomputed it back to 1 (the grace is genuinely discovered →
state bit set). **That is why phase B "failed" — we were writing the engine's scratch output, not
its input.**

> **The previous doc/code mis-read the Ghidra offset.** In the decompile `param_1` is typed
> `undefined8*`/`longlong*` (8-byte stride), so `*(uint*)(param_1 + 0xc)` is **byte `0x60`**, not
> byte `0xC`. The builder writes the source state there. Byte `0xC` (`(longlong)param_1 + 0xc`) is
> a *different* address — the cached visible flag. The hook zeroed `0xC` (the cache); it needed to
> zero `0x60` (the input). Both happen to read "+0xc" in the C output — pure pointer-arithmetic trap.

**Recipe (replaces the failed `pin+0xC` write):** in `warp_pin_detour`, for a discovered grace,
`WriteProcessMemory(pin + 0x60, {0,0,0,0}, 4)` — zero the **state dword**. Every recompute of
`FUN_14087afa0` then yields invisible on all layers. Grace-only; player dot / objectives / fog /
markers untouched (separate objects). §3 has the drop-in.

---

## 1. Q1 — where the built pin goes (caller / container)

`FUN_14088b7b0` (the WarpPinData builder, already hooked) has **exactly one static call site**:
`@0x885cc5` inside **`FUN_1408855b0` = the `WorldMapViewModel` constructor** (`*this =
CS::WorldMapViewModel::vftable`). The warp-pin **list** is a VM field initialised by
`FUN_140885460(this + 0x5b)` → `this + 0x2d8` (vtable `CS::WorldMapPinDataList<WorldMapWarpPinData>`
`0x2ad82a8`); list layout `+0x10`/`+0x18` = begin/end, **stride `0x350`** (= `sizeof(WarpPinData)`;
`vt[count] FUN_140889610` = `(end-begin)/0x350`, `vt[at] FUN_140886530` = `begin + i*0x350`).

Pins are **value-copied** into the list (copy-ctor `FUN_140885ed0` → base `FUN_140885500`), so the
builder's output is a build temp that is copied in. **`FUN_140885500` copies BOTH `+0xC` (byte) and
`+0x60` (dword)** (`*(u32*)(dst+0xc) = *(u32*)(src+0x60)`), so a zero written to the temp's `+0x60`
in our detour propagates into the persistent list pin. (Hence we do **not** need to walk the list.)

## 2. Q2/Q3 — the gate field and the show-predicate

**Visibility virtual `vt[3] = FUN_14087afa0(pin, ctx, …, char forced)`** (per-frame; shared by Warp
*and* Point pins — both vtables slot[3] = `0x87afa0`):

```c
pin[0x64]  = forced;                                  // byte +0x64 cache of the forced arg
bVar1 = vt[13](pin) || forced;                        // FUN_14088bd70 "IsOpen" (checks pin+0x240)
layerbit = FUN_140887e90(ctx);                        // 0/1/2 per map layer, else -1
bVar5 = layerbit<0 ? 0 : (*(u32*)(pin+0x60) >> layerbit) & 1;   // <-- STATE BITMASK @ +0x60
bVar2 = vt[5](pin) || vt[11](pin);
bVar4 = vt[14](pin, zoom);                            // FUN_14088bd30: [pin+0x240]+0x12 <= zoom
pin[0xC] = (bVar1 && bVar5 && bVar2 && bVar4) ? 1 : 0;   // <-- CACHED VISIBLE FLAG @ byte +0xC
```

- **`pin+0x60` (u32)** = the **state bitmask** copied from `FUN_140d25540` (`[warpData+8]+0x1E` bits
  0/1/2). Bit *b* = "visible on layer *b*". A discovered grace has its layer bit set. **Zero this
  dword → `bVar5` is 0 for every layer → cached visible is forced to 0 every recompute.** This is
  the safe, grace-local input to kill.
- **`pin+0xC` (byte)** = the cached visible flag the renderer reads to decide draw. **Recomputed**
  by `vt[3]` each update — do **not** write here (that was the failed approach).
- **Show-predicate for warp pins** = this `vt[3]` itself (plus its sub-predicates `vt[13]` IsOpen,
  `vt[14]` zoom-threshold). There is no separate pre-insert predicate like the category system's
  `FUN_140a81450`; warp pins are built unconditionally and gated per-frame by `vt[3]`.

**Field map of `WorldMapWarpPinData` (0x350 bytes, vtable `0x2ad8228`):**

| off | type | meaning |
|----|----|----|
| +0x00 | ptr | vtable (`0x2ad8228`) |
| +0x0C | byte | **cached VISIBLE flag** (vt[3] output; renderer reads) — recomputed, do not poke |
| +0x50 | u32 | iconId (`FUN_140d25650`) |
| +0x54 | u32 | warp id (`[src+0x18]`) |
| +0x5c | u32 | =1 (getter `vt[9] FUN_14087c200`; active/priority) |
| **+0x60** | **u32** | **STATE bitmask** (`FUN_140d25540`; per-layer discovered) — **zero this to suppress** |
| +0x64 | byte | cached "forced" arg (vt[3]) |
| +0x68.. | 8× 0x38 | icon sub-layers |
| +0x238 | — | MapId/pos block (`param_3[0..3]`) |
| +0x240 | ptr | sub-record (id @+0x4, zoom thresholds @+0x12/+0x13) |
| +0x348 | byte | =1 layer-select flag (vt[12] `FUN_14088bb60`) |

## 3. Q4 — the safe suppression recipe (recommended: flip the real input field)

**Chosen = (c) flip the real draw field, at the CORRECT offset.** Zero `pin+0x60` (state dword) on
the built grace pin. Reasons the alternatives are worse:
- (a) **skip-build is UNSAFE** — the caller value-copies the built temp (`FUN_140885ed0`/`…500`); an
  early-return leaves a half/zero pin that gets copied → garbage in the list (deref/draw on junk).
- (b) clear/filter the container = hand-walking the vector (the brief's `[PINSET]` crash class).
- (d) no separate pre-insert predicate exists for warp pins.

Zeroing `+0x60` is grace-local (only this pin's per-layer bits), survives the value-copy into the
list (§1), and is re-applied every map-open because the detour fires on each rebuild. **No field
here is shared with the player dot / objectives / fog / player markers** — those are other objects
in other lists; `+0x60` is this `WarpPinData`'s own state. Belt-and-suspenders: also set `+0xC`=0 so
the *current* frame hides before the next `vt[3]` recompute (optional; `+0x60` alone is sufficient).

Drop-in for `warp_pin_detour` (replace the `ret + 0xC` 1-byte write):

```cpp
if (goblin::config::graceOverlay && ret && (state & 7) != 0)
{
    uint32_t zero = 0; SIZE_T n = 0;
    // +0x60 = the per-layer STATE bitmask (input to vt[3] FUN_14087afa0). pin+0xC is only the
    // CACHED visible flag the engine recomputes from +0x60 each update — writing it does nothing.
    WriteProcessMemory(GetCurrentProcess(),
        reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(ret) + 0x60), &zero, 4, &n);
    // optional: hide the current frame immediately (recomputed from +0x60 next tick anyway)
    uint8_t vis0 = 0;
    WriteProcessMemory(GetCurrentProcess(),
        reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(ret) + 0xC), &vis0, 1, &n);
}
```

**Caveat to runtime-check:** if a discovered grace ever re-appears after this lands, a *refresh*
path (not seen statically — the builder is the only writer of `+0x60` we found) may re-copy the
source state into `+0x60` while the map is open. If so, hook that refresh or `vt[3] FUN_14087afa0`
itself (force `+0xC`=0 when `pin[0]==WorldMapWarpPinData::vftable`). Not expected: graces aren't
discovered while the map screen is open, and the VM/pins rebuild on each open.

## 4. Handles / RVAs (resolve by AOB; ASLR)
- builder `FUN_14088b7b0` `0x88b7b0` (hooked). out=pin(`param_1`), `param_3`=WarpData.
- visibility virtual `vt[3] = FUN_14087afa0` `0x87afa0` (writes `pin+0xC`, reads `pin+0x60`).
- bit selector `FUN_140887e90` `0x887e90` (layer→bit 0/1/2). state `FUN_140d25540` `0xd25540`,
  iconId `FUN_140d25650` `0xd25650`.
- copy-ctor `FUN_140885ed0` `0x885ed0` → base `FUN_140885500` `0x885500` (copies `+0xC` and `+0x60`).
- VM ctor `FUN_1408855b0` `0x8855b0`; warp list = VM `+0x2d8` (init `FUN_140885460` `0x885460`,
  list vtable `0x2ad82a8`, stride `0x350`, `count FUN_140889610`/`at FUN_140886530`).
- WarpPinData vtable `0x2ad8228`: [3] vis `0x87afa0`, [9] getter `+0x5c` `0x87c200`, [13] IsOpen
  `0x88bd70`, [14] zoom `0x88bd30`.
- gate field: **`pin+0x60` (u32 state bitmask)** = suppress (write 0); `pin+0xC` (byte) = cached
  output, do not rely on.
```
