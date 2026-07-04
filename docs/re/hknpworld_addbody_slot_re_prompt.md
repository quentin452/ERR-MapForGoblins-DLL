# hknpWorld::addBody vtable slot — naming prompt (Ghidra, Windows)

**Goal:** name the `hknpWorld` vtable functions so we can call **`addBody`** (+ `removeBody`) to spawn a
collision body at runtime = the last static gap for `add_collision` (Route D, the walkable-greybox brick;
`custom_asset_creation_options_re_findings.md` §D + `windows_terrain_heightfield_write_re_findings.md`).

## Chain CONFIRMED LIVE (Linux/Proton, 2026-07-05)
The resolve is validated end-to-end in-game:
```
instance    = *(er + 0x3d76060)            // CS::PhysWorld FD4Singleton (PHYSWORLD_RVA, already in goblin_heightfield.cpp)
CSPhysWorld = *(instance + 0x98)
hknpWorld   = *(CSPhysWorld + 0x08)
vtable      = *hknpWorld = er + 0x2eedc78   // MATCHES the findings RVA exactly ✓
```

## The task
The `hknpWorld` vtable (`er+0x2eedc78`) slots 0–31 point to these function RVAs (live dump). In Ghidra
(`D:\ghidra_proj2\ER`, imagebase `0x140000000`), name each and identify **`addBody`** (inserts a body into
the world/broadphase — takes a body id or a `hknpBody`/descriptor; touches the broadphase + body arrays) and
**`removeBody`**. Report the **SLOT INDEX** (and `+0xNN` offset) of each.

```
slot  0 +0x000  er+0x1688160        slot 16 +0x080  er+0x18af450
slot  1 +0x008  er+0x18a9c10        slot 17 +0x088  er+0x18af490
slot  2 +0x010  er+0x1680c10        slot 18 +0x090  er+0x18af480
slot  3 +0x018  er+0x33a40c0        slot 19 +0x098  er+0x18af4a0
slot  4 +0x020  er+0x1688160        slot 20 +0x0a0  er+0x18af470
slot  5 +0x028  er+0x18a8d20        slot 21 +0x0a8  er+0x18afbc0
slot  6 +0x030  er+0x1680c10        slot 22 +0x0b0  er+0x18af460
slot  7 +0x038  er+0x1931ed0        slot 23 +0x0b8  er+0x18af4d0
slot  8 +0x040  er+0x19329c0        slot 24 +0x0c0  er+0x18af4e0
slot  9 +0x048  er+0x33a4150        slot 25 +0x0c8  er+0x18af4b0
slot 10 +0x050  er+0x18a9670        slot 26 +0x0d0  er+0x18af4c0
slot 11 +0x058  er+0x251c480        slot 27 +0x0d8  er+0x18af4f0
slot 12 +0x060  er+0x18aef90        slot 28 +0x0e0  er+0x18af1e0
slot 13 +0x068  er+0x18aefc0        slot 29 +0x0e8  er+0x18af1d0
slot 14 +0x070  er+0x18aefa0        slot 30 +0x0f0  er+0x18af1f0
slot 15 +0x078  er+0x18aefb0        slot 31 +0x0f8  er+0x33a41c8
```
(The `0x18af4xx` cluster slots 16–27 look like a family of small accessors/setters; `addBody`/`removeBody`
are more likely the heavier `0x1931ed0`/`0x19329c0`/`0x18a9670`/`0x18a8d20`/`0x251c480` entries — but
CONFIRM by decompiling, don't guess.)

## Also confirm (so the DLL can build the call)
- **`addBody` full signature** — what it takes (a `hknpBody*`, a body id, a `hknpBodyCinfo`/descriptor?),
  return, and any required world lock/step-guard around it (the terrain findings say scene/world writes must
  be on the game thread — note if addBody asserts a lock).
- The **body-creation** side: `hknpBoxShape` ctor `0x1878cf0`, `CSPhysIns` ctor `0xc66df0` / DLRF factory
  `0xc66ea0` — what a minimal box body needs (shape → motion/body cinfo → addBody), so `add_collision
  <half-extents> <x y z>` has the full recipe. Cross-ref `windows_terrain_heightfield_write_re_findings.md`.

**Deliverable:** the `addBody` (+`removeBody`) slot index, its signature, and the minimal box-body build
sequence. That unblocks the Linux `add_collision` smoke test (spawn a box near the player → `hf_probe` the
top → walk into it), which ALSO settles whether addBody works off the present-tick dispatch or needs a
game-thread hook (SetWorldMatrix/move already run hook-free from present; addBody is the open question).
