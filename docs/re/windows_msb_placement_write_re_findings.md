# MSB placement WRITE — RE findings (volet A: the load path, STATIC)

Status: **volet A RESOLVED (static, Ghidra, 2026-07-03)** — answers the crux of
`windows_msb_placement_write_re_prompt.md` §A + scopes §B4. Volets B5/C (live write test, spawn-factory
drive) remain. Imagebase `0x140000000`; tool = `query.java` on `D:\ghidra_proj2\ER`.

---

## TL;DR (the decision)
- **Writing the resident MSB `Parts.position` bytes post-load is INERT.** The MSB position is snapshotted
  **twice** before the object exists: MSB blob → `CSMsbParts` record (copies it out) → `CSWorldGeomIns`
  spawned instance (gets a *separately-computed* world transform). Editing the blob touches neither.
- **The movable/authoritative transform is on the spawned instance**, `CSWorldGeomIns+0x18` — an FD4
  *location/pose module* (nested, not a flat vec3), with a cached world matrix at `inst+0x44`.
- **`CSWorldGeomDynamicIns` exists** (`FUN_1406b9880`) — a *dynamic* world-geom instance class built on the
  same factory. That is the right vehicle for BOTH "move an existing object" and "spawn a new one".
- **Consequence for the write path:** don't poke MSB bytes; drive the **instance** — ideally via the geom's
  transform **setter** (mod-agnostic), not a raw memory write (a flat poke would desync the cached matrix
  `+0x44` + Havok collision). The live probe (§C) is re-scoped accordingly.

---

## The snapshot chain (each hop copies, none aliases the MSB)

### 1. `CSMsbParts` ctor — `FUN_140cee430` (er+0xcee430) — COPIES out of the source
```c
void CSMsbParts_ctor(u64 *self, longlong src) {
    self[0] = CS::CSMsbParts::vftable;
    self[1] = *(u64*)(src + 0x08);          // copies
    self[2] = *(u64*)(src + 0x10);
    self[3] = *(u64*)(src + 0x18);
    self[4] = *(u64*)(src + 0x20);          // ← MSB PARTS position region (+0x20) copied by VALUE
    self[5] = *(u64*)(src + 0x28);
    ...
    *(u64*)((char*)self + 0x44) = *(u64*)(src + 0x44);
}
```
The parsed part record holds its **own copy** of `src+0x20` (the PARTS `Vector3 position` per the resident
layout findings). So even the *record* no longer aliases the resident blob.

### 2. `CSWorldGeomIns` ctor — `FUN_1406c5900` (er+0x6c5900) — separate transform, retains only a record ptr
```c
CSWorldGeomIns_ctor(self, param_2, param_3 /*parts rec*/, param_4 /*transform src*/, param_5) {
    self[0]  = FieldInsBase::vftable; ... self[0] = CSWorldGeomIns::vftable;
    self[2]  = param_3;                        // +0x10 = POINTER to the CSMsbParts record (not its pos)
    FUN_1406c3180(self + 3, param_4);          // +0x18 = build the location/pose module FROM param_4
    ...
    FUN_1406c46e0(self + 3, self + 0x44);      // cache a world matrix into +0x44 from the +0x18 module
    self[0x4c] = 0x7eff0000; ...
}
```
- `+0x10` keeps a pointer to the parts record — but that record already copied the position (step 1), so
  it is **not** a live view of the MSB.
- `+0x18` (the transform) is built from **`param_4`**, a world transform passed in by the caller
  (computed from the MSB local pos + block grid at load) — **independent of the blob after construction.**

### 3. The transform module — `FUN_1406c3180` (er+0x6c3180), read via `FUN_1406c4600` (er+0x6c4600)
`FUN_1406c3180` is a construct-and-**swap** move-init of a large pose struct (position/rotation/scale +
flags; note the identity-matrix constants at `er+0x3279280/0x3279230/0x32792f0` used by `FUN_1406c46e0`).
The getter `FUN_1406c4600` does `FUN_140cedb80(inst+0x18)` → inner ptr → deref — i.e. **the transform is a
nested FD4 location module, not a flat field**. A derived world matrix is cached at `inst+0x44` (byte
`0x220`). ⇒ To move the object you set this module (setter/warp), then the cached matrix + physics follow;
a raw vec3 poke at a guessed offset would leave `+0x44`/Havok stale.

---

## §B4 — the spawn factory (scopes "ADD")
The world-geom instance factory is **`FUN_1406c5900`** (the `CSWorldGeomIns` ctor above), wrapped by:
- **`FUN_1406b9880` (er+0x6b9880) → `CSWorldGeomDynamicIns`** — the **dynamic** (movable/spawnable) variant.
- `FUN_1406db840` (er+0x6db840) → `CSWorldGeomStaticIns` — the static variant.

Signature (all variants): `ctor(self, srcType, partsRec, worldTransform, arg5)`. The parts record + world
transform are exactly what the MSB loader synthesizes per Part at tile-stream. So **"add a placement"** =
build a parts record + a world transform and call the (Dynamic) factory + register it into the geom manager
(the `DAT_143d7b0c0`/`FUN_140b32880` registration seen in the ctor). Callers to trace next:
`FUN_1406a7930` (er+0x6a7930), `FUN_1406adc80` (er+0x6adc80) — the tile-load spawn drivers.

**`CSWorldGeomDynamicIns` is the key discovery:** the engine already has a movable world-geom class, so
"move" and "add" don't require faking a static placement — spawn/relocate a dynamic instance.

---

## Re-scoped live probe (§C of the prompt)
Drop "write resident MSB bytes and expect movement" as the *primary* test — statically proven inert; keep
it only as a 1-line negative control. Instead:
1. **Move via setter (primary):** from the geom manager / FieldIns registry, get a live `CSWorldGeomIns`
   (or Dynamic) for a known asset, find its transform **setter** (vmethod on `CSWorldGeomIns`/`FieldInsBase`
   — next static step: `query.java name:CSWorldGeomIns@CS@@` to walk the vtable, look for a warp/set-pose
   slot), call it with a new world transform, observe. Expected: the object moves + collides correctly.
2. **Raw-poke controls (diagnostic):** poke the cached matrix at `inst+0x44` and, separately, the `+0x18`
   module's inner position; note whether render moves but collision doesn't (tells us how much the setter
   buys us vs. a poke).
3. **Negative control:** poke the resident MSB `Parts.position (+0x20)` → expect NO movement (confirms A1).

The one result that matters: does the setter move an existing object cleanly? If yes → shippable
World-Editor "drag-a-placement" slice, and "add" is the Dynamic-factory follow-up (§B4).

---

## ★ The transform SETTER — found (vtable-walk, 2026-07-03)
The move primitive is a **virtual method at vtable offset `0xd0` (slot 26 = `0x1a`)** on the FieldIns /
`CSWorldGeomIns` instance:
```c
void SetWorldMatrix(FieldIns *self, const float mat4x4[16]);   // vtable[0xd0]
```
Two independent callers prove it:
- **`FUN_1406c9aa0`** (er+0x6c9aa0) — a reposition helper: reads a new pos vec (`x=param_4[0]`,
  `z=param_4[2]`), fetches the current world matrix via the getter, overwrites the translation row, then
  `(**(code**)(*self + 0xd0))(self, &mat)`.
- **`FUN_1406e4210`** (er+0x6e4210) — physics/anim sync: `FUN_1406c46e0(*self+0x18,&mat)` then
  `(**(code**)(vtable + 0xd0))(self, mat)`.

**Getter (current world matrix):** `FUN_1406c46e0(inst+0x18, &out_mat4x4)` (er+0x6c46e0) — non-virtual,
builds a 4x4 from the `+0x18` location module (identity basis constants at er+0x3279280/0x3279230/0x32792f0
+ translation). Matrix is row-major 4x4; **translation in the last row** (x, y, z).

So the live **move** primitive is a direct vcall — no MSB, no raw poke:
```c
// inst = a live CSWorldGeomIns* (from the geom manager DAT_143d7b0c0[+0x10], or the FieldIns registry)
float m[16] = { 1,0,0,0,  0,1,0,0,  0,0,1,0,  newX,newY,newZ,1 };   // identity basis + new translation
(*(void(**)(void*,const float*))((*(void***)inst)[26]))(inst, m);   // vtable[0xd0], slot 26
```
Because it is the engine's own setter, it refreshes the cached matrix (`+0x44`) and drives physics/render —
exactly what a raw poke would miss. The vtable-walk also confirmed `CSWorldGeomIns::vftable` @ er+0x2a84cb0
(slots 0..5 are the FieldInsBase dtor + type getters at er+0x397a60/70/80; the vtable extends past slot 26).

## Anchors (er-relative, imagebase 0x140000000, this ERR build)
- `CSMsbParts::ctor`            `FUN_140cee430` (er+0xcee430) — copies MSB pos out (snapshot #1)
- `CSMsbPartsGeom::ctor`        `FUN_140cef4d0` (er+0xcef4d0)
- `CSMsbPartsMap::ctor`         `FUN_140cf24c0` (er+0xcf24c0)
- `CSWorldGeomIns::ctor`        `FUN_1406c5900` (er+0x6c5900) — the spawn factory; transform@+0x18, matrix@+0x44
- `CSWorldGeomDynamicIns::ctor` `FUN_1406b9880` (er+0x6b9880) — **movable** variant
- `CSWorldGeomStaticIns::ctor`  `FUN_1406db840` (er+0x6db840)
- transform module ctor         `FUN_1406c3180` (er+0x6c3180); getter `FUN_1406c4600` (er+0x6c4600);
  matrix build `FUN_1406c46e0` (er+0x6c46e0); inner-ptr accessor `FUN_140cedb80`
- geom-manager singleton         `DAT_143d7b0c0` (+0x10 = the manager; registration via `FUN_140b32880`)
- RTTI: `CSWorldGeomIns@CS@@` vtable er+0x2a84cb0; `CSWorldGeomStaticIns@CS@@` er+0x2a86860;
  `CSMsbPartsGeom@CS@@` er+0x2ba6738 (see `tools/ghidra/rtti_index.txt`).

## ★★ LIVE-VERIFIED on Proton (2026-07-03) — the setter MOVES
The `vtable[0xd0]` setter was driven live behind the **`move_asset <dx> <dy> <dz>` dev RPC**
(`goblin_geom_move.cpp`) and **works**: `tools/rpc_tests/test_move_asset.py` 7/7. Picking a live
`CSWorldGeomIns`, `move_asset 0 100 0` moved its cached world-matrix translation from Y `-16.62` →
`83.38` (exactly +100), X/Z unchanged, then a restore call put it back — **game alive through all
vcalls** (no crash; the vcall path + `__fastcall(self, const float[16])` ABI + slot 26 are correct).

Two things learned that the static read couldn't give:
- **The setter writes the CACHED world matrix at `inst+0x220`, NOT the `+0x18` source pose module.** A
  byte-diff showed the setter touched **exactly one float** — `+0x220 + 0x34` = matrix element `m[13]`
  (the Y translation) — and nothing else. So verify a move by reading `inst+0x220` (translation @
  elements 12/13/14, row-major); the getter `FUN_1406c46e0(inst+0x18,…)` rebuilds from the module and
  still shows the ORIGINAL value after a setter call (it is NOT a read-back of the setter's effect).
- **Getting a live `inst`:** the FieldIns self-register map (`[er+0x3d7b0c0]→+0x10→+0x720→map`) was
  found **EMPTY** in-world here (`root==head`). The reliable source is **`CSWorldGeomMan`** (the WGM walk
  `goblin_collected` already uses for collected-graying): `mgr→+0x18 tree→BlockData+0x288 geom_ins
  vector`. Exposed as `goblin::collected::first_live_geom_instance()`.

**Still open (not yet done):** eyeball a large move on-screen (the RPC restores immediately, and the
picked instance is arbitrary/maybe off-screen) to confirm render+collision follow the `+0x220` cache;
and whether the move survives without the `+0x18` module also being set (a frame-later engine recompute
from the module could revert it — the cache-only write may be transient). Both are follow-ups; the
**setter-moves-the-world-matrix primitive is proven.**

## Next
- ~~Static: find the transform setter vmethod~~ **DONE — vtable[0xd0].**
- ~~Live (Proton): drive it behind a dev RPC~~ **DONE — `move_asset`, 7/7 (see above).**
- **Persistence check:** hold a move (no restore) + observe over several frames — does the engine revert
  it from the `+0x18` module? If so, also set the module (or use `CSWorldGeomDynamicIns`).
- **Then "add":** drive `FUN_1406b9880` (Dynamic) from a synthesized parts rec + transform; trace
  `FUN_1406a7930`/`FUN_1406adc80` for the exact arg construction + geom-manager join.
