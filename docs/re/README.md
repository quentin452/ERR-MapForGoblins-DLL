# RE coverage map — what of ELDEN RING is reverse-engineered, and what isn't

Index + honest coverage map for `docs/re/`. **172 RE docs** live here. Two kinds:
- `*_re_findings.md` / `*_RESOLVED.md` — a SOLVED structure/function (the answer).
- `*_re_prompt.md` / `*_analysis.md` — an OPEN or historical RE task handed to Ghidra/CE.

The authoritative list of PINNED code sites is **`src/re_signatures.hpp`** (45 AOBs; `resolve_all_signatures`
logs PASS/FAIL per sig at boot). Live/open work is tracked in **`docs/HANDOFF.md`**. This file is the map;
those two are the source of truth on conflict.

Rule of thumb: **FromSoft's DATA layer is well-mapped (params, items, names, flags, save, inventory,
map-read); the walls are GEOMETRY/PLACEMENT (MSB write) and SCRIPT LOGIC (ESD/EzState).** That boundary is
exactly what separates "runtime re-skin of existing content" (works) from "create new content" (not started).

---

## ✅ Mapped (RE'd + pinned/documented)

| Subsystem | Key sigs (`re_signatures.hpp`) | Key findings | Enables |
|-----------|-------------------------------|--------------|---------|
| **Params (engine of the framework)** | `GAME_DATA_MAN`, `SOLO_PARAM_LIST` | `windows_live_paramdef_offset`, `windows_legacyconv_param_live` | Runtime clone/set/get of any param row+field, paramdef-free (offset resolved live from the exe) |
| **Inventory / items** | `ADD_ITEM_FUNC`, `INVENTORY_ACCESSOR`, `GAME_DATA_MAN` | `windows_goods_count`, `windows_runtime_asset_to_itemlot`, `windows_global_item_position_structure` | Grant/count/strip items; `GameDataMan→PlayerGameData→EquipGameData→EquipInventoryData`; ItemLotParam layout; AEG `pickUpItemLotParamId`; id encoding; grantable ceiling `0x7FFFFE` |
| **Messages / FMG** | `MSG_REPOSITORY`, `GETMESSAGE` | `windows_fmg_slot`, `windows_native_msg_getter` | Inject/override item & place names; FMG-v2 layout; base/DLC/menu slot semantics; `FmgFile_lookup` binary search |
| **Save** | `SAVE_FN`, `SERIALIZE_FN` | `windows_save_serialize`, `linux_save_function`, `windows_save_function_rpm` | Whole-slot serialize hook; strip/reinject save-clean bracket; sidecar `.mfg` |
| **World map / markers** | `WORLDMAP_POINT_FN/_CTOR`, `WORLDMAP_PROJ_LOOP/_POINT`, `WORLDMAP_CONV_BUILD`, `MARKER_ARRAY_CTOR`, `MARKER_CHAIN_SLOT` | `world_map_projection`, `windows_world_to_mapspace_projection`, `windows_worldmap_tile_fog`, `windows_worldmap_tile_placement`, `windows_worldmap_page_transition`, `windows_map_point_icon_layout`, `windows_aeg_collectible_source`, `windows_runtime_msb_resident` | Marker projection + map-space; tile fog; **tile ART placement (name→256-cell grid decode)**; page switch; disk-MSB loot parse (asset→lot→item, enemy/EMEVD/collectible drops); icon textures (resident + disk) |
| **World state / entities** | `WCM_FINDER`, `GET_CHRINS_FROM_HANDLE`, `EVENT_FLAG_MAN_SLOT(_ALT)`, `IS/SET_EVENT_FLAG`, `GEOM_FLAG_SLOT`, `WORLD_GEOM_MAN_SLOT`, `PLAYER_MAPID_SLOT` | `windows_player_pos_RESOLVED`, `windows_enemy_boss_runtime_pos`, `windows_enemy_name_runtime_source`, `windows_enemy_name_hud_feed` (partial: `01_000_fe.gfx`/`EnemyTag_ColorText` HTML feed confirmed live; name-feed write site + GATE open), `windows_geom_flag_savedata_table`, `windows_fieldins_registry_layout_and_preopen` | Player/enemy/boss live position; event flags read/write; geom (collected) flags; field-instance registry/pool |
| **Nav / warp** | `LUA_WARP`, `CS_LUA_EVENT_MANAGER` | `windows_grace_warppin_teleport`, `windows_grace_warppin_setto_abi` | Grace fast-travel (dev nav) |
| **Menus / UI / present** | `CSMENUMAN_SLOT`, `CSFEMAN_SLOT`, `PAUSE_BRANCH`, `RENDER_REAPPLY_RES`, `EC_TEST_DISTANCE_VFT` | `windows_menu_item_icon`, `windows_menu_cursor_iconid_populator`, `worldmap_menu_and_native_clip`, `windows_midsession_resolution_swapchain`, `windows_ingame_pause` (branch-flip pause; live-confirmed resume-latency bug), `game_timestep_freeze` (planned: zero the `FUN_140623410` `dt`/FD4Time for a clean pause + `set_timescale`) | Menu icon/cursor; worldmap clip; resolution swapchain; native pin suppression; **in-game pause / timestep freeze** |
| **Bonfire / misc params** | `BONFIRE_TEXTID1_ACCESS`, `GOODS_TYPE_ACCESS`, `GOODS_SORT_GROUP_ACCESS`, `AEG_PICKUP_LOT_ACCESS`, `AEG_REPICK_BIT_ACCESS` | (field AOBs, `windows_itemlot_field_re_prompt` for the lot fields) | Named field edits over the param-override registry |

---

## ❌ Missing / frontier (ranked by framework impact)

1. **MSB WRITE — the big hole.** We READ disk MSB placements; there is NO write path. So you can RE-SKIN
   existing placements (repoint a lot, change a lot's item — done) but CANNOT ADD a new placement (a
   treasure/mob at NEW coords). Blocks new map content + vision #3. **First probe scoped:
   `windows_msb_placement_write_re_prompt.md`** — settles at which layer an edit moves an object (instance
   snapshot vs resident MSB bytes) via the MSB→instance load path + a live 2-target write test; splits the
   cheap "move existing" win from the real "add new" (spawn factory + tile re-stream). **Volet A DONE
   (static, `..._findings.md`):** MSB pos is snapshotted twice → resident-byte writes are inert; the
   movable transform is `CSWorldGeomIns+0x18`, and `CSWorldGeomDynamicIns` (`FUN_1406b9880`, factory
   `FUN_1406c5900`) is a movable geom class = the vehicle for move + add. **MOVE is now CRACKED + LIVE-
   VERIFIED (2026-07-03):** the transform setter is `vtable[0xd0] SetWorldMatrix(self, mat4x4)`; driving
   it on a live `CSWorldGeomIns` (via `move_asset` RPC, `test_move_asset.py` 7/7) moves the cached world
   matrix (`inst+0x220`) by the exact delta, no crash. So "move an existing placement" is a solved
   primitive **and DURABLE** (persistence confirmed — cache-only write held ~7s, no revert) **and RENDERS**
   (`move_all` mass move watched live). **ADD a new placement — all RE now DONE, ctor call unblocked
   (`windows_geom_spawn_re_findings.md`).** No isolated "spawn one geom" call exists (the spawn drivers
   `FUN_1406a7930`/`FUN_1406adc80` are the tile-streaming state machine; instances are placement-new'd into
   fixed-cap BlockData pools + pushed into geom_ins vector `+0x288`), so ADD = drive the Dynamic ctor
   `FUN_1406b9880` directly. Every static blocker is closed: srcType is an 8B packed FieldIns id (built from
   3 mask globals or copied); `param_3` = the source BlockData (live recon `spawn_probe` corrected the
   layout: BlockData@+0x10, transform module@+0x20, CSMsbPartsGeom@+0x30); the ctor reads only
   `BlockData+0x18b`; and the move-init CORRUPTION risk is solved (rebuild `param_4` via the driver's own
   builder `thunk_FUN_144cbdae7`, don't alias the source). Remaining = a multi-step BUILD (code
   `spawn_clone` on Proton: build args → `FUN_1406b9880` into a self-alloc 0x5b0 → SetWorldMatrix-offset),
   not more RE. This is the last brick of the MSB-write wall for GEOM placement. **ALT: ADD via pivot 2
   (asset-request path) is fully RE'd + reqMgr resolves live, but `FUN_1406a5080` is game-thread-bound —
   present=deadlock, worker=fault, the proximity-streamer steps don't fire (4 live attempts,
   `windows_geom_spawn_pivot2_re_findings.md §live-4-attempts`). The one wall left = a reliable per-frame
   main-update-thread injection point → `windows_geom_spawn_thread_re_prompt.md` (deferred-queue scaffolding
   ready in `goblin_geom_spawn.cpp`). **SOLVED (static) → `windows_geom_spawn_thread_re_findings.md`:** the
   registrar's own per-frame thread = the world-geom update `FUN_140623410` (er+0x623410) which each frame calls
   the reqMgr update **`FUN_1406d31f0` (er+0x6d31f0, reqMgr=`DAT_143d69ba8`)**. Hook `FUN_1406d31f0`
   (set `STREAMER_STEP_RVA=0x6d31f0`) → drain the queue on the right thread, every frame in-world. Bonus: native
   by-id spawn helpers `FUN_1406d4e80`/`FUN_1406d0040` build `AEG###_###`+block+registrar for you. Registrar is
   main-update-CONTEXT-bound (single-writer reqMgr RB-tree, not TLS) → any caller on that stack is safe. Pending
   the live `hk_step` acceptance run.**
2. **ESD / talk scripts (EzState) — mostly unmapped.** Merchant shop↔NPC join, dialogue, talk-driven
   logic need an EzState bytecode evaluator (shelved after a spike — see
   `docs/plans/merchant_item_search_plan.md` Slice 3).
3. **regulation.bin / mod-VFS virtualization** — for the strong form of world-swap (vision #1). Only
   `windows_regulation_modroot_anchor_re_prompt.md` scratches it; the VFS-level swap isn't RE'd.
4. **3D models / FLVER** — the CREATION options are now MAPPED (`custom_asset_creation_options_re_findings.md`,
   2026-07-05): a **walkable greybox needs NO authoring** (Route D — a Havok `hknpBoxShape` collision box, §4b);
   a **visible first-class AEG asset** does need `.flver`+`.hkx` (option A), and a NEW `AEG###_###` id **DOES
   stream** at runtime (name-resolved against the mounted VFS, not a fixed per-map set — safe route = a loose
   mod overlay / already-mounted bank). Settled dead ends: **B** (register geometry from an in-memory buffer)
   is impossible (engine resolves by resource NAME only); **C** (a high-level engine primitive with collision,
   no mesh) doesn't exist — the MSB/World "Hit" collision layer (`HitIns`/`CSMsbPartsHit`/`CSWorldGeomHitIns`)
   is itself model-name driven, and debug-draw primitives are render-only. Actual FLVER/mesh authoring RE
   (the offline pipeline) is still untouched.
4b. **Terrain / Havok collision WRITE — new frontier, partly RE'd (static, 2026-07-04).** We READ the terrain
   (down-ray heightfield) and can MOVE an object, but there is NO way to change collision GEOMETRY (deform the
   ground, add a platform/wall). **Static class inventory + `CSPhysWorld`/`hknpWorld` wiring DONE**
   (`windows_terrain_heightfield_write_re_findings.md`; prompt `..._re_prompt.md`): terrain is a **baked
   `hknpCompressedMeshShape`** (vtable er+0x2eeb908) ⇒ **Route A (deform-in-place) is a near-certain DEAD END**
   (an editable `hknpHeightFieldShape` er+0x2ee2a18 also exists — a live shape-vtable read decides). **Route B
   = add a dynamic collision body** (`hknpBoxShape`/`hknpSphereShape` via `CSPhysIns@CS` / DLRF factory +
   `hknpWorld::addBody`), reusing the geom-spawn machinery + the MSB-move lesson (drive the engine ctor/setter,
   never poke raw shape bytes). `CSPhysWorld` ctor `FUN_140c6f120` mapped (hknpWorld* @ `CSPhysWorld+0x08`,
   confirming the raycast `ctx+8`; hknpWorld ctor `FUN_1418a6760`; shape-tag codec `hknpUFMShapeTagCodec<3,5,8>`
   behind the `0x5e` filter). **Remaining = live (Linux):** the shape-vtable read (decides A vs B), the
   `hknpWorld` vtable dump for the exact `addBody` slot, and an `add_collision` box smoke test vs the `hf_probe`
   raycast. **UPDATE (2026-07-05, `hknpworld_addbody_slot_re_findings.md`): the add path is now STATICally
   COMPLETE.** `addBody` is NOT a vtable slot — hknp uses a deferred command buffer (`addBody` = command
   opcode 1; the vtable's slot 7/8 are the command executor + debug-printer, which leaked the full opcode
   map). The real callable recipe (as CS itself calls it, no dispatcher): `FUN_1418aabf0` (allocateBody) →
   `FUN_1418a9ff0` (addBody: AABB + broadphase insert), with a working exe template `FUN_1418a3080`
   (`hknpCharacterProxy` body create). Remaining = build the box `cinfo` + the live smoke test.
5. **Custom mob PLACEMENT** — NpcParam is param-driveable, but placing a custom enemy is MSB write (#1).

### Smaller tactical gaps (see HANDOFF)
- **Loading-screen / world-load state (stuck-load watchdog) — SOLVED (static, 2026-07-04).** Load-in-progress
  flag + phase = **`CSFD4LocationStep+0x48`** (the area-transition step index; `-1`=idle, `>=0`=loading &
  advancing; getter vtable[5] `FUN_140b413c0`, idle test vtable[4] `==-1`; vtable er+0x2b6b750). Zero-RE
  anchor = **`LocalPlayer==null`** (`[er+0x3d65f88]+0x1E508`, pinned). Screen-blacked-out refinement =
  `CSFD4FadeSystem+0x2c` alpha (vtable er+0x2b6a458). Target = `PLAYER_MAPID_SLOT` (pinned). Watchdog mirrors
  `goblin_freeze_watchdog.cpp`. Findings: `windows_loading_screen_state_re_findings.md`. Runtime gap (Linux):
  resolve the LocationStep/FD4-singleton instances via find-what-accesses (reflection-lazy — not static-pinnable).
- **Terrain raycast → heightfield (procedural relief map) — SOLVED (static, 2026-07-04).** Down-ray primitive
  `FUN_140c70360(ctx, filter, start, segDir, &pt, &nrm, &dist)→hit` (Havok `hknpWorld::castRay` `FUN_14187d960`
  "TtWorldCastRay"); `ctx = *(DAT_143d76060 + 0x98)` (`CS::PhysWorld` singleton, RTTI `PhysWorld@CS@@`). END =
  start+segDir; returns ground point + surface normal (→ hillshade) + distance, in the marker world frame.
  Terrain filter = **`0x5e`** (the engine's snap-to-ground value; AEG shape cast `0x5d/0x67`, char probe `0xe0`).
  Water = per-region `GXSR WaterInteractionManager`/`WaterHeightMap` (no global plane). Findings:
  `windows_terrain_raycast_heightfield_re_findings.md`. Runtime (Linux): must call on the game thread; confirm
  `0x5e` excludes objects.
- **Water level / sea-tag source — Options 3 AND 2 BOTH RULED OUT; real path = a MATERIAL-tag (findings
  `windows_water_level_source_re_findings.md`, 2026-07-06).** The global `kSeaLevelY` heuristic is WRONG (no
  `SeaLevel` const). **Option 3 (`GXWaterHeightMap@GXSR`) = DEAD END:** a **GPU render-resource** (~7
  ref-counted textures for the water *interaction*/wave sim), sub-object of `GXSceneContext` at **+0xBE20** —
  not a CPU base-plane, and it's the ripple overlay not the surface. **Option 2 (a water-surface cast filter)
  = ALSO DEAD END:** ER's collision filter (`CSCollisionFilter@CS` vt `er+0x2b91d00`, 128-layer matrix at
  `this+0x20`, populate `FUN_140c5dab0`) shows the cast filter byte is just a **7-bit collision-layer index**
  (`0x5e`=layer 94, vs terrain layer 2) — and **water is NOT a collision layer**. Water is a ground
  **MATERIAL** (`{Center,FR,FL,RR,RL}_MatRatio_{Default,Grass,Water,Swamp}`); no water collision surface
  exists (WaterSurface/WaterMesh/PhantomWater/… = 0 hits), so no cast returns a water surface Y. **⇒ the real
  CPU-native sea-tag = MATERIAL-based:** cast `0x5e` (done) → hit triangle material (`hknpMaterialLibrary`
  er+0x2ee36b0) → `sea = material ∈ {Water, Swamp}`. Follow-up RE = raycast-hit material extraction + the
  Water/Swamp ids. That reaches only the NEAR field (streamed collision); **whole-map / far water = a DISK
  source** (`far_water_surface_disk_re_prompt.md`). **Probe 1 DONE (`far_water_surface_disk_re_findings.md`,
  2026-07-06): the MSB water-plane (Source B) is RULED OUT** — ER MSBs have no water part/region/model name,
  parts sit at origin (surface Y is in the FLVER), `HitFilterID` isn't water, overworld tiles have no MSB
  collision. ⇒ only disk water source = the `hkxpwv` collision `Water`/`Swamp` material (rides the far-terrain
  bake, mask free with elevation); remaining = Probe 2 (material ids + offline hkxpwv decode).
- **Far-terrain elevation (the "fake 3D" distant terrain) — SCOPED (static, 2026-07-05,
  `far_terrain_heightmap_re_findings.md`).** The raycast above is loaded-region-only; the distant terrain has
  no collision. **RTTI sweep verdict: there is NO far-terrain heightmap TEXTURE** (zero non-water `*Height*`/
  `*LodTerrain*` class in 9760) — ER's terrain is **FLVER meshes streamed from `.mapbnd`** (`CSMapModelIns`,
  `CSMapbndResCap`/`RepositoryImp`/`FileCap`, `CSFlverDrawSystem`); far terrain = the coarse-LOD FLVER. So the
  prompt's "resident heightmap" (shape A) doesn't exist; whole-map ELEVATION means **parsing low-LOD terrain
  FLVERs from disk** = opening the README-#4 FLVER frontier (a project, not a quick add). Cheap alternatives:
  the shaded `71_MapTile` COLOR tiles already give a visual relief backdrop (mod-baked); `WorldMapPieceParam`
  is a cheap probe to rule out a coarse per-piece elevation. **Water for the far field RIDES this bake:** the
  `hkxpwv` collision's Water/Swamp material gives the sea mask free with the elevation
  (`far_water_surface_disk_re_prompt.md`) — NOT `GXWaterHeightMap` (a GPU wave-sim, ruled out).
- **WorldMapTile placement/rect — SOLVED (static, calibration fixed 2026-07-04).** The engine positions
  tiles on a `floor(mapU/cellSize)` grid with a **FLIPPED Z axis**, base 0: `gridX = clamp(floor(mapU/cs),
  0, N-1)`, `gridZ = clamp((N-1) − floor(mapV/cs), 0, N-1)`, per-tier `cs/N = {256/41, 342/31, 1288/9}`
  (overworld = 256/41). Tile map-space rect = `(gridX·cs, (N-1-gridZ)·cs)` + `256×256` (const). `tileId =
  dim*10000 + gridX*100 + gridZ` (@`WorldMapTile+0x30`, rect @`+0x98`). Calibration fn `FUN_1408849e0`,
  cell walk `FUN_1409d9ba0`, view-rect build `FUN_1409ce7d0`. Live-read chain: `WorldMapArea`(vt
  er+0x2b2cb08, layer vec `+0x390` stride 0x110) → `WorldMapTiledLayer`(vt er+0x2b2caf0, tree `+0x230`) →
  `WorldMapTile{+0x30 id, +0x98 rect}`. Findings: `windows_worldmap_tile_rect_reach_re_findings.md`.
  ⚠ the earlier `windows_worldmap_tile_placement_re_findings.md` grid math (`col·64` morton, no Z-flip) was
  WRONG for placement (that offset by ~21/~14) — corrected/superseded; its `suffix=8·morton` only describes
  the archive FILENAME (texture-fetch, deferred). Remaining: archive name↔cell (textures), SRV recycling,
  byte-range reads.
- **LotReader index rebuild** — snapshotted at init; newly cloned lots don't resolve (`refresh_markers` v2).
- **F2 fog-locate clamp** — the reticle-clamp bounds source in the `c32f0` subtree is unfound
  (`linux_f2_fog_locate_clamp_re_findings.md`).
- **Hidden Passage** (illusory walls, no static signal — `windows_group2_landscape`), **Wandering
  Mausoleum** (dynamic entity, no static MSB signal).
- **★ 3D world-to-screen (camera view-proj) — `windows_world_to_screen_camera_re_prompt.md`.** THE unblocker
  for the runtime-modding virtual worlds (in-game ImGui/ESP overlay: world XYZ → screen → ImDrawList). Read
  the live 4×4 view-projection (target `GameRendCameraSet` er+0x680460 / `CSCameraImp`) + resolve chain; ship
  `w2s3d(xyz)`. Reuses the freecam recon (freecam WRITES the transform, this READS the ViewProj).
  **Static recon done → `windows_world_to_screen_camera_re_findings.md`:** ViewProj candidates =
  `GameRend`/`GameRendCameraSet` instance **+0xF0 / +0x130** (two 4×4, default-init in `FUN_1406800f0`
  er+0x6800f0); alt view block via `[[cam+0x10]+0x18]+0x10`; Present hook + player-XYZ oracle already in the
  DLL. Remaining = pin the singleton anchor + confirm the matrix live (row/col-major, NDC-Y).
- **Freecam** (dev tool for the world-editor loop — recon done `windows_freecam_re_findings.md`, Route 2 =
  freeze ChrCam + override the render view matrix in `GameRendCameraSet` er+0x680460; blocked on the matrix
  offset + a `CSCameraImp` singleton AOB).
- **Havok VDB stand-up (OPTIONAL) — `windows_havok_vdb_standup_re_prompt.md`.** VDB machinery IS in the exe
  (`havok_vdb_presence_findings.md`); could give a free 3D collision render via the official client, but is
  gated on a version-matched client (~2018 Havok). SECONDARY — the ESP hknp-wireframe path avoids the version
  lock. Settle the client GO/NO-GO first.
- **ER frame bottleneck profiling (name the mono-thread hot path) — `er_frame_bottleneck_profiling_re_idea.md`.**
  IDEA/followup. `perf` proved ER is mono-thread CPU-bound (~69% on the main thread, GPU idle, vkd3d ~4%) but
  the hot addrs are VMProtect-unresolved. Resolve them → RVA → Ghidra (decrypted dump) to NAME the dominant
  frame subsystem. ⚠ For engine understanding, NOT for the Linux<Windows fps deficit (that = wine per-call
  overhead → gamescope/Proton/governor, not one hot fn).
- **Debug-render / wireframe flag (greybox job #2a — CHEAP scan) — `windows_debug_render_flag_re_prompt.md`.**
  Does retail ER expose a leftover debug-render flag (wireframe / untextured / flat / collision-draw) that
  restyles the engine's OWN render, systems untouched? Binary GO/NO-GO strings/RTTI/globals scan (hours). GO =
  nearly-free greybox restyle; NO-GO routes to job #2(b) post-process or (c) D3D12 PSO override. Blocks nothing;
  retires the "restyle the real ER render" roadmap question. NOT the ImGui-mirror path (#3, the wall).
  **RESOLVED → `windows_debug_render_flag_re_findings.md`: NO-GO for a cheap flag.** Debug-draw classes ARE
  live in retail (`CSDbgDispStep`/`CSDbgMenuStep` steps; `CSHkDebugDisp`=`hkDebugDisplayHandler`), but there is
  **no single resident enable**: the disp-step gate `DAT_143d85b18` is a live context pointer (Linux-disproven),
  the Havok handler's display methods are stubbed (`0x80040200`), and drawing needs the `hknpViewer`/process-
  context standup — **converges with the VDB path, not a poke8**. Whole-scene restyle: no flag either. ⇒
  **Recommended collision-viz = draw hknp shapes ourselves via ESP, unblocked by `w2s3d`** (no native flag, no
  Havok-version lock). A manager global `DAT_1447dacd0` (er+0x47dacd0) + a disp-mask (inst+0x10, `0x1FFFF`) exist
  as anchors if anyone attempts the standup anyway.
- **Native world-map RENDER cull (stop the WorldMapDialog Scaleform draw) — OPEN, Windows; goal A now DEAD.**
  The vmap already covers + input-locks the native map; the last brick = stop the native map from RENDERING
  (wasted GPU under the opaque vmap). THREE levers now dead: the D3D12 scissor + the `MovieImpl+0xB0` clip
  (`windows_native_map_render_toggle_re_findings.md` §4c/§4d), and **candidate 1 the "draw vfunc no-op" —
  DISPROVEN 2026-07-06** (`windows_native_map_drawvfunc_re_findings.md`): WorldMapDialog's vtable is 13 slots,
  none submit the movie, and CSMenuManImp registers no draw task — the map draws through a **central
  CSScaleform pass on the render thread**, not a menu vtable. Remaining = **goal B, a per-movie render/visible
  gate** on the mapped `CSScaleformSwfPlayer` (0xe8 struct, `WorldMapDialog+0x140→+0x58`); pin via Linux-live
  RPM A/B of that struct (`movieclip` scaffolding) or a Ghidra render-thread trace. Gated on vmap Track A/B in
  production.
- **ER in-combat state (gate the vmap like the native map) — OPEN, Linux-first.** The native-map redirect
  (`native_map_redirect_linux_re_plan.md`) means the vmap can be stuck OPEN in combat (ER blocks the map-open
  upstream, so our create-callback hook can't toggle it). Find ER's combat/danger flag → `combat_active()` →
  force-close the vmap in combat (+ the same flag gates fast-travel-in-combat). Prompt (mem_fwa diff-scan on
  WorldChrMan/PlayerIns aggro, or the map-open combat gate): `combat_state_gate_re_prompt.md`.
- **In-game pause** (`windows_ingame_pause_re_prompt.md`), **gamepad input device**
  (`windows_gamepad_input_device_re_prompt.md`), **silent deadlock freeze** (unsolved; watchdog shipped).
- **Keybinding config (read the user's LIVE kb+pad binds) — OPEN, `windows_keybinding_config_re_prompt.md`.**
  Raw device layer + the `CSPcKeyConfig` singleton (`DAT_143d5deb8`) are already RE'd (`windows_input_path_re.md`);
  the gap = DECODE its command→binding table + the command ids, so mod hotkeys respect remapping and work on a
  pad (today = hardcoded `GetAsyncKeyState` VK, kb-only). Cheap intermediate needing NO decode: feed the
  engine's polled `XINPUT_STATE` (`DAT_1430b92e0`) into ImGui gamepad nav.
- **★ World→map-space affine RESIDENT source — ANSWERED (static), `windows_worldmap_affine_resident_source_re_findings.md`** (verdict: exe-invariant, no mod param — bias/scale `.rdata`, origin a zeroed `.data` global, keys immediates; only the legacy fold is a param. Remaining = empirical map-closed `du/dv==0` validation on Linux). Prompt: `windows_worldmap_affine_resident_source_re_prompt.md`.
  The vmap-only migration's real unblocker: project `(area,grid,pos)→map (u,v)` with the native map NEVER
  opened, so we can close the menu (stop its double draw AND input) and still place markers. The affine math +
  converter layout are SOLVED (`windows_world_to_mapspace_projection`); the only gap = where the ctor
  (`FUN_1408855b0`) / builder (`FUN_140876100`) SOURCE the per-converter `origin/bias/scale` — exe-baked
  (mod-invariant) vs a resident param (like `WorldMapLegacyConvParam`, already read by `legacy_fold`). Either
  answer drops the map-open dependency + the boot-time "silent prime" hack (prime proven to work live
  2026-07-06, but it flashes the native map). Replaces the dead M5 Scaleform-draw-cut levers.

---

## Conventions
- Platform prefix: `windows_*` = done via the Windows Ghidra/CE project (`D:\ghidra_proj2\ER`);
  `linux_*` = done via in-DLL probes on the Proton box (`src/goblin_param_scan.cpp`, RPC probes).
- A findings doc supersedes its prompt; the prompt is kept for context. Offsets are per the ERR build
  (imagebase `0x140000000`) — verify live on the deploy build (struct offsets are far more patch-stable
  than RVAs; pin code sigs, never raw RVAs — see `common.md` AOB doctrine).
- **Surviving a game patch:** `patch_diff_maintenance.md` — the build fingerprint (`[BUILD]` boot log +
  `er_version` RPC) that flags when Steam updated ER under the pinned RVAs/AOBs, and the binary-diff
  recovery recipe (BinDiff/Diaphora on the *decrypted dumps* — never the VMProtect'd on-disk exes).
