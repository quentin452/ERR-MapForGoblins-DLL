# RE coverage map — what of ELDEN RING is reverse-engineered, and what isn't

Index + honest coverage map for `docs/re/`. **169 RE docs** live here. Two kinds:
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
| **World state / entities** | `WCM_FINDER`, `GET_CHRINS_FROM_HANDLE`, `EVENT_FLAG_MAN_SLOT(_ALT)`, `IS/SET_EVENT_FLAG`, `GEOM_FLAG_SLOT`, `WORLD_GEOM_MAN_SLOT`, `PLAYER_MAPID_SLOT` | `windows_player_pos_RESOLVED`, `windows_enemy_boss_runtime_pos`, `windows_enemy_name_runtime_source`, `windows_geom_flag_savedata_table`, `windows_fieldins_registry_layout_and_preopen` | Player/enemy/boss live position; event flags read/write; geom (collected) flags; field-instance registry/pool |
| **Nav / warp** | `LUA_WARP`, `CS_LUA_EVENT_MANAGER` | `windows_grace_warppin_teleport`, `windows_grace_warppin_setto_abi` | Grace fast-travel (dev nav) |
| **Menus / UI / present** | `CSMENUMAN_SLOT`, `CSFEMAN_SLOT`, `PAUSE_BRANCH`, `RENDER_REAPPLY_RES`, `EC_TEST_DISTANCE_VFT` | `windows_menu_item_icon`, `windows_menu_cursor_iconid_populator`, `worldmap_menu_and_native_clip`, `windows_midsession_resolution_swapchain` | Menu icon/cursor; worldmap clip; resolution swapchain; native pin suppression |
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
   not more RE. This is the last brick of the MSB-write wall for GEOM placement.
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
  Water = per-region `GXSR WaterInteractionManager`/`WaterHeightMap` (no global plane → use a sea-level
  heuristic first). Findings: `windows_terrain_raycast_heightfield_re_findings.md`. Runtime (Linux): must call
  on the game thread; confirm `0x5e` excludes objects; pick sea-level const.
- **Far-terrain elevation (the "fake 3D" distant terrain) — OPEN, `far_terrain_heightmap_re_prompt.md`.** The
  raycast above is **loaded-region-only** (it queries physics collision, streamed in a small radius). The
  distant terrain you SEE is visual LOD with **no collision** → the cast can't reach it, so a collision
  heightfield can never cover the visible far map. Full-map relief needs the far-LOD **heightmap source** the
  renderer uses (a resident height texture, or a disk no-bake asset — maybe a sibling of the `71_MapTile`
  color archive). Near field = the raycast; far field = this. Mod-agnostic (read the active install, not a bake).
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
- **Freecam** (dev tool for the world-editor loop — recon done `windows_freecam_re_findings.md`, Route 2 =
  freeze ChrCam + override the render view matrix in `GameRendCameraSet` er+0x680460; blocked on the matrix
  offset + a `CSCameraImp` singleton AOB).
- **In-game pause** (`windows_ingame_pause_re_prompt.md`), **gamepad input device**
  (`windows_gamepad_input_device_re_prompt.md`), **silent deadlock freeze** (unsolved; watchdog shipped).
- **Keybinding config (read the user's LIVE kb+pad binds) — OPEN, `windows_keybinding_config_re_prompt.md`.**
  Raw device layer + the `CSPcKeyConfig` singleton (`DAT_143d5deb8`) are already RE'd (`windows_input_path_re.md`);
  the gap = DECODE its command→binding table + the command ids, so mod hotkeys respect remapping and work on a
  pad (today = hardcoded `GetAsyncKeyState` VK, kb-only). Cheap intermediate needing NO decode: feed the
  engine's polled `XINPUT_STATE` (`DAT_1430b92e0`) into ImGui gamepad nav.

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
