# RE coverage map — what of ELDEN RING is reverse-engineered, and what isn't

Index + honest coverage map for `docs/re/`. **227 RE docs** live here. Two kinds:
- `*_re_findings.md` / `*_RESOLVED.md` — a SOLVED structure/function (the answer).
- `*_re_prompt.md` / `*_analysis.md` — an OPEN or historical RE task handed to Ghidra/CE.

The authoritative list of PINNED code sites is **`src/re_signatures.hpp`** (55 sigs; 49 health-checked by
`resolve_all_signatures` — PASS/FAIL per sig logged at boot; the rest resolve at call sites only). Live/open
work is tracked in **`docs/HANDOFF.md`**. This file is the map; those two are the source of truth on conflict.

Rule of thumb, updated 2026-08-15: **FromSoft's DATA layer is well-mapped (params, items, names, flags, save,
inventory, map-read, EMEVD script READ, ESD literal READ); the walls are GEOMETRY/PLACEMENT WRITE (new MSB
placements, character-solid collision) and SCRIPT LOGIC WRITE (ESD/EzState authoring).** "Create new content"
has nevertheless STARTED via the virtual-world route — mod-owned map pages, greybox boxes + walkable
`add_collision` bodies sidestep the MSB/FLVER/ESD walls entirely (see the Missing/frontier list + `Mapped`).

---

## ✅ Mapped (RE'd + pinned/documented)

| Subsystem | Key sigs (`re_signatures.hpp`) | Key findings | Enables |
|-----------|-------------------------------|--------------|---------|
| **Params (engine of the framework)** | `GAME_DATA_MAN`, `SOLO_PARAM_LIST` | `windows_live_paramdef_offset`, `windows_legacyconv_param_live` | Runtime clone/set/get of any param row+field, paramdef-free (offset resolved live from the exe) |
| **Inventory / items** | `ADD_ITEM_FUNC`, `INVENTORY_ACCESSOR`, `GAME_DATA_MAN` | `windows_goods_count`, `windows_runtime_asset_to_itemlot`, `windows_global_item_position_structure` | Grant/count/strip items; `GameDataMan→PlayerGameData→EquipGameData→EquipInventoryData`; ItemLotParam layout; AEG `pickUpItemLotParamId`; id encoding; grantable ceiling `0x7FFFFE` |
| **Messages / FMG** | `MSG_REPOSITORY`, `GETMESSAGE` | `windows_fmg_slot`, `windows_native_msg_getter` | Inject/override item & place names; FMG-v2 layout; base/DLC/menu slot semantics; `FmgFile_lookup` binary search; name-anchor item classification (`goods_is_map` "Map:" prefix) |
| **Save** | `SAVE_FN`, `SERIALIZE_FN` | `windows_save_serialize`, `linux_save_function`, `windows_save_function_rpm` | Whole-slot serialize hook; strip/reinject save-clean bracket; sidecar `.mfg` |
| **World map / markers** | `WORLDMAP_POINT_FN/_CTOR`, `WORLDMAP_PROJ_LOOP/_POINT`, `WORLDMAP_CONV_BUILD`, `MARKER_ARRAY_CTOR`, `MARKER_CHAIN_SLOT` | `world_map_projection`, `windows_world_to_mapspace_projection`, `windows_worldmap_tile_fog`, `windows_worldmap_tile_placement`, `windows_worldmap_page_transition`, `windows_map_point_icon_layout`, `windows_aeg_collectible_source`, `windows_runtime_msb_resident` | Marker projection + map-space; tile fog; **tile ART placement (name→256-cell grid decode)**; page switch; disk-MSB loot parse (asset→lot→item, enemy/EMEVD/collectible drops); icon textures (resident + disk) |
| **World state / entities** | `WCM_FINDER`, `GET_CHRINS_FROM_HANDLE`, `EVENT_FLAG_MAN_SLOT(_ALT)`, `IS/SET_EVENT_FLAG`, `GEOM_FLAG_SLOT`, `WORLD_GEOM_MAN_SLOT`, `PLAYER_MAPID_SLOT` | `windows_player_pos_RESOLVED`, `windows_enemy_boss_runtime_pos`, `windows_enemy_name_runtime_source`, `windows_enemy_name_hud_feed` (superseded by the native-tag path), `windows_geom_flag_savedata_table`, `windows_fieldins_registry_layout_and_preopen`, `cross_mod_boss_naming` (**SOLVED + WIRED** — mod-agnostic boss enumeration via EMEVD `2003[011] HandleBossHealthBar` × MSB placement × GameAreaParam; incl. the common_func **template `90005870`** call sites; tier-4 seeding in `build_live_bosses`; live-verified on Golden Age: 290 boss bars, Tree Sentinel marker fixed) | Player/enemy/boss live position; event flags read/write; geom (collected) flags; field-instance registry/pool; **mod-agnostic boss list (name+pos+identity), shipped** |
| **EMEVD (script READ)** | (parsers, not AOBs) | `windows_emevd_*` corpus; `windows_emevd_condition_evaluator_re_findings.md` (**SOLVED static** — T1 dispatcher `FUN_140567d40`, `CSEmkCondition::Evaluate` = vtable slot 1, item test `FUN_14057c8e0`; G1 tracer scoped, one live confirmation pending) | Boss bars + defeat flags, quest NPCs, world-feature flags, EMEVD drops, gate tables (fragment/story) — all mod-agnostic, read from the ACTIVE install's `event\` dir (capture-first, dir-keyed cache) |
| **ESD / talk scripts (literal READ)** | (runtime C++ parser, not AOBs) | `esd_ezstate_decoder_re_findings.md` (78% literal `82 <i32> A1` args), `src/worldmap/esd_parser.cpp` (runtime walker: fsSL/fSSL, states/conditions/commands, BND4 talkbnd) | Merchant-shop→NPC pin join (39 merchants, live), talk-ESD reading from the mod's own dir (captured path first) |
| **Nav / warp** | `LUA_WARP`, `CS_LUA_EVENT_MANAGER` | `windows_grace_warppin_teleport`, `windows_grace_warppin_setto_abi`, `linux_player_pos_write_setpos` (coord teleport SOLVED + live-verified: raw +0x6C0 = output mirror → snap-back; the WORKING write is the havok body Vec3 `*(*(LocalPlayer+0x190)+0x68)+0x70/74/78`; `SetPos` er+0xdc6380 RE'd but incomplete from the RPC thread) | Grace fast-travel (dev nav); coordinate teleport (`warp_local`/`warp_xyz`/vmap click); `warp_far` streamed far teleport |
| **Menus / UI / present** | `CSMENUMAN_SLOT`, `CSFEMAN_SLOT`, `PAUSE_BRANCH`, `RENDER_REAPPLY_RES`, `EC_TEST_DISTANCE_VFT` | `windows_menu_item_icon`, `windows_menu_cursor_iconid_populator`, `worldmap_menu_and_native_clip`, `windows_midsession_resolution_swapchain`, `game_timestep_freeze` (**SHIPPED** — FD4Time is a per-group arg, NOT a pokeable global; dt is a pure `xmm1` register arg, so the planned dt-zero was RET-proven a no-op and removed; the shipped freeze uses `SetDisableAllChrUpdate` via `goblin_pause.cpp`, freeze-reason mask, user-verified), `windows_ingame_pause` (branch-flip pause REMOVED — resume hitch grew with duration) | Menu icon/cursor; worldmap clip; resolution swapchain; native pin suppression; **in-game freeze / timestep stop (vmap-redirect + manual)** |
| **Native-map redirect / render cull** | `WORLDMAP_CREATE_CB` (call-site-resolved) | `native_map_redirect_linux_re_plan.md` — create-callback hooked, the native map NEVER opens = render-cull for free (the old goal-B Scaleform gate hunt ran NEGATIVE and is superseded) | vmap stands in for the native map; no native draw underneath, no gate hunt |
| **3D world-to-screen (camera)** | (camera chain, call-site-resolved) | `windows_world_to_screen_camera_re_findings.md` + `src/goblin_w2s.cpp` (**SHIPPED + LIVE-CONFIRMED** — `camMgr = *(er+0x3d6b880)` → `GameRend+0x18` → `camObj+0x10` POSE + `camObj+0x50` lens; NOTE: the earlier `GameRend+0xF0` candidate was the PLAYER pose, corrected) | `w2s3d(xyz)` world→screen; in-game ImGui/ESP overlay; greybox box render; motion-sync camera ring |
| **World geometry MOVE** | `SET_WORLD_MATRIX_FN`, `CINFO_INIT_FN`, `ALLOCATE_BODY_FN`, `ADD_BODY_FN` | `windows_geom_spawn_re_findings.md` — transform setter = `vtable[0xd0] SetWorldMatrix(self, mat4x4)`; `move_asset`/`move_all` RPCs, durable (cache-only write held, no revert), renders | Move ANY live `CSWorldGeomIns` by exact delta — dev/editor primitive |
| **Collision ADD (walkable box)** | `CINFO_INIT_FN`, `ALLOCATE_BODY_FN`, `ADD_BODY_FN` | `hknpworld_addbody_slot_re_findings.md` + `add_collision_linux_impl_brief.md` + `src/goblin_add_collision.cpp` — real `hknpBoxShape` (TOML half-extents), `allocateBody` → `filterInfo` stamp → `addBody`; RPC `add_collision`; **ray-collidable, NOT yet character-solid** (`windows_add_collision_character_solid_re_findings.md` — open lead: broadphase-path split on `body+0x44 & 2`) | Walkable greybox boxes in the virtual-world route |
| **Bonfire / misc params** | `BONFIRE_TEXTID1_ACCESS`, `GOODS_TYPE_ACCESS`, `GOODS_SORT_GROUP_ACCESS`, `AEG_PICKUP_LOT_ACCESS`, `AEG_REPICK_BIT_ACCESS` | (field AOBs, `windows_itemlot_field_re_prompt` for the lot fields) | Named field edits over the param-override registry |
| **Input (Windows wheel)** | (input module, no AOB) | `src/input/input_rawinput.cpp` — the game makes ZERO `GetRawInput*` calls while the map/overlay is up; wheel now via a passive `WH_MOUSE_LL` hook (no raw-input registration ever touched — the earlier `RIDEV_INPUTSINK` registration REPLACED the game's per-device registration and broke its mouse) | Mouse-wheel zoom in the vmap/panel on Windows; smooth per-notch delivery; game's raw mouse unaffected |
| **Virtual worlds / greybox** | (mod-owned, no AOB) | `src/goblin_virtual_world.cpp`, `docs/plans/virtual_world_multi_world_design.md`, `src/goblin_objects.cpp` + `src/goblin_r3d.cpp` (D3D12 backend), `objects.toml` | Custom mod map pages; greybox boxes rendered in-world; sidesteps the MSB/FLVER/ESD walls |

---

## ❌ Missing / frontier (ranked by framework impact)

1. **MSB WRITE — still the big hole, now precisely characterized.** We READ disk MSB placements and MOVE
   live placements; there is NO way to ADD a new placement (a treasure/mob at NEW coords) into the engine's
   resident world. Blocks new map content + vision #3. Status per layer:
   - **MOVE = CRACKED + LIVE-VERIFIED + DURABLE (2026-07-03).** Transform setter `vtable[0xd0] SetWorldMatrix(self,
     mat4x4)` on a live `CSWorldGeomIns` (via `move_asset` RPC) moves the cached world matrix (`inst+0x220`) by
     the exact delta, no crash; persistence confirmed (held ~7 s, no revert); renders (`move_all` mass move).
     This is the "move existing placement" primitive — solved.
   - **ADD — the standalone-ctor route is a DOCUMENTED DEAD END (2026-08, `windows_geom_spawn_builder_re_findings.md`).**
     The pose-descriptor builder `thunk_FUN_144cbdae7` is MSVC-EH-wrapped and streaming-context-welded —
     calling it standalone HANGS the game (contextual, not an arg bug). `spawn_clone` (`goblin_geom_move.cpp`)
     deliberately never calls it (a live test froze the game) and returns `DEAD-END standalone spawn`. The
     recommended pivots: **(a) the streaming-thread spawn hook `FUN_1406a7930`** (the tile-streaming state
     machine — drive it as the engine does) and **(b) the asset-request path `FUN_1406a5080`** (fully RE'd,
     reqMgr resolves live, but game-thread-bound; the per-frame injection point exists: registrar update
     `FUN_1406d31f0` on the world-geom thread — hook fires, by-id helper accepts the request, but the
     request→instance servicing `FUN_1406c6050` state machine / proximity step does NOT complete yet:
     `linux_geom_spawn_request_servicing_re_prompt.md` — **LAST GAP, still open**).
   - **Character-solidity of added collision** is a second open gap (see the collision ADD row above).
2. **ESD / talk scripts — READ side shipped, WRITE side still open.** The runtime C++ parser
   (`src/worldmap/esd_parser.cpp`) decodes the **78% literal** `82 <i32> A1` args (shop-id ranges, textIds,
   event-flag ids — the merchant-shop→NPC join is live, 39 merchants). The **remaining 21% branching
   expressions** (operators `0x40–0x50`, calls `0x6F`) need a bounded port of SoulsFormats' **EzSemble**
   disassembler (not novel RE). Still hard/strategic: authoring NEW ESD (needs the assembler + ERR re-ships
   the 524 KB talkesdbnd every version) and a runtime C++ evaluator.
3. **regulation.bin / mod-VFS virtualization** — for the strong form of world-swap (vision #1). Only
   `windows_regulation_modroot_anchor_re_prompt.md` scratches it; the VFS-level swap isn't RE'd.
4. **3D models / FLVER — CREATION options MAPPED; actual authoring untouched.** A **walkable greybox needs NO
   authoring** (Route D — a Havok `hknpBoxShape` collision box, §4b — now SHIPPED via `add_collision`); a
   **visible first-class AEG asset** does need `.flver`+`.hkx` (option A), and a NEW `AEG###_###` id **DOES
   stream** at runtime (name-resolved against the mounted VFS). Settled dead ends: **B** (register geometry
   from an in-memory buffer) is impossible (engine resolves by resource NAME only); **C** (a high-level
   engine primitive with collision, no mesh) doesn't exist — the MSB/World "Hit" collision layer is itself
   model-name driven. Actual FLVER/mesh authoring RE (the offline pipeline) is still untouched.
4b. **Terrain / Havok collision WRITE — READ side shipped, WRITE partly shipped.** We READ the terrain
    (down-ray heightfield `FUN_140c70360` → `hknpWorld::castRay`, filter `0x5e`) and now ADD dynamic bodies
    (`hknpBoxShape` via `add_collision`, see the Mapped row). What's still missing: **deforming existing
    terrain** (baked `hknpCompressedMeshShape` → Route A near-certain dead end) and **character-solidity of
    added bodies** (`windows_add_collision_character_solid_re_findings.md` — "necessary but not sufficient",
    open lead = `body+0x44 & 2` broadphase-path split). Terrain is baked `hknpCompressedMeshShape`
    (vtable er+0x2eeb908); an editable `hknpHeightFieldShape` er+0x2ee2a18 exists (a live shape-vtable read
    decides A vs B). `addBody` is NOT a vtable slot — hknp uses a deferred command buffer (opcode 1); the
    callable recipe: `FUN_1418aabf0` (allocateBody) → `FUN_1418a9ff0` (addBody).
5. **Custom mob PLACEMENT** — NpcParam is param-driveable, but placing a custom enemy is MSB write (#1).

### Smaller tactical gaps (see HANDOFF)
- **EMEVD condition-group evaluator (native logic oracle) — SOLVED (static, 2026-08, `windows_emevd_condition_evaluator_re_findings.md`).**
  T1 dispatcher `FUN_140567d40`; T2 conditions are heap objects `CSEmkCondition` — `Evaluate` = vtable slot 1,
  group container `CSEmkEventIns+0x40`; T3 item test `FUN_14057c8e0`. G1 = YES (statically; ONE live
  confirmation pending), G2 = NO. Next = scope the G1 tracer (one hook at `EMEVD_DISPATCH` rva 0x567d40;
  8 AOBs proposed in the findings). Opened 2026-07-28 for the runtime randomizer.
- **Loading-screen / world-load state — SOLVED + SHIPPED (2026-07-04 static; watchdog live).** Load-in-progress
  flag + phase = **`CSFD4LocationStep+0x48`** (the area-transition step index; getter vtable[5]
  `FUN_140b413c0`, idle test vtable[4] `==-1`; vtable er+0x2b6b750). Production watchdog (`goblin_load_watchdog.cpp`)
  uses the zero-RE anchor **`LocalPlayer==null`** (`[er+0x3d65f88]+0x1E508`) + lazy MapId-slot resolve — the
  precise LocationStep path is documented but not needed live. Screen-blackout = `CSFD4FadeSystem+0x2c` alpha.
- **Terrain raycast → heightfield (procedural relief map) — SOLVED (static, shipped as `hf_*` RPCs).** Down-ray
  `FUN_140c70360(ctx, filter, start, segDir, &pt, &nrm, &dist)→hit`; `ctx = *(DAT_143d76060 + 0x98)`
  (`CS::PhysWorld` singleton). Terrain filter = **`0x5e`**; water = per-region `GXSR WaterInteractionManager`/
  `WaterHeightMap` (no global plane). Runtime (Linux): must call on the game thread.
- **Water level / sea-tag source — real path = a MATERIAL-tag; offline decode VALIDATED, Oodle + DLC RSA are
  the walls (`windows_water_level_source_re_findings.md`, `far_water_surface_disk_re_findings.md`, 2026-07-06).**
  Options 3 (GPU `GXWaterHeightMap`) and 2 (water collision layer) are BOTH DEAD ENDS — water is NOT a
  collision layer, it's a ground **MATERIAL** (`{Center,FR,FL,RR,RL}_MatRatio_{Default,Grass,Water,Swamp}`).
  ⇒ sea-tag = cast `0x5e` → hit triangle material (`hknpMaterialLibrary` er+0x2ee36b0) → `sea = material ∈
  {Water, Swamp}`. Far/whole-map water = the **`hkxpwv` collision** Water/Swamp material (rides the far-terrain
  bake) — the offline dvdbnd→collision reader chain is validated on Linux; remaining = material ids +
  DLC (m40-43) RSA key (offline unreadable). In-process Oodle bridge exists (`dcx_file` RPC).
- **Far-terrain elevation — no heightmap TEXTURE exists; far terrain = coarse-LOD FLVER meshes from `.mapbnd`**
  (`far_terrain_heightmap_re_findings.md`). Whole-map ELEVATION = parsing low-LOD terrain FLVERs (a project).
  **NEW cheap route shipped: `far_relief`/`far_relief_probe` RPC** — MSB-placement Y-cloud relief per overworld
  tile, free (no Havok, no new parse). Water for the far field RIDES the `hkxpwv` bake, NOT `GXWaterHeightMap`.
- **WorldMapTile placement/rect — SOLVED (static, calibration fixed 2026-07-04).** `gridX = clamp(floor(mapU/cs),
  0, N-1)`, `gridZ = clamp((N-1) − floor(mapV/cs), 0, N-1)`, per-tier `cs/N = {256/41, 342/31, 1288/9}`;
  rect = `(gridX·cs, (N-1-gridZ)·cs)` + `256×256`; `tileId = dim*10000 + gridX*100 + gridZ`. Calibration fn
  `FUN_1408849e0`, cell walk `FUN_1409d9ba0`, view-rect `FUN_1409ce7d0`. ⚠ the earlier
  `windows_worldmap_tile_placement_re_findings.md` grid math (`col·64` morton, no Z-flip) was WRONG for
  placement — superseded (its `suffix=8·morton` only describes the archive FILENAME). **Archive format CRACKED**
  (`map_tile_loading_plan.md`, slice 2 map art on canvas, slice-3a live map→world transform done); remaining =
  decode + SRV recycling.
- **F2 fog-locate clamp** — the reticle-clamp bounds source in the `c32f0` subtree is unfound
  (`linux_f2_fog_locate_clamp_re_findings.md`).
- **Hidden Passage** (illusory walls, no static signal), **Wandering Mausoleum** (dynamic entity, no static
  MSB signal).
- **Freecam** (dev tool for the world-editor loop — recon done `windows_freecam_re_findings.md`; Route 2 =
  freeze ChrCam + override the render view matrix; the historical blockers "matrix offset + CSCameraImp
  singleton" are now ANSWERED in fact by the w2s camera chain — `goblin_w2s.cpp` — so a freecam build can
  reuse it).
- **Havok VDB stand-up (OPTIONAL) — `windows_havok_vdb_standup_re_prompt.md`.** VDB machinery IS in the exe
  (`havok_vdb_presence_findings.md`); gated on a version-matched client (~2018 Havok). SECONDARY — the ESP
  hknp-wireframe path avoids the version lock (unblocked by w2s).
- **ER frame bottleneck profiling** (`er_frame_bottleneck_profiling_re_idea.md`). IDEA/followup. ER is
  mono-thread CPU-bound (~69% on the main thread) but the hot addrs are VMProtect-unresolved. For engine
  understanding, NOT the Linux<Windows fps deficit (that = wine per-call overhead).
- **Debug-render / wireframe flag — RESOLVED NO-GO for a cheap flag** (`windows_debug_render_flag_re_findings.md`).
  Debug-draw classes ARE live in retail, but there is **no single resident enable**; drawing needs the
  `hknpViewer`/process-context standup (converges with the VDB path). ⇒ Recommended collision-viz = draw hknp
  shapes ourselves via ESP (unblocked by w2s). Anchors: manager global `DAT_1447dacd0` (er+0x47dacd0) +
  disp-mask (inst+0x10, `0x1FFFF`).
- **Native world-map RENDER cull — SUPERSEDED by the native-map redirect (v2.2.0).** The old goal-B
  `CSScaleformSwfPlayer` render-gate hunt ran NEGATIVE (`sfplayer` probe) — but the redirect makes it moot:
  the create-callback is hooked and the native map NEVER opens, so no native draw happens under the vmap.
- **ER in-combat state — track ABANDONED, replaced by the world-freeze.** The per-entity `IsBattleState`
  (`*(int*)(*(ChrIns+0xC950)+0x30C)==6`) pin stood, but per-enemy detection died on the AI-FSM offset
  (no fixed EnemyIns→CSAiThink offset). The shipped vmap redirect instead FREEZES all characters while the
  vmap is open (`SetDisableAllChrUpdate` — ER's own cutscene freeze) → no combat can start → the map is always
  openable. `combat_active()` survives only as a diag verb.
- **In-game pause** (`goblin_pause.cpp` — shipped: branch-flip pause REMOVED, freeze is the mechanism),
  **gamepad input device** (`windows_gamepad_input_device_re_prompt.md` — shipped: XInput/DInput hooks +
  pad nav, v2.7/2.8), **silent deadlock freeze** (unsolved; watchdog shipped).
- **Keybinding config (read the user's LIVE kb+pad binds) — OPEN, `windows_keybinding_config_re_prompt.md`.**
  Raw device layer + `CSPcKeyConfig` singleton (`DAT_143d5deb8`) are RE'd (`windows_input_path_re.md`);
  the gap = DECODE its command→binding table + command ids. The README's old "cheap intermediate" (feed
  polled `XINPUT_STATE` `DAT_1430b92e0` into ImGui gamepad nav) is DONE (`src/input/input_gamepad.cpp`).
- **★ World→map-space affine RESIDENT source — ANSWERED (static), map-closed VALIDATION DONE + SHIPPED.**
  `windows_worldmap_affine_resident_source_re_findings.md`: exe-invariant, no mod param (bias/scale
  `.rdata`, origin a zeroed `.data` global, keys immediates; only the legacy fold is a param). The off-VM
  base-affine fallback is shipped (`88ab933`) — projection works with the native map CLOSED, killing the
  map-open dependency + the boot-time "silent prime" hack.

---

## Conventions
- Platform prefix: `windows_*` = done via the Windows Ghidra/CE project (`D:\ghidra_proj2\ER`);
  `linux_*` = done via in-DLL probes on the Proton box (`src/goblin_param_scan.cpp`, RPC probes).
- A findings doc supersedes its prompt; the prompt is kept for context. Offsets are per the ERR build
  (imagebase `0x140000000`) — verify live on the deploy build (struct offsets are far more patch-stable
  than RVAs; pin code sigs, never raw RVAs — see `docs/memory/common.md` AOB doctrine).
- **Surviving a game patch:** `patch_diff_maintenance.md` — the build fingerprint (`[BUILD]` boot log +
  `er_version` RPC) that flags when Steam updated ER under the pinned RVAs/AOBs, and the binary-diff
  recovery recipe (BinDiff/Diaphora on the *decrypted dumps* — never the VMProtect'd on-disk exes).
