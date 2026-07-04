# Custom-asset creation — options RE/research prompt

**Question (user, 2026-07-05):** to add OUR OWN placeable+walkable content (beyond rearranging existing
assets), are we OBLIGATED to author `.flver` (mesh) + `.hkx` (collision) offline, or is there another path
to "our own asset"? Map the options, rank by effort vs capability, name the minimal path.

## Established context (don't re-derive)
- **Placing EXISTING assets = SOLVED (runtime).** Pivot 2 (asset streaming-request) is statically RE-complete
  (`windows_geom_spawn_pivot2_re_findings.md`): `FUN_1406a5080(reqMgr, L"AEG###_###")` registers a request →
  streamer builds a real block-registered `CSWorldGeom` instance (renders; collision follows the standard
  world-geom path). reqMgr = `[DAT_143d69ba8 (FD4Singleton) + 0x30]`. **Only blocker = threading** (must fire
  on the main-update/streaming thread, not present/RPC — else deadlock). DLL: `geom_spawn::spawn_asset`.
- **MOVE existing geom = SOLVED** (`goblin_geom_move`, SetWorldMatrix vslot 26).
- **RULE (user):** reuse the shared AEG resource for meshes that ALREADY exist — a new mesh for an existing
  shape duplicates RAM/VRAM. New-mesh authoring is ONLY for genuinely-new shapes.
- **Assets are `AEG###_###`** (AssetGeometryParam / the AEG geombnd+hkxbnd archives). The frontier wall
  (`docs/re/README.md`) is GEOMETRY CREATION (MSB/model write), not placement.

## The options to investigate (verify each; give effort + capability + blockers)

### A. Offline AEG injection (the presumed default path)
Author or kitbash a `.flver` (mesh) + `.hkx` (collision), pack into the AEG archives (`aeg###.geombnd`,
`aeg###_###.geomhkxbnd` — confirm the exact bnd layout for THIS install), register a NEW `AEG###_###` id in
**AssetGeometryParam** (+ any AssetMaterial/SFX rows it needs), then pivot 2 spawns it at runtime like any AEG.
- Is a NEW AEG id actually streamable at runtime, or only ids present at map-load? (Does the reqMgr tree
  accept an id not in the original map's asset set — the `FUN_1406a5080` insert into `reqMgr+0x318`?)
- Tooling: FLVER authoring (SoulsFormats / FLVER Editor / Blender io_flver), HKX collision (hkx repack,
  collision-generator), witchy/yabber for bnd. Which work for ER `.hkx` (the Havok version + the Ser flavor)?
- Effort: high (per-asset offline pipeline) but well-trodden by the ER modding community.

### B. Runtime new-resource registration (no packed file)
Can the engine register a model/collision resource from an IN-MEMORY buffer at runtime (not a packed archive)?
Trace what `FUN_1406c7000`/`FUN_1406e38c0` (the request→instance resource resolution, pivot-2 findings) read
— do they REQUIRE a resident named resource (dvdbnd/mounted bnd), or is there a resource-register entrypoint
that accepts a buffer? Almost certainly file-backed only — CONFIRM + document why (so we stop looking).

### C. Engine PRIMITIVE / debug geometry (a shape with no authored `.flver`)
Does ER already carry primitive/debug geometry we can spawn WITHOUT authoring a mesh?
- Debug draw / collision visualizers (box/sphere/capsule) — is there a spawnable debug-shape or a
  `DbgMenu`/`GX` primitive with real collision?
- Hit/volume shapes: `HitParam`/`Havok phantom`/SpEffect area volumes, judgement/asset-collision volumes,
  invisible-wall collision — anything that gives a WALKABLE/BLOCKING Havok shape with no flver.
- If a collision-only primitive exists → invisible walkable platforms/walls with zero authoring (huge for a
  greybox custom world).

### D. Collision-only (Havok) without a visible mesh
Independent of C: is there a path to spawn a bare Havok collision shape (a box/mesh from parameters) as a
first-class world-geom instance (walkable), leaving rendering to a separate placed AEG for looks? Splits the
"walkable" need from the "looks" need — greybox first, art later.

## Deliverable
1. **Is `.flver`+`.hkx` authoring MANDATORY** for new shapes, or do C/D give a no-authoring greybox path?
2. **Ranked options table**: effort × capability × runtime-vs-offline × blockers, for A–D.
3. **Minimal path to "our own asset"** that is placeable (pivot 2) AND walkable — the smallest first brick.
4. Confirm/deny B (runtime buffer register) so it's settled.
5. For A: whether a NEW AEG id streams at runtime (the make-or-break for the offline pipeline feeding pivot 2).

Anchors: `DAT_143d69ba8` (FD4Singleton, reqMgr `+0x30`), `FUN_1406a5080` (registrar), `FUN_1406c7000`/
`FUN_1406e38c0` (resource→instance), AssetGeometryParam, the AEG geombnd/hkxbnd archives. Ghidra project
`D:\ghidra_proj2\ER` (imagebase 0x140000000). Cross-ref `windows_geom_spawn_pivot2_re_findings.md`.
