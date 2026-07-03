# ADD a new geom placement (spawn_clone) — Ghidra findings

Answers `windows_geom_spawn_re_prompt.md` (partial — 2 of 3 blockers solved). Ghidra `query.java` on
`D:\ghidra_proj2\ER`, imagebase `0x140000000`, 2026-07-03. Complements the Linux `geom_dump` recon in
that prompt.

## ★ Vtable reconcile (the prompt's open flag) — SOLVED
The live instance vtable `er+0x2a84208` is **`CSWorldGeomDynamicIns@CS@@`** (ctors er+0x6ba0f0, er+0x6b9880);
`er+0x2a84cb0` is its base **`CSWorldGeomIns@CS@@`**. No contradiction: `CSWorldGeomDynamicIns` derives from
`CSWorldGeomIns` (single inheritance, vtable extended), so **`vtable[0xd0] SetWorldMatrix` is inherited at
the same slot** — that is why `move_asset` works on the live (Dynamic) instances. Takeaway: **the live
movable instances the collected/move walk finds ARE `CSWorldGeomDynamicIns`** — exactly what route-1's
Dynamic ctor (`FUN_1406b9880`) produces. (Move findings doc says vt `0x2a84cb0`; that's the base class — the
live objects are the Dynamic subclass. Slot 0xd0 is identical.)

## Blocker 1 — `srcTypeDesc` build (`FUN_14062e700`) — SOLVED: it's an 8-byte packed FieldIns id
```c
undefined8* FUN_14062e700(u64* out, uint partType, u32* blockData, uint idx) {
    *out = CONCAT44(*blockData,   // HIGH 32 = *(u32*)BlockData (the block/map tag)
                    (g0 & partType) << (g1 & 0x1f) | 0x60000000 | (g2 & idx)); // LOW 32 = packed id
    return out;                    // g0=er+0x3b339a0, g1=er+0x3b339a4, g2=er+0x3b339a8 (runtime globals)
}
```
`srcTypeDesc` is **8 bytes** — a FieldIns id/handle: high32 = the block's first dword, low32 = a bitfield
tagged `0x60000000` (the geom FieldIns type tag, cf. items' `0x40000000`) mixing `partType` and `idx`. It is
**trivially buildable** at runtime (read the 3 mask globals + the block dword). For a clone, reuse the source
block's tag and a fresh `idx`. NB the driver passes `srcTypeDesc` **by value** (8 bytes on the stack), so the
ctor's `param_2` (`srcType`) is this qword, not a pointer.

## Blocker 2 — `transform` build (`thunk_FUN_144cbdae7` → er+0x144cbdae7) — PARTIAL
- The `transform` local is **24 bytes** (`local_1e8[24]`). `FUN_1406c3180` (the `+0x18` pose-module ctor,
  already in the move findings) consumes it as `param_4` via `*self = *param_2` (copies the first qword =
  a **vtable**) then a swap-init — so `transform` is an **FD4 pose *wrapper*** `{ vtable@0, … }`, NOT a raw
  4x4 matrix. **So you can't just pass the `+0x220` matrix.**
- The builder itself (`thunk_FUN_144cbdae7`, target er+0x144cbdae7) decompiles as a garbled fragment
  (`*out = arg; branch on a stack arg → FUN_1450358f6 / thunk_FUN_1401e3126`) — the target sits in a
  relocated/hot region Ghidra resolves poorly. Not cleanly readable from static alone.
- **Recommended for spawn_clone (avoid rebuilding it):** since we CLONE a live instance, reuse the *source*
  instance's own pose module rather than synthesizing `transform`. The source Dynamic instance already has a
  constructed `+0x18` module; a clone can (a) construct via the ctor with a `transform` copied from the
  source's module, or (b) skip the ctor's transform arg and post-set the clone's world matrix with the
  proven `vtable[0xd0] SetWorldMatrix` (offset by the delta) — the move primitive we already have. Route (b)
  sidesteps blocker 2 entirely: spawn at the source transform, then `SetWorldMatrix`-move it.

## Lead `FUN_1406c7000` — CHECKED, downgraded (it's an asset-name/request builder, not an instance factory)
Decompiled `FUN_1406c7000` (er+0x6c7000): it does **not** allocate/return a geom instance. It builds an
**asset resource name** `swprintf(L"%s_%04d", partName @ param_2+0x48, idx @ param_1+0x18)`, sets up an FD4
resource container (`FUN_140cf2360(_,10,0xd)`), reads a transform/pos at `param_2+0x88/0x90` and a flag at
`param_2+0x100`, and aborts on "incompatible heap." So `FUN_1406a5080`+`FUN_1406c7000` are an **asset
streaming-REQUEST** path (build the model resource name + register the request), keyed by the same 8-byte
id — NOT a leaner instance ctor. It still consumes a rich part descriptor (name@+0x48, transform@+0x88), so
it doesn't dodge the "need a valid part record" problem. **Conclusion: no shortcut here — drive the Dynamic
ctor `FUN_1406b9880` directly (blocker 3), or take route (b).** (Note: this "request an asset by name" path
is a *different* add-strategy worth remembering — ask the streaming system to load an asset rather than
placement-new an instance — but it's a bigger detour, not the spawn_clone shortcut hoped for.)

## Next (to finish the unblock)
1. Decompile **`FUN_1406b9880`** arg reads + base `FUN_1406c5900`'s **parts-record reads** (`rec+0x18b`,
   `rec+0x124`, model refs) — the minimal fields a cloned/synth record must satisfy (prompt blocker 3). This
   is now THE remaining static blocker.
2. Then the Proton `spawn_clone` probe — favor **route (b)**: build the 8-byte `srcTypeDesc` (solved),
   reuse the SOURCE instance's part record + a `transform` copied from it, `FUN_1406b9880` into a
   self-allocated `0x5b0`, push to `+0x288`, then `SetWorldMatrix` (`vtable[0xd0]`, proven) to offset it by
   the delta — sidesteps the 24-byte transform builder (blocker 2) entirely.

## ★ Blocker 3 — the ctor's `param_3` reads — SOLVED (and it corrects the prompt)
> **Live recon reconcile (see "LIVE RECON DONE" below):** `param_3` (`self+0x10`) is the **BlockData**, NOT
> a `CSMsbParts` record — the real `CSMsbPartsGeom` is embedded at `inst+0x30`. The offsets below are
> correct; read them as **`BlockData+0x18b`** and the registry on the **BlockData** (`+0xe8/+0xf8/+0xfc`).
> `spawn_clone` therefore passes the source's **BlockData** as `param_3`, not a synthesized record.

Re-decompiled the base ctor `FUN_1406c5900` + Dynamic ctor `FUN_1406b9880`, tracing every deref of
`param_3` (stored at `self+0x10 = param_1[2]`):

- **Only ONE direct field is read from the record: `rec+0x18b`** — a `char` flag (base ctor reads it twice:
  `inst[0x26c] = (rec[0x18b]==0)` and a branch on it). **That is the entire set of record *field* reads.**
- **Correction to the prompt:** the guessed `rec+0x124` / `rec+0x3b` / `rec+0x3c` / `rec+0xd` are **NOT**
  record fields — they are `param_1[4]+…`, i.e. the **transform module** (`self+0x20`, set from the 24-byte
  transform wrapper `param_4`, not from the record). No model-ref field is dereferenced off the record in
  either ctor — the model/asset is used by the render/physics registration elsewhere (already resident for
  a reused record → it renders).
- **The record is also used as an INSTANCE REGISTRY (mutated, not just read).** The Dynamic ctor calls
  `FUN_1406a6630(record, inst)` which **adds the new instance to the record's own instance list**:
  ```
  rec+0xe8 = slot array (inst ptrs)   rec+0xf0 = free-index list   rec+0xf8 = cursor (bumped)
  rec+0xfc = capacity                 rec+0xd0 = grow-check sub-object (FUN_1406ac220)
  guard: rec+0xf8 < rec+0xfc  ||  FUN_1406ac220(rec+0xd0)   (won't overflow; silently skips if full)
  ```

**Clone-safety verdict (route b, reuse the SOURCE's live record):**
- **Field-wise safe:** the ctor only needs `rec+0x18b` valid, which it trivially is on a live source record.
- **But the ctor MUTATES the shared record:** it registers the clone into the source record's instance list
  (`rec+0xe8`/`+0xf8`). So the clone becomes **tracked by the source record** and shares its lifecycle
  (on tile-unload / record destroy, the engine will iterate/free the clone too). Bounded + non-crashing (the
  capacity guard prevents overflow), and **fine for a dev/throwaway `spawn_clone` probe**. For a fully
  isolated production clone, synthesize a MINIMAL record with a valid `+0x18b` + its own registry
  (`+0xe8/+0xf0/+0xf8/+0xfc` sized ≥1) instead of reusing the source — but that is extra work; reuse-source
  is the pragmatic first probe.

**So static blocker 3 is closed:** route (b) can drive `FUN_1406b9880(inst, srcTypeDesc, source_record,
transform)` reusing the live source record (only `+0x18b` matters), accept that the clone is registered into
the source record, then `SetWorldMatrix` to offset it. No unknown record fields remain.

## ★★ Transform-independence (the Linux agent's move-init risk) — SOLVED: REBUILD, don't alias
Decompiled the move-init `FUN_1406c3180` + its sub-copy `FUN_140cef4a0` to settle how `param_4` (the
transform descriptor) is consumed:
- **`param_4` is a big pose *descriptor* (~0x188 bytes), not a 24-byte handle.** `FUN_1406c3180` (the geom's
  transform move-ctor, `self+0x18 = param_1+3`) field-by-field **swaps ~0x188 bytes** with `param_4`, seeds
  default pose constants (30.0/5.0/10.0/-1.0f), and **move-constructs an embedded `CSMsbPartsGeom` at
  `self+0x30`** from `param_4+0x18` via **`FUN_140cef4a0` = `CSMsbPartsGeom` move-ctor** (`*self+0x30 =
  CSMsbPartsGeom::vftable`, calls base move `FUN_140cecbe0`). It also swaps the heap sub-ptr `param_4[1]`
  into `self+0x20` (the "pose ptr" the live recon saw).
- **So aliasing the source is worse than feared:** a source-aliased `param_4` would (a) steal the source's
  `+0x20` heap pose ptr AND (b) **move-OUT the source's `CSMsbPartsGeom`** — gutting the live source asset.
  Deep-dup (option a) is a trap: the descriptor embeds a pointer-rich `CSMsbPartsGeom` (the earlier
  geom_dump's deep record) → a shallow copy shares sub-objects.
- **Cleanest fix — REBUILD `param_4` with the driver's own builder (no aliasing).** Both drivers build it as
  ```
  thunk_FUN_144cbdae7(&param_4 /*out, owned*/, BlockData, partsList, *(BlockData+8+0x58));
  //   partsList = *(BlockData+8+0x48)   (er+0x6c3910; body decompiles garbled but the 4-arg
  //   signature is identical at both call sites FUN_1406a7930 / FUN_1406adc80)
  ```
  Calling this ourselves from the SOURCE's BlockData yields a **fresh, owned pose descriptor** — the ctor
  then move-inits from OUR copy, leaving the source untouched. This sidesteps blocker 2 AND the corruption
  risk in one step. (Option b — a copy-ctor of the pose class — would also work but is unnecessary; option c
  — synth from a 4x4 — is dead: the builder body is unreadable statically.)
- **Remaining detail (for the probe):** the builder builds for a PART context (it reads `partsList` + an
  index). Confirm which part index it resolves for a standalone call (the driver passes a running `idx`);
  simplest is to target the SOURCE instance's own part so the descriptor matches a real, resident asset,
  then `SetWorldMatrix`-offset the clone. This is a live-probe question, not static.

**Revised spawn_clone (route b, corruption-safe):** build srcType (solved) → `thunk_FUN_144cbdae7` a FRESH
`param_4` from the source BlockData/partsList → `FUN_1406b9880(inst, srcType, source_BlockData, &param_4)`
into a self-alloc 0x5b0 → (skip `+0x288`, full) → `SetWorldMatrix` to offset. No source-aliased bytes.

## LIVE-VERIFY checklist (hand to the Linux/Proton agent — this box's loaded DLL is stale)
The Windows box runs the game with an OLDER mod DLL, so its RPC (`geom_dump`) predates these findings —
don't trust a local RPC probe. Confirm on a freshly-deployed Proton build:
1. **srcTypeDesc packing (blocker 1):** read the 3 mask globals `er+0x3b339a0/a4/a8` (RPM, game-static —
   DLL-version-independent) and a live Dynamic instance's `srcType` field, and check the low32 carries the
   `0x60000000` tag + the computed `(g0&partType)<<g1 | g2&idx` bitfield. (geom_dump already prints
   `inst+0x08`; confirm which field holds the passed-by-value srcType qword.)
2. **transform is a 24B FD4 pose wrapper (blocker 2):** dump 24B of a live instance's `+0x18` module head —
   expect a vtable ptr at [0] (not raw floats). Confirms route (b) is needed (copy source pose, not the 4x4).
3. **Blocker 3 (now STATICally solved):** the ctor only reads `rec+0x18b` (valid on any live record) and
   registers the clone into the source record's instance list (`rec+0xe8/+0xf8`, cap `rec+0xfc`). Live check
   is just a sanity dump: confirm `rec+0x18b` is a small flag and `rec+0xf8 < rec+0xfc` (registry not full)
   on the chosen source before spawning.
4. **Then the `spawn_clone` probe (route b):** srcTypeDesc(built) + source record + copied transform →
   `FUN_1406b9880` into self-alloc 0x5b0 → push `+0x288` → `SetWorldMatrix` to offset by the delta →
   see a duplicated asset render+collide.

## ★★ LIVE RECON DONE (Linux/Proton, 2026-07-03) — `spawn_probe` RPC, fresh DLL
Built a read-only recon (`goblin::geom_move::spawn_probe`, `spawn_probe` RPC + `first_live_geom_with_block`
+ `tools/rpc_tests/test_spawn_probe.py`) that dumps every ctor argument off a live Dynamic instance and
hex-windows the header. Ran on First Step (dynamic instance #0 = `m60_41_36_00-AEG004_903_1000`, a REAL
placed asset). Base resolved `0x6ffff4ba0000` (inst vt RVA `0x2a84208` = CSWorldGeomDynamicIns ✓).

**Confirmed live + the layout it corrects:**
```
inst+0x00  vtable = er+0x2a84208 (CSWorldGeomDynamicIns)
inst+0x08  srcType = 0x3c1412016ff00000   lo=0x6ff00000 (0x6xxxxxxx geom tag ✓) hi=0x3c141201 == BlockData[0] tag ✓
inst+0x10  = BlockData ptr (NOT a CSMsbParts record)   ← ctor param_3 IS the BlockData
inst+0x18  = BlockData ptr again (aliases +0x10)        ← so the transform is NOT here
inst+0x20  = 0x1bf8e9ac0  ← the REAL transform/pose module ptr (matches blocker-3 "param_1[4] = self+0x20")
inst+0x30  vtable = er+0x2ba6738 (CSMsbPartsGeom)        ← the MSB part sub-object embedded on the instance
inst+0x48  = MSB part ptr (wide name source)
inst+0x220 region = the cached world matrix (translation floats; what SetWorldMatrix/move writes)
```
masks (live): `g0=0xff g1=0x14 g2=0xfffff`. This build's dynamic srcType packs `partType=0xff, idx=0`.

**Corrections to the static plan (important):**
- **param_3 = BlockData, NOT a cloned parts record.** The Dynamic driver passes `param_1` (BlockData) as the
  3rd ctor arg; it lands at `self+0x10`. So `rec+0x18b` (the one flag) = **BlockData+0x18b** (read live = 0),
  and the `FUN_1406a6630` "instance registry" is on the **BlockData** (`+0xe8` slots / `+0xf8` cursor / `+0xfc`
  cap = 1024, cursor 0 → room). ⇒ **spawn_clone must pass the SOURCE's BlockData, not synthesize a record.**
- **The transform module is at `self+0x20`** (a heap ptr), NOT `+0x18`. Blocker-3's read-offset call was
  right. The 24-byte transform *wrapper* is the ctor INPUT that `FUN_1406c3180` move-inits into `self+0x20`.
- **The `BlockData+0x288` geom_ins vector is EXACTLY FULL** (begin=end..cap, n=41 = 9 dyn + 32 static). So
  route-1's "push into `+0x288`" **cannot append in place** — but the ctor self-registers into WGM/render
  (`FUN_140b32880`/`FUN_1406c9020`) independently, so a **first render probe can SKIP the `+0x288` push**
  (the instance renders; it's just untracked for unload = a bounded leak, fine for a throwaway). The push /
  realloc is only needed for a production-clean clone.

**The ONE remaining design risk before the ctor call:** `FUN_1406c3180` is a **move-init (swap)** — it
STEALS from `param_4`. Passing the source's own `+0x18/+0x20` bytes as `param_4` would swap the source's
pose-module ptr out → **corrupt the live source asset**. So `param_4` must own an INDEPENDENT pose module.
Options for the next iteration: (a) deep-duplicate the `self+0x20` pose object (size unknown — probe it),
(b) find a copy-ctor variant, or (c) decomp `thunk_FUN_144cbdae7` (the transform builder) enough to
synthesize a minimal wrapper from a 4x4. Until one is settled, **do NOT fire the ctor with source-aliased
bytes.** Everything else (srcType, param_3=BlockData, self-alloc 0x5b0, skip-+0x288, post-SetWorldMatrix)
is ready.

## Anchors
- `CSWorldGeomDynamicIns` vt er+0x2a84208 (ctors er+0x6ba0f0/0x6b9880); `CSWorldGeomIns` vt er+0x2a84cb0.
- `CSMsbPartsGeom` vt er+0x2ba6738 — the part sub-object embedded at **inst+0x30** (live-confirmed).
- transform/pose module ptr at **inst+0x20** (live-confirmed); BlockData ptr at inst+0x10 AND inst+0x18.
- `FUN_14062e700` er+0x62e700 (srcTypeDesc, 8B); masks er+0x3b339a0/a4/a8.
- transform builder `thunk_FUN_144cbdae7` er+0x6c3910 → er+0x144cbdae7 (garbled); `transform` = 24B FD4 pose
  wrapper; consumed by `FUN_1406c3180` (er+0x6c3180).
- lead factory `FUN_1406c7000` (er+0x6c7000) via `FUN_1406a5080` (er+0x6a5080).
