# RE brief — terrain / Havok collision WRITE path (deform the heightfield, mod-agnostic)

**Goal:** find the path to **MODIFY the terrain collision** at runtime — deform the ground, raise/lower a
patch, or drop a new collision volume — the write counterpart to the read-only heightfield raycast we already
have. This is the first probe of a genuinely new frontier: today MFG only *samples* the world (down-rays,
`windows_terrain_raycast_heightfield_re_findings.md`) and can *move an existing object* (MSB-placement
`vtable[0xd0] SetWorldMatrix`); it has **no** way to change the collision GEOMETRY itself. Static Ghidra on
`D:\ghidra_proj2\ER` (App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`, read-only), then a live
Proton/RPC verification. The DLL is in-process and can call a resolved fn on the game thread.

## Prime lesson to carry in (from the MSB-move RE — do NOT relearn it the hard way)
`windows_msb_placement_write_re_findings.md` proved: **poking raw bytes of a live physics-backed object is a
trap.** The engine caches derived state (the world matrix at `inst+0x220`) and Havok holds the object in its
broadphase; a blind write desyncs one or both → invisible move / phantom collision / crash. The move worked
ONLY because we drove the engine's **own setter**, which refreshes the cache and re-syncs Havok. **Assume the
same for terrain:** an `hknp` shape's sample buffer is almost certainly baked/immutable, guarded by a cached
AABB + a broadphase entry. The deliverable is the **engine-sanctioned mutation entrypoint** (a setter, a
rebuild call, or a dynamic-body add), NOT "which bytes hold the heights."

## Known anchors to build on (already RE'd — reuse, don't re-find)
```
CS::PhysWorld singleton   DAT_143d76060 (er+0x3d76060)   ctx = *(DAT_143d76060 + 0x98)   RTTI PhysWorld@CS@@
                          ctx+8 = the hknpWorld holder;  *(hknpWorld+0x4d8) = vtable (query slots)
hknpWorld::castRay        FUN_14187d960  (vtable slot 0x80, profiler "TtWorldCastRay")
hknpWorld::castShape      FUN_14187d9f0  (vtable slot 0x88, profiler "TtWorldCastShape")
ray cast (full, wrapper)  FUN_140c70360  (ctx, filter, start, segDir, &pt, &nrm, &dist) -> hit
                          — on hit, the AEG shape variant ALSO yields the hit collidable/body + material id
                            (findings §2) → this is the handle from "a point on the ground" to "the shape".
terrain filter            0x5e  (walkable ground / map collision)
```
The raycast already hands back **which body/collidable was hit** (findings §2, the AEG streamer derives an
asset id from it). That is the natural entry: cast a ray at the ground → get the terrain collidable → walk to
its shape → ask "can this be mutated, and how?"

## What we need (priority order)

1. **Identify the terrain collision SHAPE type.** From a ground-hit collidable (or by walking the loaded map
   collision), what `hknp` shape class backs the landscape? Candidates by Havok convention:
   `hknpHeightFieldShape` / `hknpCompressedHeightFieldShape` (a sampled height grid — the *mutable-in-theory*
   case) vs `hknpCompressedMeshShape` / `hknpConvexShape` (a baked triangle soup — effectively immutable).
   Give: the RTTI name (look for `hknp*Shape@@` / profiler tags), the shape vtable RVA, and the offset chain
   **collidable → body → shape** (how to reach the shape ptr from the raycast's returned body). This single
   answer decides whether "deform the existing ground" is even on the table or whether we must add proxies
   (Q4).

2. **Is the terrain shape MUTABLE, and via what call?** For the identified shape:
   - Does it expose a **setter / rebuild** (e.g. a `setHeight`/`setSampleValue`, a `updateShape` /
     `rebuildAABB` / broadphase-refresh, a "shape has changed" notify on the body or world)? Havok shapes are
     usually immutable after build, but the *heightfield* family sometimes keeps an editable sample buffer +
     an explicit rebuild. Find the read accessor first (the sampler the raycast bottoms out in — the height
     lookup inside `castRay`), then look for its **write sibling** and any dirty/rebuild call around it.
   - If it is a compressed/baked mesh (no editable buffer): say so plainly — that closes the "in-place
     deform" route and routes us to Q4 (add a proxy) instead. A negative here is a real deliverable.
   - Note what MUST be refreshed after a change: the shape AABB, the body's cached world AABB, and the
     **broadphase** (the `hknpWorld` spatial index) — the terrain analog of the move's `+0x220`/physics-sync.
     A height write that skips the broadphase update will read stale on the next `castRay` (the cheapest live
     negative control — Q6).

3. **The static-vs-dynamic body question.** Terrain is a STATIC/fixed body in the broadphase (never stepped).
   Static bodies are often stored in an immutable partition. Determine: is the terrain body flagged static
   (motion type / body flags), and does mutating a static body's shape require removing + re-adding it to the
   world (`hknpWorld::removeBody`/`addBody` — find the vtable slots near castRay's `0x80/0x88`), or is there
   an in-place "static shape changed" fast path? This is the difference between "cheap tweak" and "rebuild the
   region."

4. **Fallback route — ADD a dynamic collision volume (the pragmatic win).** If deforming the baked terrain is
   immutable (likely), the mod-agnostic path mirrors the MSB "add" strategy: **spawn a dynamic collision body
   with a simple shape** (box/sphere/convex) at a world transform, so we can *add* collision (a floor, a
   wall, a ramp) even if we can't reshape the landscape. Find: the `hknpWorld` **addBody** entrypoint + the
   minimal body/shape construction (does the engine wrap this as a CS class we can drive, à la
   `CSWorldGeomDynamicIns`? — check for a CS collision/rigidbody wrapper on `CS::PhysWorld`). Even a single
   box body proven addable+queryable is a shippable primitive (dev "place a platform"). Cross-reference
   `windows_geom_spawn_re_findings.md` — the geom instances already carry collision, so their spawn path may
   be the cheaper vehicle than raw `hknp` body construction.

5. **Where does the engine ITSELF mutate collision?** The most reliable setter is one the game already calls.
   Look for existing callers that add/remove/modify collision at runtime — destructible objects, opening
   doors/elevators (`docs/re/linux_group2_prompt_binding_re_findings.md` solved elevators — do they toggle a
   collision body?), fog walls, illusory walls, the AEG streamer adding/removing prop collision on stream
   in/out (`FUN_140699670` neighborhood). Any of these is a live, correct-by-construction example of the
   add/remove/modify call we want — decode the one with the simplest ABI.

## Constraints to confirm (runtime, Linux/Proton side)
6. **Thread-safety + verification.** Any collision write races the physics step exactly like the raycast
   (findings §6): must run on the **game thread** (the world-map step hook `hk_c32f0`, or a dedicated
   game-thread hook), NOT the present/RPC thread. Verify a change the way the move was verified — **byte-diff
   + a re-cast**: (a) cast a down-ray, record ground Y; (b) apply the mutation on the game thread; (c) cast
   again and confirm the ground Y / hit body changed by the expected delta; (d) note whether render (visual
   mesh) follows or only collision does (terrain visual is separate from collision — a collision-only change
   is still useful for dev/logic but say so).
7. **Loaded-region only + persistence.** Like the raycast, only streamed-in collision is touchable; confirm a
   mutation survives (does the streamer re-baking a tile revert it, à la the move's persistence check?) and
   what happens on tile re-stream / warp. Note if a change must be re-applied per tile-load.

## Deliverable
Findings → `docs/re/windows_terrain_heightfield_write_re_findings.md`. The minimum useful result is a clear
verdict on ONE of two routes, with the RVA + ABI + `this`-resolution + game-thread call recipe:
- **Route A (deform):** the terrain shape is an editable heightfield → the height-set + broadphase-refresh
  call pair. (Best case; likely NOT available if it's a baked mesh — a proven negative is still a deliverable.)
- **Route B (add proxy):** `hknpWorld::addBody` (or a CS wrapper / the geom-spawn path) to drop a dynamic
  collision volume at a transform → the smallest box-body add that a subsequent `castRay` confirms hitting.
Rank which is the cheaper first live probe (expected: **B**, since baked-mesh terrain makes A a dead end and B
reuses the move/spawn machinery). Objects/markers are out of scope — this is collision geometry only.
