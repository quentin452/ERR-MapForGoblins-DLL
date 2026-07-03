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

## Anchors
- `CSWorldGeomDynamicIns` vt er+0x2a84208 (ctors er+0x6ba0f0/0x6b9880); `CSWorldGeomIns` vt er+0x2a84cb0.
- `FUN_14062e700` er+0x62e700 (srcTypeDesc, 8B); masks er+0x3b339a0/a4/a8.
- transform builder `thunk_FUN_144cbdae7` er+0x6c3910 → er+0x144cbdae7 (garbled); `transform` = 24B FD4 pose
  wrapper; consumed by `FUN_1406c3180` (er+0x6c3180).
- lead factory `FUN_1406c7000` (er+0x6c7000) via `FUN_1406a5080` (er+0x6a5080).
