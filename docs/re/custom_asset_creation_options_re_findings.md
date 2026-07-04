# Custom-asset creation — options RE findings (STATIC)

Answers `custom_asset_creation_options_re_prompt.md` (user Q, 2026-07-05): to add OUR OWN placeable+walkable
content, are we OBLIGATED to author `.flver` (mesh) + `.hkx` (collision) offline, or is there a lighter path?
Static Ghidra on `D:\ghidra_proj2\ER` (imagebase `0x140000000`, read-only) + RTTI index
`D:\ghidra_scripts\rtti_index.txt`, 2026-07-05. Builds on `windows_geom_spawn_pivot2_re_findings.md`
(pivot 2 = name-driven AEG request) and `windows_terrain_heightfield_write_re_findings.md` (Route B =
`hknpBoxShape` + `CSPhysIns` + `addBody`). **A live-verify checklist is at the end — Windows can't verify
(its DLL is stale); hand it to the Linux/Proton agent.**

---

## 0. TL;DR — the answer to the user's question
- **`.flver`+`.hkx` authoring is MANDATORY only for a VISIBLE, first-class AEG asset** (option A). It is **NOT**
  mandatory for a **WALKABLE greybox**: option **D** (a Havok `hknpBoxShape` collision body) gives a walkable/
  blocking platform-or-wall with **ZERO authored art** — invisible, parametric (just half-extents + a transform).
- **The minimal "our own asset" brick = D** (collision box, no authoring, pure runtime). Add looks later by
  placing an existing AEG stand-in over it (pivot 2, already RE-complete) or by authoring a real AEG (A).
- **B (register a model/collision from an in-memory BUFFER) is a confirmed DEAD END** — the engine resolves
  geometry strictly **by resource NAME** through the FD4 resource repository (file/VFS-backed). No buffer entry.
- **C (a high-level engine PRIMITIVE with collision, no mesh) does NOT exist as an easier tier than D** — the
  MSB/World "Hit" collision layer is itself resource/model-NAME driven (needs an authored collision `.hkx`);
  debug-draw primitives are render-only (no collision). So **C collapses into D** (the Havok box IS the primitive).
- **A DOES work for a genuinely-new asset:** a NEW `AEG###_###` id streams at runtime because resolution is
  **by name against the mounted VFS**, not a fixed per-map asset list (evidence in §A). The one real constraint
  is mount scope (its archive/overlay must be mounted for the current map — the safe route is a loose mod
  overlay or an already-mounted common bank).

---

## Evidence base (Ghidra decompiles, this run)
Two headless runs decompiled the resource-request + collision-instance paths:

### The request is NAME-keyed (no buffer) — `FUN_1406c7000` (er+0x6c7000)
Builds a resource **name string** and stores it into an FD4 resource container:
```c
FUN_140cf2360(container,10,0xd);                       // set up FD4 resource container (type 10 / 0xd)
... FUN_141eba960() ...                                 // = FUN_141ebb680()+0x28 : FD4 resource-repo/heap ctx
FUN_14011d9b0(&name, L"%s_%04d", partName@+0x48, idx@param_1+0x18);   // build the resource NAME
FUN_14011b190(container+0x2a0, name, len);              // stash the NAME into the container
// aborts: "Tried to create container with incompatible heap."
```
⇒ the request carries a **name** (`"<part>_%04d"`), never a byte buffer. `FUN_141eba960` (30+ callers across
container code) is the FD4 resource-repository/heap-context getter — confirming this routes through the FD4
resource system, which resolves names against the mounted archives (loose overlay → packed dvdbnd, per
[[dvdbnd-packed-reader]] path-hash prime 0x85). **This is the core fact behind both A and B.**

### The resolver needs the named resource already LOADED — `FUN_1406e38c0` / `FUN_1406e61d0`
`FUN_1406e38c0` (the state-4 service, builds the scene/render node) calls
`FUN_1406e61d0(resource = *(inst+8), …)`, whose whole body is:
```c
if (resource != 0) return FUN_141eb9ed0(0x40,8);   // resource loaded → alloc the 0x40 instance holder
return 0;                                            // resource null → nothing
```
So the instance is only built once the **named resource is non-null (resolved from disk)**. Resource-handle
driven end-to-end — **no in-memory-buffer register exists on this path (B is dead)**.

### The "Hit" collision layer is resource/model-NAME driven too (C is not a shortcut)
ELDEN RING carries a full first-class **collision-only "Hit" subsystem** (RTTI): `WorldHitManImp@CS`,
`HitIns`/`HitInsBase@CS`, `CSMsbPartsHit@CS`, `CSWorldGeomHitIns@CS`, plus `WorldArea/Block/GridAreaHit`.
These are the runtime form of MSB "Hit" parts (`h######` = invisible walls / walkable collision meshes). But
they are **model-driven, not parametric**:
- `CSMsbPartsHit` ctor `FUN_140cf1ef0` — stamps the vtable + base ctor; structurally parallel to
  `CSMsbPartsGeom` (an MSB part that names a resource).
- `HitIns` ctor `FUN_140706940` — builds its collision geometry via
  `thunk_FUN_140222b96(self+3, desc, *(desc+8)+0x48, *(desc+8)+0x58, …)` — reading the descriptor's
  **`+0x48`/`+0x58`**, the SAME name/partsList offsets the geom resource uses. ⇒ its collision comes from a
  **named authored resource** (a baked `.hkx`), not a box parameter.
- `CSWorldGeomHitIns` ctor `FUN_1406e07d0` — the geom's collision half (~0xd8 bytes), self-registers into
  singleton `DAT_143d691d8`; pairs with the geom model's resident collision.
⇒ using the Hit layer STILL requires authoring a collision mesh. Debug-draw primitives (`CSFfxDebugDrawer`,
`GXFfxDebugDrawer`, `GXDraw*`) are **render-only, no physics**. **So there is no no-authoring collision
primitive ABOVE the Havok layer — the only one is the Havok box (D).**

---

## §A — does a NEW `AEG###_###` id stream at runtime? (the make-or-break for A) — YES
Two independent facts say a new id is streamable, not restricted to the map's original asset set:
1. **The registrar INSERTS a fresh id** (pivot 2 findings): `FUN_1406a5080` allocates a new request id
   (`FUN_1406abfa0`) and RB-tree-**inserts** it into `reqMgr+0x318` — it does not require the id to pre-exist.
2. **Resolution is by NAME against the FD4 resource repository** (this run, §evidence): `FUN_1406c7000` builds
   an arbitrary `"AEG%03u_%03u"`-shaped name and hands it to the resource container; the loader resolves that
   name against the mounted VFS (loose mod overlay wins, then packed dvdbnd — deterministic path hash).
⇒ **a new AEG id whose file is present in the mounted VFS, with an `AssetGeometryParam` row, will stream.**
This matches ER modding-community reality (add rows + files to an asset bank; the streamer loads them by name).
**One real constraint (live-verify):** mount SCOPE. AEG banks are mounted per asset-set; a brand-new `aeg###`
*number* needs its archive mounted for the current map. The safe, proven route is a **loose mod-overlay file**
(name-resolved first, always visible) or **adding a new id into an already-mounted common bank** (e.g. the
shared `aeg099` bank) rather than minting a new bank number.

---

## Ranked options table (effort × capability × runtime/offline × blockers)
| Opt | What | Authoring | Runtime vs offline | Gives | Effort | Verdict |
|-----|------|-----------|--------------------|-------|--------|---------|
| **D** | Havok `hknpBoxShape` + `CSPhysIns` + `hknpWorld::addBody` | **NONE** | pure runtime | **walkable/blocking** volume (invisible) | **low–med** | ★ minimal brick; art layered later |
| **A** | Author `.flver`+`.hkx`, pack AEG bnd, register `AssetGeometryParam` row, pivot-2 spawn | flver **+** hkx (offline pipeline) | offline build + runtime spawn | **visible + collidable** first-class asset | high | the "real asset" path; new id DOES stream (§A) |
| **A′** | Author `.flver` only, reuse/skip collision; or place existing AEG for looks over a D box | flver only | offline + runtime | **visible** (collision from D) | med | greybox-with-art; splits looks from walkable |
| **C** | High-level engine primitive (Hit / debug shape) with no mesh | — | — | — | — | **does not exist** (Hit = model-driven; debug-draw = render-only) ⇒ use D |
| **B** | Register model/collision from an in-memory buffer (no file) | none | runtime | — | — | **DEAD END** (engine resolves by name/file only) |

## Minimal path to "our own asset" (placeable AND walkable) — the smallest first brick
1. **Walkable greybox NOW = D.** `add_collision <half-extents> <transform>`: build an `hknpBoxShape`
   (ctor `0x1878cf0`), make a `CSPhysIns` (ctor `0xc66df0`) / DLRF-spawn it (`0xc66ea0`), `hknpWorld::addBody`
   on the game thread; verify with the existing down-ray (`hf_probe`). Zero authoring. (Remaining static gap:
   the exact `addBody` vtable slot — dump `hknpWorld` vtable `0x2eedc78` live; see terrain findings §6.)
2. **Add looks without authoring** = place an EXISTING AEG stand-in over the box via pivot 2 (name-driven
   request, RE-complete) — the mod-agnostic "circle/greybox first" posture.
3. **Promote to a real bespoke asset** = A (author flver+hkx, register a new AEG id — which streams, §A).

## Deliverable answers (prompt §Deliverable)
1. **`.flver`+`.hkx` mandatory?** Only for a visible first-class AEG (A). A **walkable greybox needs neither**
   (D). Visible-but-borrowed-collision needs only a flver (A′) or none (place an existing AEG).
2. **Ranked table** — above.
3. **Minimal placeable+walkable brick** — D (collision box), then optional AEG stand-in for looks.
4. **B (runtime buffer register)** — **CONFIRMED DEAD** (name/file-backed resolution only). Settled; stop looking.
5. **A: new AEG id streams at runtime?** — **YES**, name-resolved against the mounted VFS + fresh-id RB-insert;
   only caveat is mount scope (use a loose overlay / already-mounted bank). Live-confirm below.

## Live-verify checklist (Linux/Proton — Windows DLL is stale)
1. **D smoke test (highest value):** `add_collision` a box near the player on the game thread → `hf_probe`
   the footprint → expect a hit at box-top Y. Needs the `hknpWorld::addBody` slot (dump `hknpWorld` vtable
   from `*(CSPhysWorld+0x08)`, `CSPhysWorld = *(DAT_143d76060+0x98)`).
2. **A new-id stream test:** drop a NEW `AEG###_###` (a renamed copy of an existing one) as a **loose mod-
   overlay** file + an `AssetGeometryParam` row for it, then `spawn_asset <new AEGname>` on the main-update
   thread (pivot 2's thread constraint) → does the streamer resolve+place it? Confirms §A end-to-end and the
   mount-scope caveat (loose overlay vs new bank number).
3. **Hit-layer confirm (optional):** dump a live `HitIns`/`CSWorldGeomHitIns` descriptor `+0x48`/`+0x58` to
   confirm it names a collision resource (baked `.hkx`), closing C.

## Anchors (er-relative, imagebase 0x140000000, this ERR build)
```
request name builder   FUN_1406c7000 (er+0x6c7000)   swprintf(L"%s_%04d") -> FD4 res container FUN_140cf2360(_,10,0xd)
FD4 res-repo/heap ctx  FUN_141eba960 (er+0x1eba960) = FUN_141ebb680()+0x28
scene-node builder     FUN_1406e38c0 (er+0x6e38c0)  -> FUN_1406e61d0(resource,1,…); world-matrix FUN_1409f1320
resource->holder       FUN_1406e61d0 (er+0x6e61d0)  alloc 0x40 IFF resource!=0 (must be loaded first)
Hit subsystem (RTTI):
  WorldHitManImp@CS    vtable 0x2a8c980 (ctors 0x70fa10,0x70fd90)
  HitIns@CS            vtable 0x2a8bc00 (ctor  FUN_140706940 @0x706940; desc+0x48/+0x58 = collision resource)
  HitInsBase@CS        vtable 0x2a8beb0
  CSMsbPartsHit@CS     vtable 0x2ba6800 (ctor  FUN_140cf1ef0 @0xcf1ef0; base FUN_140ced100)
  CSWorldGeomHitIns@CS vtable 0x2a87178 (ctor  FUN_1406e07d0 @0x6e07d0; singleton DAT_143d691d8; ~0xd8 B)
Route D (no authoring): hknpBoxShape vtable 0x2eec698 (ctor 0x1878cf0); CSPhysIns@CS vtable 0x2b92b18
  (ctor 0xc66df0; DLRF factory 0xc66ea0); hknpWorld vtable 0x2eedc78 (find addBody slot live);
  CSPhysWorld = *(DAT_143d76060+0x98), hknpWorld* = CSPhysWorld+0x08.
Cross-ref: windows_geom_spawn_pivot2_re_findings.md (pivot 2), windows_terrain_heightfield_write_re_findings.md (Route B / D).
```
