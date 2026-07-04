# hknpWorld::addBody — RE FINDINGS (static Ghidra) — supersedes the slot prompt

Answers `hknpworld_addbody_slot_re_prompt.md` (name the `hknpWorld` vtable `addBody` slot for Route D, the
walkable-greybox collision box). Static Ghidra on `D:\ghidra_proj2\ER` (imagebase `0x140000000`, read-only),
2026-07-05. Builds on `windows_terrain_heightfield_write_re_findings.md` (Route B) and
`custom_asset_creation_options_re_findings.md` (§D). **Windows can't run the box test — its DLL is stale;
live checklist at the end for the Linux/Proton agent.**

---

## 0. TL;DR — the premise was wrong (productively), and the recipe is now COMPLETE
- **`addBody` is NOT a vtable slot.** hknp uses a **deferred command buffer**: `addBody` is **command opcode
  1**. The vtable slots the prompt dumped (er+0x2eedc78 = genuinely `hknpWorld@@`, RTTI-confirmed) are the
  **command-dispatcher interface** — slot 7 = the command **executor** switch (`FUN_141931ed0`), slot 8 = the
  command **debug-to-string** (`FUN_1419329c0`). Naming a "heavy slot" as addBody could never have worked.
- **But we don't need the command buffer** — the engine's own CS layer calls the low-level handlers directly,
  so we can too. The full callable recipe (allocate a body, then add it):
  ```
  hknpBodyId  FUN_1418aabf0(bodyMgr, &outId, &cinfo)   // = hknpBodyManager::allocateBody  (inferred)
  void        FUN_1418a9ff0(bodyMgr, &ids, count, additionMode, activationMode)  // = addBody (HIGH conf.)
  ```
  Working template in the exe: **`FUN_1418a3080`** (creates + adds a `hknpCharacterProxy` body). CS flush
  proof: **`FUN_140c72c20`** calls `FUN_1418a9ff0(world, ids, n, 0, 0)` outside any command dispatcher.
- **Route D is now statically unblocked.** Remaining = build the box `cinfo` + resolve `bodyMgr` from the
  live `hknpWorld` (already reachable), then the live box smoke test.

---

## 1. Why it's a command, not a slot (evidence)
`er+0x2eedc78` = `hknpWorld@@` vtable (RTTI index). Decompiling the prompt's candidate slots:
- **slot 7 `FUN_141931ed0`** — a `switch(*(u16*)(cmd+4))` over ~0x47 opcodes, each dispatching to a handler
  on `*(this+0x18)` (the world's body/sim subsystem). This is `hkCommandDispatcher::execCommand`.
- **slot 8 `FUN_1419329c0`** — the SAME opcode switch, but each case builds a **debug string** (`"addBody
  Id=… additionMode=… activationMode=… lastInBatch="`, `"removeBody Id=…"`, `"setBodyShape Id=… shape="`,
  …). This is `hkCommandDispatcher::commandToString` — and it hands us the **entire opcode table** (§2).
- slots 5/10 (`FUN_1418a8d20`/`FUN_1418a9670`) = small deleting-dtors stamping `hkSecondaryCommandDispatcher`
  / `hknpModifier` vtables (secondary-base artifacts) — not operations.
`hknpWorld`'s primary base is thus a command dispatcher; public ops (addBody/removeBody/setShape/…) enqueue a
command that the executor later runs. The API-command pattern is corroborated by RTTI
(`hknpWorldApiCommandViewerEx`, plentiful `hkSignal<hknpWorld*, hknpBodyId>` member-slots).

## 2. The hknp world command opcode map (from the slot-8 debug printer — reusable)
```
0x00 bodyAllocated            0x18 setBodyMaterial              0x30 commitAddBodies
0x01 addBody                  0x19 setBodyQuality               0x31 activateBodiesInAabb
0x02 destroyBody              0x1a setBodyCollisionFilterInfo   0x32 addCollisionCaches
0x03 removeBody               0x1b updateBodyFlags              0x33 deleteAllCaches
0x04 attachBody               0x1c rebuildBodyCollisionCaches   0x34 setEventDispatcher
0x05 detachBody               0x1d updateBodyCollisionCaches    0x35 shiftWorld
0x06 setBodyTransform         0x1e setBodyActivationState       0x36 shiftBroadphase
0x07 setBodyPosition          0x1f setBodyActivationPriority    0x37 applyHardKeyFrame
0x08 setBodyOrientation       0x20 setBodyActivationControl     0x38 destroyMotions
0x09 setBodyVelocity          0x21 setMotionCenterOfMass        0x39 rebuildMotionMassProperties
0x0a setBodyLinearVelocity    0x22 setMotionInertia             0x3a motionAllocated
0x0b setBodyAngularVelocity   0x23 setBodyCollisionLookAhead    0x3b defragmentInactiveCacheStreams
0x0c applyLinearImpulse       0x24 setBodyDragProperties        0x3c bodyAllocatedAttached
0x0d applyAngularImpulse      0x25 clearBodyDragProperties      0x3d setCollisionFilter
0x0e applyPointImpulse        0x26 setGravity                   0x3e setShapeTagCodec
0x0f setPointVelocity         0x27 setAirDensity                0x3f particlesColliderAllocated
0x10 reintegrateMotion        0x28 stepCollide                  0x40 destroyParticlesCollider
0x11 setBodyMotionType        0x29 stepSolve                    0x41 addParticles
0x12 setBodyMotion            0x2a constraintAllocated          0x42 removeParticles
0x13 setBodyMass              0x2b destroyConstraints           0x43/44 enableParticles
0x14 setBodyMassDistribution  0x2c disableConstraint            0x45 rebuildParticleCollisionCaches
0x15 clearBodyMassDistribution 0x2d enableConstraint            0x46 stepAllParticlesColliders
0x16 setBodyShape             0x2e setConstraintGroup
0x17 setBodyMotionProperties  0x2f updateBroadphase
```
Executor (slot 7) opcode→handler for the ones we need: `1(addBody)→FUN_1418a9ff0`,
`3(removeBody)→FUN_1418b0630`, `6(setBodyTransform)→FUN_14188c090`, `0x16(setBodyShape)→FUN_14188bec0`,
`0x2f(updateBroadphase)`, `0x30(commitAddBodies)`.

## 3. The real callable recipe (bypass the command buffer, as CS does)
### 3a. addBody — `FUN_1418a9ff0` (er+0x18a9ff0), HIGH confidence
`void addBodies(bodyMgr /*param_1*/, hknpBodyId* ids /*param_2*/, u32 count /*param_3*/, int additionMode
/*param_4*/, int activationMode /*param_5*/)`:
- iterates `ids`, fetches each body slot `body = (id&0xffffff)*0xb0 + bodyMgr[0x28]` (**bodies are 0xb0=176 B**,
  array at `bodyMgr+0x28`, count at `bodyMgr+0x30`),
- reads the body's **shape** (via a shape vfunc `+0x20`) → computes its **AABB**, quantizes into broadphase
  int coords using the world consts at `bodyMgr+0x530..0x5c0`, stores the quantized AABB at `body+0x50/+0x58`,
- inserts into the **broadphase** via `(**(bodyMgr+0x4d8)+0x28)(broadphase, ids, count, …)`, then activates.
- `additionMode==1` shortcuts to a deferred path `FUN_1418b6850(bodyMgr+0x18, ids, count, …)`; `==0` does the
  inline add above. So the body must be **allocated (with a valid shape) BEFORE** this call.

### 3b. allocateBody — `FUN_1418aabf0` (er+0x18aabf0), inferred from the template
Signature `(bodyMgr, hknpBodyId* outId, hknpBodyCinfo* cinfo) -> &outId`. Populates a slot in `bodyMgr+0x28`
from the cinfo and returns a fresh id.

### 3c. Working template — `FUN_1418a3080` (er+0x18a3080): create + add a body from scratch
```c
FUN_141911210(&cinfo);            // init hknpBodyCinfo (defaults)
cinfo.flags |= 0x100;
cinfo.orientation = *(this+0x80..0x8c);           // quaternion
FUN_14167eb10(&cinfo.shape_or_pos, this+0x50);    // position / shape transform
FUN_141694ae0(&cinfo.name, "hknpCharacterProxy"); // debug name
id = *FUN_1418aabf0(bodyMgr=*(this+0x40), &outId, &cinfo);   // ALLOCATE
FUN_1418a9ff0(bodyMgr, &id, 1, /*additionMode*/1, /*activationMode*/0);   // ADD
// … then registers the id in a CS map (FUN_140c64740)
```
And **`FUN_140c72c20`** (a CS per-frame flush) shows the batch shape: collect pending ids, then
`FUN_1418b0630(world, rmIds, n, 0)` (removeBody) + `FUN_1418a9ff0(world, addIds, n, 0, 0)` (addBody) —
**called directly, no dispatcher** → confirms our DLL can call `FUN_1418a9ff0` on the game thread.

## 4. Route D box recipe (assembled)
1. **Shape:** `hknpBoxShape` ctor `FUN_141878cf0` (er+0x1878cf0; from terrain findings) — half-extents +
   convex radius. (Sphere alt: `FUN_14187a220`.)
2. **cinfo:** `hknpBodyCinfo cinfo; FUN_141911210(&cinfo)`; set `shape=box`, `position`+`orientation`
   (block-local frame — see live note 3), `motionType = STATIC`, `flags |= 0x100`, optional name.
3. **Allocate:** `hknpBodyId id; FUN_1418aabf0(bodyMgr, &id, &cinfo)`.
4. **Add:** `FUN_1418a9ff0(bodyMgr, &id, 1, additionMode, activationMode)` (try `0,0` like the CS flush, or
   `1,0` like the char proxy).
5. **bodyMgr** = the world's body/sim core. `hknpWorld = *(CSPhysWorld+0x08)`,
   `CSPhysWorld = *(DAT_143d76060+0x98)` (both already resolved live, commit 3adf5ef). `FUN_1418a9ff0` reads
   world-scale offsets (`+0x28` bodies, `+0x180` motions, `+0x4d8` broadphase, `+0x8c8` flag) — so `bodyMgr`
   is the `hknpWorld` itself (hknpBodyManager is embedded). **Confirm live** by checking `hknpWorld+0x28`
   points at an array of 0xb0-stride bodies and `+0x30` is a plausible count.

## 5. Live checklist (Linux/Proton — the box smoke test)
1. **Resolve bodyMgr:** from `hknpWorld` (§4.5); verify `+0x28`/`+0x30` look like the body array/count.
2. **Build + add:** box shape → cinfo (`FUN_141911210` + fields) → `FUN_1418aabf0` (alloc) →
   `FUN_1418a9ff0` (add) near the player, on the game thread (SetWorldMatrix/move already run hook-free from
   present; addBody touches the broadphase/body arrays, so if present-thread stalls, hook a per-frame step —
   `FUN_140c72c20` is the engine's own flush point to mirror).
3. **Verify with the oracle:** `hf_probe` (down-ray, filter `0x5e`) on the box footprint → expect a hit at
   box-top Y. Then walk into it.
4. **Frame:** the raycast is Havok **block-local** — build the cinfo transform in that same frame or the box
   lands in the wrong chunk (terrain findings §4.3).
5. **Modes:** try `additionMode/activationMode` = `0,0` then `1,0`; note which makes it immediately solid
   (static bodies may need `updateBroadphase` opcode `0x2f` / `commitAddBodies` `0x30` after — check if
   `FUN_1418a9ff0` already broadphase-inserts, which the decomp suggests it does).

## 6. Anchors (er-relative, imagebase 0x140000000, this ERR build)
```
hknpWorld vtable            er+0x2eedc78  (RTTI .?AVhknpWorld@@)  = command-dispatcher primary vtable
 slot 7 execCommand         FUN_141931ed0 (er+0x1931ed0)  switch(opcode) -> per-op handler on *(this+0x18)
 slot 8 commandToString     FUN_1419329c0 (er+0x19329c0)  the opcode-name table in §2
addBody (opcode 1 handler)  FUN_1418a9ff0 (er+0x18a9ff0)  (bodyMgr, hknpBodyId* ids, u32 n, int addMode, int actMode)
allocateBody (inferred)     FUN_1418aabf0 (er+0x18aabf0)  (bodyMgr, hknpBodyId* out, hknpBodyCinfo* cinfo)
cinfo init                  FUN_141911210 (er+0x1911210)
create+add template         FUN_1418a3080 (er+0x18a3080)  (hknpCharacterProxy body creation)
CS per-frame flush          FUN_140c72c20 (er+0xc72c20)   removeBody FUN_1418b0630 + addBody FUN_1418a9ff0, no dispatcher
removeBody (opcode 3)       FUN_1418b0630 (er+0x18b0630)
setBodyShape (opcode 0x16)  FUN_14188bec0 ;  setBodyTransform (0x06) FUN_14188c090
body slot stride            0xb0 (176 B) @ bodyMgr+0x28 ; count @ bodyMgr+0x30 ; broadphase @ bodyMgr+0x4d8
Route D shape               hknpBoxShape ctor er+0x1878cf0 ; hknpSphereShape ctor er+0x187a220
world resolve (live)        hknpWorld = *(CSPhysWorld+0x08); CSPhysWorld = *(DAT_143d76060+0x98)
```
Cross-ref: `windows_terrain_heightfield_write_re_findings.md`, `custom_asset_creation_options_re_findings.md`.
