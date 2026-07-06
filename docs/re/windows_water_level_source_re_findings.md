# RE FINDINGS — per-region water-level source (Options 3 AND 2 both dead → the real path is material-tag / MSB)

Answers `windows_water_level_source_re_prompt.md`. Static Ghidra headless on `D:\ghidra_proj2\ER`
(`eldenring.exe` 2.6.2.0 / imagebase `0x140000000`, `-noanalysis`), 2026-07-06. Scripts:
`D:\ghidra_scripts_mfg\mfg_water.java` + `mfg_wfilter.java` (outputs `out_water.txt` / `out_wfilter.txt`).

**Both prompt options investigated. Verdict:**
- **Option 3 (sample `GXWaterHeightMap@GXSR`) — DEAD END:** a GPU render-resource for the water *interaction*
  (ripple/wave) sim, not a CPU per-(x,z) base-plane (§1–§4).
- **Option 2 (a water-surface cast filter) — ALSO DEAD END:** ER has **no water-surface collision**. Water is
  a ground **MATERIAL** (Default/Grass/Water/Swamp), not a collision layer/surface, so no cast filter can
  return a water surface Y (§9).
- **The real, CPU-native path (recommended): a MATERIAL-based sea-tag** — cast `0x5e` (already done), read the
  hit triangle's collision material, tag `sea = material ∈ {Water, Swamp}`. Correct per-region water *mask*
  (blue tint at seabed Y — what the tag needs), mod-agnostic. Remaining RE scoped in §9. Fallback = MSB
  water-plane Y.

---

## 0. TL;DR
- `GXWaterInteractionManager@GXSR` is **not a standalone FD4Singleton** — it is a **member sub-object of
  `GXSceneContext`** (the GXSR render scene context) at **`GXSceneContext + 0xBE20`**. It is reached through
  the render scene, not a static instance slot.
- `GXWaterHeightMap@GXSR` (0x160 bytes) is a **GPU-texture container**: its base holds **7 ref-counted GPU
  resources** (height/normal/displacement render targets for the wave sim), released via the DLGraphics
  device-resource path. **No CPU float height array, no origin/bounds/cell-size, no `sample(x,z)` method.**
- It models the **dynamic water *interaction*** (player/wind ripples, wave displacement) tuned by
  `GXWaterInteractionParam@GXBS` (`WaveSpeed`), **not the flat base water surface Y** the sea-tag needs.
- `WaterDepth` (the one exe string that looked like a lead) is an **AUDIO RTPC**, not a water level (see §3).
- **No gameplay-side water-height query exists** in the exe: `Underwater`/`InWater`/`WaterField`/`DeepWater`/
  `WaterVolume`/`SwimTop` → **zero** string hits. ER has no swimming; deep water is MSB kill-volumes.
- ⇒ **Option 3 cannot feed the sea-tag** without a per-frame GPU readback that would still return ripple
  displacement, not the base plane. **Do Option 2 instead** (or read the per-region MSB water plane Y).

---

## 1. The manager is embedded in GXSceneContext (not a singleton)

`GXWaterInteractionManager@GXSR` — vt `er+0x2f0bd98`, ctor `FUN_141a0cb30` (er+0x1a0cb30), dtor
`FUN_141a0cd20` (er+0x1a0cd20). Callers of the ctor/dtor place it inside `GXSceneContext`:

- **Ctor site** = the `GXSceneContext` ctor `FUN_141a17e10` (er+0x1a17e10): it constructs the manager at
  `sceneCtx + 0x17c4` qwords → **`GXSceneContext + 0xBE20`** (`FUN_141a0cb30(param_1 + 0x17c4, param_2)`).
- **Dtor site** confirms the same offset: `Unwind_1429201a2` calls
  `FUN_141a0cd20(*(sceneObj + 0x80) + 0xbe20)`.

So to reach the live manager you resolve the `GXSceneContext` render singleton, then `+0xBE20`. (Not pursued
further — see §5; the object it owns is the wrong data regardless of reachability.)

**Manager fields set in the ctor** (all *interaction* tuning, no world-space water Y):
```
+0x08,+0x10  allocator/heap ptr (container bases)          +0x18..0x48  container #1 (vector, 0-init)
+0x54  float 0x3ca3d70a = 0.02   (decay/epsilon)           +0x58  int 0x800 = 2048
+0x60  allocator ptr             +0x68..0x80  container #2  +0x88  sub-obj (FUN_141ed6150)
+0xC0  float 3.5   +0xC8 int 4   +0xD0 float 1/30 (0.0333)  +0xE0  float 5.0   +0xE8 heap alloc (freed in dtor)
```
None of these is a per-region surface height; they are ripple-sim constants (radius/decay/timestep).

## 2. GXWaterHeightMap = a GPU-resource container (the decisive fact)

`GXWaterHeightMap@GXSR` — vt `er+0x30370b0`. The RTTI-index "ctor" `0x1b78960` is actually the **deleting
destructor** (writes vftable, then `if (flag&1) operator delete(this, 0x160)` → object size **0x160**). It
calls the base destructor **`FUN_141b79810`** (er+0x1b79810), whose body frees **seven ref-counted GPU
resources**:
```
base+0x18, +0x30, +0xe0, +0xe8, +0xf0, +0xf8, +0x100   → each: FUN_141af8630(ptr,1)  (DLGraphics deferred
   device-resource release) + DLReferenceCountObject::Unref + allocator free (vtbl+0x68);  base+0x124 = int
```
This is the canonical **DLGraphics GPU-texture release** pattern. `GXWaterHeightMap` therefore holds a set of
GPU textures (height + normal + displacement + sim buffers), **not** a CPU height grid. `GXWaterNormalMap`
(base dtor `FUN_141b78530`) is the same shape (GPU resources at +0x8, +0x20). These classes are **GX-reflected
types** (a UTF-16 class-name sits immediately after a 1-slot vtable), instantiated data-driven by the GX
resource/serialization system on region load — a render subsystem, no direct symbolic ctor call site.

**Consequence:** there is no `sample(x,z)→height`. Even a GPU readback of these textures yields the *ripple
displacement field* of the wave sim, not the region's flat base water level. Option 3, as written, is aimed at
the wrong structure.

## 3. `WaterDepth` is an audio RTPC (the lead that looked real, wasn't)

The only `WaterDepth` string (er+0x2bc52f8) sits in an **audio/reverb runtime-parameter name table**, not a
water param. The full cluster (er+0x2bc5280…):
```
TimeOfDay, WindStrength, ReverbType, RoomType, ListenerIndoorRate, OutdoorIndoor, WeatherType, WaterDepth,
PlayerMoveSpeed, PlayerIsRiding, PlayerCrouching, IsBuddy, IsChrSilence, IsPlayingCutScene, AvgDistForOutdoor,
… MaterialHardness, UsingAutoReverb, DistanceFromDummyPoint_00..04, PlayerPosHeight_Legacy,
PlayerPosHeight_Open, IsMenuDisplayedForVoice, IsPause, IsPlayingMovie
```
These are Wwise-style RTPC inputs. `WaterDepth` = how deep the player is wading (to muffle sound);
`PlayerPosHeight_Legacy`/`_Open` = player height for audio, **not** a per-map water height (the prompt's
`Height_Legacy`/`Height_Open` guess was this cluster, misread). Its lone xref (er+0x3b3edd8) is a **data**
reference (the name table), no code accessor. Dead end for a water surface Y.

## 4. No gameplay water-height query exists
`Underwater`, `InWater`, `WaterField`, `DeepWater`, `WaterVolume`, `SwimTop` → **0 hits** each. Consistent with
ER having no swim system; "deep water" is instant-death via MSB hazard/kill volumes, so there is no
CPU "water height at (x,z)" oracle to borrow. The base water surface exists only as **geometry** (a water-plane
FLVER/collision per water body in the MSB), region-local — not a global queryable field.

## 5. (Was: "pivot to Option 2".) Option 2 was then investigated too — see §9. It is ALSO a dead end (no water
collision surface). The real path is the material-tag in §9, or the MSB water-plane fallback below.

- **Per-region MSB water plane Y (fallback).** Water bodies are MSB parts (a flat render plane); their
  placement Y is readable through the disk-MSB path MFG already has (`windows_runtime_msb_resident`). Needs a
  naming/material convention to pick the water parts — heavier and gives a Y, not a per-cell mask.

If someone still wants the GPU interaction field (e.g. for wave-animated water rendering, not the sea-tag), the
anchors in §1–§2 are the entry: `GXSceneContext + 0xBE20` → the manager → its GPU textures, plus a D3D readback.
That is a much larger, render-thread effort and does not serve the sea-tag.

## 6. Code impact
`goblin_heightfield.cpp` keeps `kSeaLevelY` as a **dormant sentinel** (unchanged) — neither option yielded a
per-cell water source to wire yet. The `c.sea = groundY < kSeaLevelY` line stays inert until the §9 material-tag
lands, at which point it becomes `c.sea = (hitMaterial ∈ {Water, Swamp})`. The render branch
(`panel_virtual_map.cpp:1545`, `c.sea` → water-blue) is already wired and unchanged.

## 7. Anchors (this build, er-relative)
```
GXWaterInteractionManager   vt er+0x2f0bd98  ctor FUN_141a0cb30  dtor FUN_141a0cd20
  owner / reach             sub-object of GXSceneContext @ +0xBE20  (GXSceneContext ctor FUN_141a17e10)
GXWaterHeightMap            vt er+0x30370b0  (0x160 bytes)  deleting-dtor FUN_141b78960  base-dtor FUN_141b79810
  nature                    holds ~7 ref-counted GPU resources (DLGraphics) — NOT a CPU height array
GXWaterNormalMap            vt er+0x3036fd8  base-dtor FUN_141b78530  (GPU resources)
GXWaterInteractionParam     vt er+0x30583b0  ctor FUN_141c89850  (0x70, DLNonCopyable; wave-sim tuning)
WaterDepth (audio RTPC)     str er+0x2bc52f8  (reverb/RTPC name table, NOT water level)  data-xref er+0x3b3edd8
gameplay water query        NONE (Underwater/InWater/WaterField/DeepWater/WaterVolume/SwimTop = 0 hits)
```

## 9. Option 2 investigated — the collision filter is a 128-layer matrix; WATER IS NOT A COLLISION SURFACE

Decoded ER's collision filter to test the prompt's premise ("a filter value hits the water surface").

**The filter mechanism (fully mapped).** ER's filter = **`CSCollisionFilter@CS`** (vt `er+0x2b91d00`, ctor
`FUN_140c5d730`), a subclass of `hknpCollisionFilter`. It holds a **128×128-bit layer-adjacency matrix** at
`this+0x20` (0x800 bytes), populated by **`FUN_140c5dab0`** (a 12.8 KB list of `enable(ROW,COL)` calls). Its
`isCollisionEnabled` overloads (vtable slots 3–6: `FUN_140c61940`/`61d70`/`61be0`/`61ae0`) read each body's
**shape tag** at `body+0x6c`: `layer = tag & 0x7f` (7-bit, 0–127), `+ group bits (>>7 &3, >>0x19 &0x1f, bit
0x1e)`. Collision-enabled ⇔ `matrix[layerA][layerB]` bit set. **⇒ the cast "filter" byte (`0x5e`, `0x5d`,
`0x67`…) IS the query's 7-bit collision LAYER** — the cast behaves as a body on that layer and hits whatever
layers it is enabled against. (`0xe0`/`0x2000058` set high flag bits above the 7-bit layer.)

`0x5e` (the terrain filter) is enabled against layers **2, 1, 0x52, 0x54** (layer 2 = map/terrain = the
seabed). There is **no water layer** among them, or anywhere in the matrix, because **water is not a collision
layer.**

**Water is a ground MATERIAL, not a surface.** The only concrete "water" in the collision/character domain is a
**surface-material blend**: the strings `{Center,FrontRight,FrontLeft,RearRight,RearLeft}_MatRatio_{Default,
Grass,Water,Swamp}` (er+0x2bc32b8…) — a 5-contact-point (Torrent's 4 hooves + centre) material-ratio system for
footstep VFX/SFX/movement. `Water` sits alongside `Grass`/`Swamp`/`Default` as a **ground material**, with no
collision surface of its own. Combined with the zero string hits for `WaterSurface`/`WaterMesh`/`PhantomWater`/
`WaterCollision`/`Wade`/`Splash` (§4-adjacent recon): **ER water has no collision body.** The seabed terrain is
the only collision; a raycast can only ever return the seabed Y, never a water surface Y. **⇒ the literal
Option 2 (a filter that hits the water surface) is impossible.**

## 10. The real path — a MATERIAL-based sea-tag (recommended)

The sea-tag only needs to *classify* a relief cell as water (paint it blue); it does not need the exact surface
Y. Since the ground collision under water carries a **material** that can be `Water`/`Swamp`, the correct
mod-agnostic, CPU-native tag is:

> per cell: cast `0x5e` (already done in `goblin_heightfield.cpp`) → resolve the **hit triangle's collision
> material** → `c.sea = (material ∈ {Water, Swamp})`. Blue tint is drawn at the seabed Y (fine for a 2D relief).

**Remaining RE for this path (a fresh, well-scoped sub-task):**
1. **Extract the material from a raycast hit.** `FUN_140c70360` already returns the hit body/collidable
   (out-params `param_5`/`param_6` carry the swizzled collidable info; §2 of
   `windows_terrain_raycast_heightfield_re_findings.md` notes the AEG variant recovers the hit body + a
   body/material id). Resolve hit → `hknpShape`/shape-key → **`hknpMaterialLibrary`** (er+0x2ee36b0, per
   `far_terrain_heightmap_re_findings.md` §5b) → the material id.
2. **The `Water`/`Swamp` material id values** (the enum ordinals) — from the `MatRatio` material enum or the
   material library. Validate live over a known lake vs dry land.

This is the honest, correct sea-tag source and it reuses the existing single cast (no second cast, no GPU, no
manager). The MSB water-plane Y (§5) remains the fallback if a true surface *height* is ever needed.

## 11. Anchors — Option 2 / collision filter (this build, er-relative)
```
CSCollisionFilter@CS        vt er+0x2b91d00  ctor FUN_140c5d730 (er+0xc5d730) / FUN_140c5d890
  layer matrix              this+0x20, 128x128 bits (0x800B); populate FUN_140c5dab0 (er+0xc5dab0)
  isCollisionEnabled        vtable slots 3-6: FUN_140c61940 / 61d70 / 61be0 / 61ae0
  body shape tag            body+0x6c;  layer = tag & 0x7f (7-bit, 0..127); groups >>7&3, >>0x19&0x1f, bit 0x1e
  filter byte == query layer  0x5e→layer 94 (terrain, vs layers 2/1/0x52/0x54); 0x5d/0x67 = AEG; 0xe0/0x2000058 flagged
water is a MATERIAL          {Center,FR,FL,RR,RL}_MatRatio_{Default,Grass,Water,Swamp}  str er+0x2bc32b8..0x2bc34b8
  material library           hknpMaterialLibrary er+0x2ee36b0  (raycast hit -> shape-key -> material id)
NO water collision surface   WaterSurface/WaterMesh/PhantomWater/WaterCollision/Wade/Splash = 0 string hits
```

## 8. Deliverable status
**Options 3 AND 2 both investigated and ruled out (static): DONE.** Option 3 = GPU wave-sim resource (§1–§4);
Option 2 = no water collision surface, water is a ground material (§9). **Recommended path handed off: the
material-based sea-tag** (§10) — cast `0x5e` → hit material ∈ {Water, Swamp}; remaining RE = raycast-hit
material extraction + the Water/Swamp material ids (`hknpMaterialLibrary` er+0x2ee36b0). Fallback = MSB
water-plane Y (§5).
