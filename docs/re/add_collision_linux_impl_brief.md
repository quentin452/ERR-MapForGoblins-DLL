# `add_collision` — Linux/Proton implementation brief (Route D: walkable greybox box)

Implements the walkable-greybox collision box: spawn a Havok `hknpBoxShape` body into the live world so the
player can stand on / be blocked by it, with ZERO authored art. This is the minimal "our own asset" brick
(`custom_asset_creation_options_re_findings.md` §D). The static RE is COMPLETE
(`hknpworld_addbody_slot_re_findings.md`, `windows_terrain_heightfield_write_re_findings.md`); this is the
build + live-verify. **Windows can't run it (stale DLL) — Linux/Proton owns deploy + RPC + the smoke test.**

Oracle already shipped: the `hf_probe` down-ray (`goblin::heightfield`) — cast at the box footprint, expect a
hit at box-top Y. Zero new read-RE needed to verify.

---

## 0. The recipe (from the findings — the whole thing)
```
world  = *(CSPhysWorld + 0x08)              // hknpWorld; CSPhysWorld = ctx = *(PhysWorldSingleton + 0x98)
shape  = hknpBoxShape::ctor(half-extents, convexRadius)          // FUN_141878cf0
hknpBodyCinfo cinfo; cinfoInit(&cinfo);      // FUN_141911210  (defaults)
   cinfo.shape = shape;  cinfo.position/orientation = <transform>;  cinfo.motionType = STATIC;  cinfo.flags |= 0x100
id     = allocateBody(bodyMgr, &id, &cinfo)  // FUN_1418aabf0  -> hknpBodyId
addBody(bodyMgr, &id, 1, addMode, actMode)   // FUN_1418a9ff0  (computes AABB + inserts into broadphase)
```
`bodyMgr` = the `hknpWorld` itself (hknpBodyManager is embedded; `FUN_1418a9ff0` reads `+0x28` bodies /
`+0x30` count / `+0x180` motions / `+0x4d8` broadphase). Bodies are **0xb0 (176) bytes**, array at
`bodyMgr+0x28`. The engine calls these two handlers DIRECTLY (no command dispatcher) — see the CS flush
`FUN_140c72c20` — so our DLL can too.

## 1. PREREQUISITE — map `hknpBodyCinfo` first (one Ghidra pass on Windows, ~10 min)
The ONE thing static RE hasn't pinned is the **cinfo field layout** (which offset is `shape`, `position`,
`orientation`, `motionType`, `qualityId`, `mass`). Do this BEFORE coding, else you're poking blind. Decompile
on `D:\ghidra_proj2\ER` (`query.java`):
- **`FUN_141911210`** (cinfo init) — its stores reveal the default fields + struct size.
- **`FUN_141878cf0`** (`hknpBoxShape` ctor) — its args (half-extents vec, convex radius) + what it returns.
- **`FUN_14167eb10`** (the template's `cinfo.position/shape` writer) + re-read `FUN_1418a3080` — cross-map
  which cinfo offset each template store targets (`+0x50` pos/shape, `+0x80` orientation, `flags|=0x100`).
- **`FUN_1418aabf0`** (allocateBody) — confirm arg order `(bodyMgr, hknpBodyId* out, hknpBodyCinfo* cinfo)`
  and that it reads `cinfo.shape` / `cinfo.motionType`.
Deliver a `hknpBodyCinfo` offset table into `hknpworld_addbody_slot_re_findings.md`. **A cheaper fallback if
Ghidra stalls:** live-recon — after calling `FUN_141911210` on a scratch buffer, dump 0x100 bytes; then find
a real static body in `bodyMgr+0x28` (0xb0 stride) and diff a known-good body's header to learn the shape/
motion/flags offsets. Either way, don't guess the layout.

## 2. New files — mirror `goblin_geom_spawn` + `goblin_heightfield`
`src/goblin_add_collision.{hpp,cpp}`, namespace `goblin::add_collision`. Header shape (copy the staged
resolve/result idiom from `goblin_geom_spawn.hpp`):
```cpp
namespace goblin::add_collision {
  struct AddResult {
    bool ok=false;
    uint64_t world=0, bodyMgr=0, shape=0; uint32_t bodyId=0;
    float half[3]={}, pos[3]={};
    char err[128]={};
  };
  // Read-only: resolve world/bodyMgr from the PhysWorld singleton (no allocation). Sanity before firing.
  AddResult resolve_world();
  // Build box (half-extents) + allocate + addBody at a world/local position. force=false => resolve+build
  // cinfo only, NO addBody (report + dump); force=true => actually add. Present/game-thread gated (§4).
  AddResult add_box(const float half[3], const float pos[3], bool force=false);
}
```
Reuse from `goblin_heightfield.cpp`: the `resolve_ctx()` chain gives you **`ctx = CSPhysWorld`**; then
`world = rd(ctx + 0x08)`, `bodyMgr = world` (confirm `rd(world+0x28)` is a plausible ptr and `*(u32*)(world+
0x30)` a sane count before use). Reuse `goblin::get_player_world_pos` for a default position (player + a small
offset). Use the SAME guarded `rd()` + `alignas(16)` vector discipline as the raycast (Havok reads vectors via
`movaps` → unaligned = fault).

## 3. Signatures — add to `re_signatures.hpp` (RVA now, AOB later)
Follow the CASTRAY/ENSURE_ASSET_REQUEST pattern: an AOB const + an RVA fallback + a `resolve_func_aob` call,
plus a `[SIG]` boot health-check line. Craft AOBs from a LIVE `mem_dump` (`tools/hf_hook_scout.py disasm
--aob`) once the RVAs are proven to resolve. RVAs (ERR 2.2.9.6, imagebase 0x140000000):
```
BOX_SHAPE_CTOR_FN    er+0x1878cf0   hknpBoxShape ctor
BODY_CINFO_INIT_FN   er+0x1911210   hknpBodyCinfo init
ALLOCATE_BODY_FN     er+0x18aabf0   hknpBodyManager::allocateBody(bodyMgr, &outId, &cinfo)
ADD_BODY_FN          er+0x18a9ff0   addBody(bodyMgr, ids, count, addMode, actMode)
```
Signatures (typedefs):
```cpp
using CinfoInitFn = void (*)(void* cinfo);
using BoxCtorFn   = void* (*)(void* self, const float halfExtents[4], float convexRadius);  // confirm in §1
using AllocBodyFn = uint32_t* (*)(void* bodyMgr, uint32_t* outId, void* cinfo);              // confirm in §1
using AddBodyFn   = void (*)(void* bodyMgr, uint32_t* ids, uint32_t count, int addMode, int actMode);
```

## 4. Thread strategy — start on present, fall back to a game-thread hook
`SetWorldMatrix`/`move_asset` already run hook-free from the present thread, but `spawn_asset` (pivot 2)
**deadlocked** it (RB-tree lock inversion with the streamer). `addBody` touches the broadphase/body arrays —
unknown contention. So:
1. **First attempt: present thread** (same `pump()`/`tick_present()` path the raycast uses). Gate behind the
   freeze watchdog (`goblin_freeze_watchdog.cpp`) and `force` — if it stalls, the watchdog logs it, no silent
   hang. This is the cheap path; try it first.
2. **If it stalls: hook a per-frame game step and inject there.** The engine's OWN body-add flush is
   **`FUN_140c72c20`** (collects pending ids → `FUN_1418a9ff0`) — mirror its call site / thread. That's the
   safe injection point (the lock is held correctly there), the same lesson pivot 2 reached.
Do the alloc+add as ONE unit on the chosen thread (don't split alloc and add across frames). SEH-wrap the two
engine calls in a `noinline` helper (`clang-cl-seh-noinline` idiom, like `call_cast`).

## 5. RPC command — `add_collision`, staged like `spawn_asset`
In `goblin_debug_rpc.cpp` (near `hf_probe` / `spawn_asset`):
```
add_collision                       -> resolve_world() only (report world/bodyMgr; NO alloc) [safe default]
add_collision <hx> <hy> <hz>        -> build box + cinfo, DUMP, but do NOT addBody (force=false)
add_collision <hx> <hy> <hz> go     -> actually alloc + addBody at the player (force=true)
add_collision <hx> <hy> <hz> <x> <y> <z> go  -> at an explicit position
```
Default half-extents e.g. `100 20 100` (a 2m-thick 20×20 platform in ER units; ~10 u/m). Log `world`,
`bodyMgr`, `shape`, `bodyId`, and the resolved position. Add `add_collision` to the RPC help string (line
~412) and a `tools/rpc_tests/test_add_collision.py` (+ its filename to the `.vscode/tasks.json` `rpcTest`
pickString options, per CLAUDE.md).

## 6. Smoke test (the oracle closes the loop)
1. In-world, map CLOSED (collision unloads while the map is open — heightfield note).
2. `add_collision 100 20 100 go` near the player (place its top ~50u ABOVE the player's feet so it's clearly
   separate from the ground).
3. `hf_probe_present` on the box footprint → expect **GROUND y ≈ box-top Y** (not the terrain below). A hit at
   the box top = the body is in the broadphase and collidable.
4. Walk onto/into it — stand on the platform / be blocked by a wall (make one axis thin for a wall variant).
5. Persistence: does it survive a few seconds + a tile re-stream? Note if the streamer purges non-native
   bodies (may need re-add per region load) — record in the findings.

## 7. Coordinate frame (don't skip — it's the #1 way this lands invisibly wrong)
The raycast is Havok **block-local**, NOT the vmap world frame (`goblin_heightfield` captures a per-sample
`local−world` offset). Build the cinfo transform in the **same block-local frame** the raycast/bodies use, or
the box spawns in the wrong chunk and `hf_probe` misses it. Easiest: derive the box position from
`get_player_world_pos` (already local) + a small delta, matching what `hf_probe`'s `@player` path uses.

## 8. Risk register
- **cinfo layout wrong** → alloc reads garbage shape/motion → AV (SEH-caught) or an invisible/no-collision
  body. Mitigation: §1 first; dump cinfo before firing (`force=false`).
- **bodyMgr ≠ world** (embedded manager at a sub-offset) → `FUN_1418a9ff0` walks the wrong array. Mitigation:
  validate `world+0x28`/`+0x30` look like {ptr, small count} before calling; if not, find the bodyMgr offset
  by decompiling how `FUN_1418a3080` gets `*(this+0x40)`.
- **thread stall** → freeze watchdog fires (no crash); fall back to the `FUN_140c72c20` game-thread hook (§4).
- **static motion needs a commit** → if the box is allocated+added but not solid, try `updateBroadphase`
  (opcode 0x2f) / `commitAddBodies` (0x30) after, or `addMode/actMode` = `1,0` vs `0,0` (§findings §5.5).
- **not persistent** → acceptable for the first probe (bounded); note it and re-add per region if needed.

## 9. Anchors (er-relative, imagebase 0x140000000)
```
PhysWorld singleton slot   er+0x3d76060  (PHYSWORLD_RVA, already in goblin_heightfield.cpp)
  ctx = CSPhysWorld        = *(*(er+0x3d76060) + 0x98)      [resolve_ctx() reuse]
  hknpWorld = bodyMgr      = *(ctx + 0x08)                  [confirm +0x28 bodies / +0x30 count]
hknpBoxShape ctor          FUN_141878cf0 (er+0x1878cf0)     [+ hknpSphereShape er+0x187a220]
hknpBodyCinfo init         FUN_141911210 (er+0x1911210)
allocateBody               FUN_1418aabf0 (er+0x18aabf0)     (bodyMgr, &outId, &cinfo)
addBody                    FUN_1418a9ff0 (er+0x18a9ff0)     (bodyMgr, ids, count, addMode, actMode)
create+add template        FUN_1418a3080 (er+0x18a3080)     (hknpCharacterProxy — copy its cinfo fills)
CS per-frame body flush    FUN_140c72c20 (er+0xc72c20)      [game-thread injection point if present stalls]
body stride                0xb0 (176 B) @ bodyMgr+0x28; count @ +0x30; broadphase @ +0x4d8
oracle                     goblin::heightfield hf_probe (down-ray, filter 0x5e)
```
Cross-ref: `hknpworld_addbody_slot_re_findings.md`, `windows_terrain_heightfield_write_re_findings.md`,
`custom_asset_creation_options_re_findings.md`. Sibling impls to copy: `src/goblin_geom_spawn.*` (staged
resolve/force RPC), `src/goblin_heightfield.*` (world resolve + SEH-guarded aligned-vector engine call).
```
