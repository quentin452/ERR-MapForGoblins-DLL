# RE findings — terrain / Havok collision WRITE path (STATIC, in progress)

Answers `docs/re/windows_terrain_heightfield_write_re_prompt.md`. Static Ghidra on `D:\ghidra_proj2\ER`
(App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`, read-only) + the RTTI index
`D:\ghidra_scripts\rtti_index.txt` (9760 classes). Builds on the read-only raycast
(`windows_terrain_raycast_heightfield_re_findings.md`) and the MSB-move setter lesson
(`windows_msb_placement_write_re_findings.md`). **Status: static class inventory + `CSPhysWorld`/`hknpWorld`
wiring DONE; the decisive "which shape is the ground" + the exact `hknpWorld::addBody` slot need a LIVE
Proton check (§Live checklist).**

---

## 0. TL;DR — the write is Route B (add a body), not Route A (deform)
ELDEN RING's physics is Havok **`hknp`** wrapped by **`CS::CSPhysWorld`** (the singleton the raycast already
uses, `DAT_143d76060`). The RTTI index shows the engine carries the FULL `hknp` shape + body toolkit:
- **Both** an editable-family **`hknpHeightFieldShape`** AND a baked **`hknpCompressedMeshShape`** exist. ER
  map/terrain collision is (per the modding-format convention: `hkxpwv`/compressed `.hkx`) a
  **compressed mesh = baked triangle soup = immutable in place** → **Route A (deform the ground) is a
  near-certain DEAD END**, exactly as the prompt predicted. Confirm live by reading a real ground-hit body's
  shape vtable and comparing to the table in §1.
- The realistic write is **Route B: construct a simple shape (Box/Sphere/Convex) + a body and add it to the
  live world** — the collision analog of the MSB "add a Dynamic geom instance". The engine has a CS wrapper
  for exactly this (**`CSPhysIns@CS`**, a per-object physics instance) plus **DLRF runtime-class factories**
  for `CSPhysIns`/`CSPhysSysIns`. So "add collision" reuses the same shape-of-work as the geom spawn: build
  the pieces, call the engine's own ctor/registrar, don't poke raw bytes.

---

## 1. Class inventory (RTTI index — er-relative vtable RVAs, this build)
The decisive live test needs these: cast a down-ray, read the hit body → shape ptr → **shape vtable**, and
compare to this table to learn what the ground actually is.

### `hknp` shapes (vtable RVA)
```
hknpShape            (base)   0x2eccaf0     ctors 0x187a1c0,0x1875dc0,0x18740e0,0x186f4d0,0x186c420,0x186b100
hknpCompositeShape            0x2ee0538     (base of mesh/heightfield)
hknpCompressedMeshShape       0x2eeb908  ← BAKED terrain candidate (immutable)   ctors 0x1879250,0x1866bf0,0x19bdf80,0x190a330,0x190a240
hknpHeightFieldShape          0x2ee2a18  ← editable-family (mutable candidate)   ctors 0x18798c0,0x18740e0,0x1910b50
hknpExternMeshShape           0x2eec388
hknpLodMeshShape              0x2eec280
hknpBoxShape                  0x2eec698  ← Route B simple shape   ctors 0x1878cf0,0x1916c30
hknpSphereShape               0x2eec9b0  ← Route B simple shape   ctors 0x187a220,0x1883cc0
hknpCapsuleShape              0x2eec7a0
hknpCylinderShape             0x2eec8a8
hknpConvexShape               0x2eccbf0
hknpConvexPolytopeShape       0x2eec488
hknpTriangleShape             0x2eec590
```

### World / body / CS-physics wrappers
```
hknpWorld            (Havok)  0x2eedc78     ctors 0x18a8030,0x18a6760
hknpShapeManager             0x2ef1358     ctors 0x192bf20,0x192bdd0   ← carries MutableShapeInfo + "shape changed" hkSignal (mutation-notify lead)
CSPhysWorld@CS               0x2b93888     ctors 0xc6f9a0,0xc6f120     ← the singleton wrapper (raycast ctx = *(DAT_143d76060+0x98))
CSPhysIns@CS                 0x2b92b18     ctors 0xc66df0,0xc66e50,0xc66dc0  ← per-object physics INSTANCE (Route B vehicle)
CSPhysSysIns@CS              0x2b92eb8     ctors 0xc67d90,0xc67a20
CSHavokManImp@CS             0x2b90438     ctor  0xc4e9a0
CSPhysicsAppendData@CS       0x2b92a88     ctors 0xc66830,0xc66730     ← "append physics data" — a lead for adding collision
DLRuntimeClassImpl<CSPhysIns@CS,0>         ctor 0xc66ea0   ← DLRF reflection factory (spawn a CSPhysIns)
DLRuntimeClassImpl<CSPhysSysIns@CS,0>      ctor 0xc69010   ← DLRF reflection factory (spawn a CSPhysSysIns)
```
`CSHavokBufferCapacity@CS` (vtable 0x2b91fd8) binds `hknpWorld` to the `hknpBodyManager` /
`hknpMotionManager` / `hknpConstraintManager` — so the CS layer owns the Havok body/motion/constraint
managers; the add-body call will route through those from `CSPhysWorld`.

### Mutation-notify mechanism (Route A refresh, if ever viable)
`hknpShapeManager` holds a **`MutableShapeInfo`** slot and fires **`hkSignal1<const hknpShape*>` /
`hkSignal2<const hknpShape*, u8>`** — Havok's "this shape changed, refresh the broadphase/caches" mechanism.
If a shape IS made mutable, this is the notify to raise after editing it. (Its presence is why Route A can't
be dismissed 100% on static alone — but the terrain being a compressed mesh makes it moot for the ground.)

---

## 2. Decompilation (query.java)
NB the RTTI index `ctor_rvas` column mixes ctors + the scalar-deleting-DESTRUCTOR (both reference the
vtable). Run 1's three RVAs turned out to be **destructors** — still useful for struct layout:

- **`CSPhysWorld` dtor `FUN_140c6f9a0`** — stamps `CS::CSPhysWorld::vftable`, then tears down members:
  `+0x08`/`+0x10` (`param_1[1]/[2]`, two owned objects via `FUN_141680fa0`), `+0x18` (`param_1[3]`, an object
  with a virtual dtor at its **vtable+0x68**), a **vector at `+0x20..0x30`** (`param_1[4..6]` = begin/end/cap,
  cleared together), and `+0x68` (`param_1[0xd]`, an owned object destroyed via its vtable+8 deleting-dtor).
  So `CSPhysWorld` layout ≈ `{ vtable, obj@+8, obj@+10, subsys@+18, vec@+20..30, …, owned@+68 }`.
- **`CSPhysIns` base ctor `FUN_140c66df0`** — 11 bytes, just `*self = CS::CSPhysIns::vftable`. The REAL
  construction is in its callers `FUN_140c5a2d0`, `FUN_140c67d90` (the `CSPhysSysIns` ctor region),
  `FUN_140c771b0` — these build + register the instance (Route B's actual vehicle; §Next decompiles them).
- **`hknpShapeManager` dtor `FUN_14192bf20`** — confirms the manager holds **two hkArrays**: a
  **`MutableShapeInfo[]`** at `param_1[3]` (entries **0x10 bytes**, `-1` sentinel in slot0, count@`+0x24`,
  capacity mask `& 0x3fffffff`) and a second shape-ptr array at `param_1[5]` (count@`+0x20`/`param_1[6]`,
  freed 8-byte entries), plus a `CRITICAL_SECTION` at `param_1[9]`. Frees via the Havok TLS allocator
  (`TlsGetValue(DAT_1447dacd0)` → alloc@`+0x58`, sizes 0x30/0x28). So mutable-shape registration = push a
  0x10-byte `MutableShapeInfo` into the `+0x18` array under the crit-section — the register-a-mutable-shape
  entry is the counterpart to look for IF Route A is ever pursued.

Run 2 (below) targets the engine's OWN add-collision-to-world path + the real ctors.

**Run 2 — the real `CSPhysWorld` ctor `FUN_140c6f120` (er+0xc6f120), fully mapped:**
- **Allocates + constructs the `hknpWorld`** (0xb70 bytes via the Havok TLS allocator) with
  **`FUN_1418a6760` = `hknpWorld::ctor`** (`er+0x18a6760`, matches the index's hknpWorld ctor) and stores it
  at **`param_1[1]` = `CSPhysWorld+0x08`**. ⇒ **confirms the raycast's `ctx+8 = hknpWorld`**: `ctx` (the
  raycast's `*(DAT_143d76060+0x98)`) IS the `CSPhysWorld` instance, and `+0x08` is the live `hknpWorld*`.
- Registers the world's event-signal handlers via **`FUN_1418ae7d0(hknpWorld, eventId, 0xffffff)`** →
  `FUN_140c6efd0(slot, handler, name)`. Named events: `1`=`gr.OnManifoldEvent`, `2`=`gr.OnContactImpulse`,
  `3`=`gr.OnContactImpulseClipped`, `0x1a`=`gr.OnConstraintForce`, `0x1b`=`gr.OnConstraintForceExceeded`,
  `0xd`=`gr.freezeBodiesWhichLeftTheBroadPhase`. (`FUN_1418ae7d0` = get an event slot on the world — a handle
  for hooking body events if the add-body path ever needs a "body added/left broadphase" notify.)
- Builds a **shape-tag codec** chain at `param_1[2]` (`CSPhysWorld+0x10`): `hkReferencedObject` →
  `hknpShapeTagCodec` → `hknpMaterialPaletteShapeTagCodec` → **`hknpUFMShapeTagCodec<3,5,8>`**, then
  `FUN_14187dc50(hknpWorld, codec)` installs it. The **UFM `<3,5,8>`** = the bit layout that packs
  material/collision-filter into a shape tag — this is the encoding behind the raycast's `0x5e` filter byte;
  a Route-B body's shape must carry a compatible tag to be hit by the same filter.
- `param_1[0xd]` (`+0x68`) = a **`hkTaskGraph`** (0x208 bytes). `param_1[3..6]` (`+0x18..0x30`) = an
  allocator-checked container (`DAT_143d87310` = the heap the `>>5&1` "incompatible heap" assert guards).

**`CSPhysWorld` layout (confirmed):** `{ vtable, hknpWorld*@+0x08, shapeTagCodec@+0x10, container@+0x18..0x30,
 …, hkTaskGraph*@+0x68 }`. **`CSPhysIns` ctor `FUN_140c66e50`** = stamps `CS::CSPhysIns::vftable` (+ optional
`free(self,0x60)` on the deleting-dtor bit) → CSPhysIns is a **small ~0x60-byte wrapper**.

`FUN_1406c9020` (the geom "phys registration" from the MSB findings) is NOT the body-add — it's an
FD4Singleton (`DAT_143d69ba8`) access that fills **4 handles** into geom `+0x1f0..0x208` keyed by geom
`+0x164` (LOD/collision-set fetch), called from the geom ctor `FUN_1406c5900`. The clean `hknpWorld::addBody`
was not reached statically here; find it live (dump the `hknpWorld` vtable @ `0x2eedc78` from the live
`CSPhysWorld+0x08`, or find-what-accesses on a body pointer as a geom streams in).

---

## 3. Route B — the plan (add a dynamic collision volume)
1. **Get the world.** `ctx = *(DAT_143d76060 + 0x98)` (already resolved for the raycast); `ctx+8` = the
   `hknpWorld` holder. `CSPhysWorld@CS` (vtable 0x2b93888) is the wrapper — find its **add-body / append**
   vmethod (or drive `CSPhysicsAppendData`), the collision analog of `CSWorldGeomIns`'s `SetWorldMatrix`.
2. **Build a simple shape.** `hknpBoxShape` (ctor 0x1878cf0) or `hknpSphereShape` (ctor 0x187a220) — small,
   self-contained, no baked data. Half-extents / radius + a transform.
3. **Build + add a body.** Via `CSPhysIns@CS` (ctor 0xc66df0) or the `DLRuntimeClassImpl<CSPhysIns>` factory
   (0xc66ea0) → register into the world's body manager. The ctor should do the broadphase insert (like the
   geom ctor self-registers).
4. **Verify with the raycast we already have** — cast a down-ray at the box and confirm a hit at the box top
   (§Live checklist). This closes the loop with ZERO new read RE.

Reuses everything the MSB-move/spawn work established (game-thread call, SEH guard, byte-diff, DLRF factory
pattern). Expected first shippable primitive: **"place a collision box/platform at a transform."**

---

## 4. Live checklist (hand to the Linux/Proton agent — Windows can't verify, its DLL is stale)
1. **Decide Route A vs B (the one measurement that matters).** Cast the existing down-ray (`hf_probe`) at the
   player, take the returned **hit body** (RE §2 of the raycast findings says the shape-cast variant yields
   it), walk body → shape ptr → **read the shape's vtable**, subtract imagebase, and match against §1:
   `0x2eeb908` = `hknpCompressedMeshShape` (baked → Route B only) vs `0x2ee2a18` = `hknpHeightFieldShape`
   (editable → Route A worth a probe). **This single read decides the whole track.**
2. **Route B smoke test.** Behind a new dev RPC (`add_collision <dx dy dz>`): build an `hknpBoxShape`, make a
   `CSPhysIns` (or DLRF-spawn it), add to the world near the player on the GAME thread, then `hf_probe` on the
   box footprint → expect a hit at box-top Y. Byte-diff nothing needed; the raycast is the oracle.
3. **Coordinate frame.** The raycast is Havok **block-local**, not world frame (D2.1, `bbc705f`) — build the
   box transform in the same block-local frame the raycast uses, or it lands in the wrong chunk.
4. **Thread + persistence.** Game thread only (scene/world writes race the step). Check the body survives a
   few seconds + a tile re-stream (does the streamer purge non-native bodies?); note if it must be re-added
   per region load.

## 5. Anchors (er-relative, imagebase 0x140000000, this ERR build)
```
CSPhysWorld singleton   DAT_143d76060 (er+0x3d76060)   ctx = *(+0x98) = CSPhysWorld;  ctx+0x08 = hknpWorld*  [confirmed via the ctor]
CSPhysWorld ctor        FUN_140c6f120 (er+0xc6f120)    {hknpWorld*@+8, shapeTagCodec@+10, container@+18..30, hkTaskGraph*@+68}
CSPhysWorld RTTI/vtable  CSPhysWorld@CS@@  vtable 0x2b93888
hknpWorld ctor          FUN_1418a6760 (er+0x18a6760)   0xb70 bytes;  vtable 0x2eedc78
world event-slot getter FUN_1418ae7d0 (world, eventId, mask)  [0xd = freezeBodiesWhichLeftTheBroadPhase, 1 = OnManifoldEvent…]
shape-tag codec         hknpUFMShapeTagCodec<3,5,8>  installed via FUN_14187dc50(world, codec)  [material/filter bit packing behind 0x5e]
raycast (oracle)        FUN_140c70360 (filter 0x5e, down-ray) -> hit + body   [read-only, already shipped]
terrain shape (baked)   hknpCompressedMeshShape vtable 0x2eeb908   [confirm live it's the ground]
editable shape          hknpHeightFieldShape    vtable 0x2ee2a18
Route B shapes          hknpBoxShape 0x2eec698 (ctor 0x1878cf0); hknpSphereShape 0x2eec9b0 (ctor 0x187a220)
Route B body vehicle    CSPhysIns@CS vtable 0x2b92b18 (~0x60B wrapper, ctors 0xc66df0/0xc66e50); DLRF factory 0xc66ea0
append-data lead        CSPhysicsAppendData@CS vtable 0x2b92a88 (ctors 0xc66830,0xc66730)
mutation notify         hknpShapeManager vtable 0x2ef1358 + MutableShapeInfo[] (0x10B entries @ mgr+0x18, count@+0x24) + hkSignal
```

## 6. Next
- **Static (optional, Windows):** find `hknpWorld::addBody` — dump the `hknpWorld` vtable (`0x2eedc78`) and
  decompile the geom-stream body add (the collision insert inside/after the geom ctor `FUN_1406c5900`, since
  `FUN_1406c9020` was only the LOD-handle fetch). Or trace a caller of `FUN_1418a6760` neighbourhood. This is
  the one missing call for a fully static Route-B recipe; but it is EASIER live (next bullet).
- **Live (Linux/Proton — the decisive steps):** (a) the shape-vtable read in §4.1 → decides Route A vs B;
  (b) dump the live `hknpWorld` vtable from `CSPhysWorld+0x08` to locate `addBody`/`removeBody`; (c) the
  `add_collision <dx dy dz>` box smoke test in §4.2, verified with the existing `hf_probe` raycast.
- **If Route A survives** (ground is an `hknpHeightFieldShape`, not a compressed mesh): a separate brief for
  the `hknpHeightFieldShape` sample-set write + the `hknpShapeManager` `MutableShapeInfo` register/notify
  (mgr+0x18 array, 0x10B entries) — but expect a baked compressed mesh, i.e. Route B.
