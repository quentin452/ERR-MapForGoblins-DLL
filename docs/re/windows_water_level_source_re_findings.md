# RE FINDINGS — per-region water-level source (Option 3 investigated → DEAD END; pivot to Option 2)

Answers `windows_water_level_source_re_prompt.md`. Static Ghidra headless on `D:\ghidra_proj2\ER`
(`eldenring.exe` 2.6.2.0 / imagebase `0x140000000`, `-noanalysis`), 2026-07-06. Script:
`D:\ghidra_scripts_mfg\mfg_water.java` (output `D:\ghidra_scripts\out_water.txt`).

**The user picked Option 3 (sample `GXWaterHeightMap@GXSR`). Verdict: Option 3 is the WRONG target — it is a
GPU render-resource for the water *interaction* (ripple/wave) simulation, not a CPU-sampleable per-(x,z)
base-water-plane. Recommendation: use Option 2 (a water-inclusive collision cast filter), which is CPU-native
and semantically correct.** Evidence below.

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

## 5. Recommendation — pivot to Option 2 (water-inclusive cast filter)

Option 3 is ruled out. The two viable per-region sources, cheapest first:

1. **Option 2 — a water-surface collision cast filter (RECOMMENDED).** The terrain cast already works
   (`goblin_heightfield.cpp`, `FUN_140c70360`, filter `0x5e` = `FILTER_GROUND`, which skips the water volume
   and hits the seabed). If the water surface carries a collision layer (plausible — the deep-water death
   trigger needs one), a *different* filter value hits it. Then per cell: cast `0x5e` (seabed/terrain Y) **and**
   the water filter (surface Y); `sea = surfaceY > terrainY`, and `surfaceY` is the real per-region water level.
   CPU-native, one extra cast, no GPU readback, no manager. **Next RE step:** decode the
   `WorldCollisionFilterCommand` query-type→layer table (referenced in
   `windows_terrain_raycast_heightfield_re_findings.md` §3) to find the water-surface filter, or brute-force
   candidate filter bytes live via a cast RPC over a known lake vs the ocean (expect surface>seabed, different
   Ys per region). See that doc's §4 option (2).
2. **Per-region MSB water plane Y (fallback if no water collision filter exists).** Water bodies are MSB parts
   (a flat plane); their placement Y is readable through the disk-MSB path MFG already has
   (`windows_runtime_msb_resident`). Needs a naming/material convention to pick the water parts — more work and
   less mod-agnostic than a cast filter.

If someone still wants the GPU interaction field (e.g. for wave-animated water rendering, not the sea-tag), the
anchors in §1–§2 are the entry: `GXSceneContext + 0xBE20` → the manager → its GPU textures, plus a D3D readback.
That is a much larger, render-thread effort and does not serve the sea-tag.

## 6. Code impact
`goblin_heightfield.cpp` keeps `kSeaLevelY` as a **dormant sentinel** (unchanged) — Option 3 yielded no
sampler, so nothing to wire yet. The `c.sea = groundY < kSeaLevelY` line stays inert until Option 2 lands the
water filter, at which point it becomes `c.sea = waterSurfaceY > c.groundY` (second cast). The render branch
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

## 8. Deliverable status
Option 3 investigated and **ruled out** (GPU wave-interaction resource, not a base-plane CPU sampler). Manager
location + heightmap nature + the audio-RTPC false-lead: **DONE (static)**. Actionable next step handed off:
**Option 2 — the water-surface collision filter** (`windows_terrain_raycast_heightfield_re_findings.md` §3–§4),
verifiable live on Linux/Proton with a filter arg on the cast RPC over a lake vs the ocean.
