# ADD a new geom placement — Windows/Ghidra RE prompt (route 1: spawn_clone)

**Goal:** drive `CSWorldGeomDynamicIns` spawn (`FUN_1406b9880`) live to ADD one new world-geom instance
(the last MSB-write hole — see `windows_msb_placement_write_re_findings.md`). MOVE is fully solved+live
(vtable[0xd0] setter, `move_asset` 7/7). ADD is blocked because the ctor's args must be built by two
helpers whose formats aren't decompiled. **This prompt = decomp those + the ctor's record reads, so a
`spawn_clone` RPC can be driven from Linux/Proton without crashing.** Ghidra project `D:\ghidra_proj2\ER`,
imagebase `0x140000000`, `query.java`.

## The spawn shape (from the driver `FUN_1406a7930`, already scoped)
```c
FUN_14062e700(&srcTypeDesc, partType, BlockData, idx);                 // (1) build srcType descriptor
thunk_FUN_144cbdae7(&transform, BlockData, partsList, *(BlockData+8+0x58)); // (2) build world transform
inst = *(BlockData+0x2c0) + count*0x5b0;                               // dynamic pool slot (placement-new)
FUN_1406b9880(inst, &srcTypeDesc, BlockData, &transform);             // (3) the Dynamic ctor
// then push `inst` into BlockData geom_ins vector at +0x288 (begin/end/cap +0x290/+0x298/+0x2a0)
```

## What to decompile (the blockers)
1. **`FUN_14062e700`** (srcType descriptor build): full signature + the **byte size + field layout of
   `srcTypeDesc`** (a stack local of unknown size — need it to allocate one). What does it read from the
   part at `idx`? Can it be built from an EXISTING loaded part (so its model is resident → renders)?
2. **`thunk_FUN_144cbdae7`** (transform build) and its target: signature + the **byte size + format of
   `transform`** (`local_1e8`). Is it a 4x4 world matrix (same as the instance's `+0x220` cache we
   already read/write) or a different pose struct? If it's the same matrix, we can skip this helper and
   pass a copied+offset `+0x220` matrix directly.
3. **`FUN_1406b9880`** (Dynamic ctor): exactly which fields it reads from `srcTypeDesc`, `BlockData`, and
   `transform`; and **which fields of the parts record** (`inst+0x10`) the base ctor `FUN_1406c5900`
   dereferences (model refs, the `rec+0x124`/`rec+0x18b` the earlier note guessed). Does it self-register
   into WGM/render/physics (finding says yes via `FUN_140b32880`/`FUN_1406c9020`), or does the driver?
4. **The `+0x288` push**: is a plain vector-push of `inst` enough for render+collision, or is a **pool
   index** (`BlockData+0x49c` dynamic count) assumed by other code (culling/streaming) that must also be
   bumped? (If the pool is full — capacity fixed at tile-load — where does route-1's self-allocated `inst`
   live so nothing else treats the pool as contiguous?)

## Live ground-truth (Linux recon, `geom_dump` RPC, 2026-07-03)
A loaded First-Step Dynamic instance + its parts record (RVAs are `abs - moduleBase`):
- **instance** `vt+0x2a84208` (runtime CSWorldGeomIns vtable — NB the findings doc's `0x2a84cb0` is a
  DIFFERENT vtable; reconcile which is the live class). `+0x08 = <id/tag 0x…3c141201>`, **`+0x10 =
  partsRec`, `+0x18 = partsRec` (SAME ptr!)** — so `+0x18` (the "transform module") aliases `+0x10` here;
  confirm whether `+0x18` is really a separate FD4 module or the same record. `+0x20 = 0x40389ac0`
  (float?). Move target `+0x220` = the cached world matrix (proven).
- **parts record** (`inst+0x10`): `+0x00 = 0x13c141201` (**not** a module vtable — a tag? confirm the
  record's real vtable/type), a large `0xFF…` sentinel block `+0x10..+0xc8`, then many repeated sub-object
  pointers `vt+0x45d6840` at `+0xd8,+0x100,+0x120,+0x140,+0x160,+0x190,+0x1b0,+0x1c8,+0x1e0,+0x1f8`
  (DLW-string / array headers?). So the record is **deep + pointer-rich → a shallow memcpy clone shares
  sub-objects** (risky); the ctor likely only reads a few fields — identify them so a MINIMAL synthetic
  record (or a safe reuse of the live one) suffices.
  Full hex is in the `[GEOMDUMP]` log; re-dump with the `geom_dump` RPC.

## Deliverable
The three helper signatures + `srcTypeDesc`/`transform` sizes+layouts + the ctor's required record reads,
enough to write `goblin::geom_spawn::spawn_clone(dx,dy,dz)`: reuse a live part's descriptor/record, build
(or copy) a transform offset by the delta, `FUN_1406b9880` into self-allocated `0x5b0`, push to `+0x288`,
and see a duplicated asset render+collide. Then the live probe closes the MSB-write ADD frontier.
