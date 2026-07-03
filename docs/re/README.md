# RE coverage map — what of ELDEN RING is reverse-engineered, and what isn't

Index + honest coverage map for `docs/re/`. **156 RE docs** live here. Two kinds:
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
| **World map / markers** | `WORLDMAP_POINT_FN/_CTOR`, `WORLDMAP_PROJ_LOOP/_POINT`, `WORLDMAP_CONV_BUILD`, `MARKER_ARRAY_CTOR`, `MARKER_CHAIN_SLOT` | `world_map_projection`, `windows_world_to_mapspace_projection`, `windows_worldmap_tile_fog`, `windows_worldmap_page_transition`, `windows_map_point_icon_layout`, `windows_aeg_collectible_source`, `windows_runtime_msb_resident` | Marker projection + map-space; tile fog; page switch; disk-MSB loot parse (asset→lot→item, enemy/EMEVD/collectible drops); icon textures (resident + disk) |
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
   primitive **and DURABLE** (persistence confirmed — cache-only write held ~7s, no revert). **ADD a new
   placement is now SCOPED (static):** the spawn drivers `FUN_1406a7930`/`FUN_1406adc80` are the
   tile-streaming state machine (no isolated "spawn one geom" call); instances are placement-new'd into
   fixed-capacity BlockData pools (static `+0x2b0`, dynamic `+0x2c0`) + pushed into geom_ins vector `+0x288`.
   Route 1 `spawn_clone` is BLOCKED on 3 helper decomps (the Dynamic ctor's srcType/transform args) —
   can't drive `FUN_1406b9880` blind. Linux recon done (`geom_dump`); Ghidra handoff =
   `windows_geom_spawn_re_prompt.md`. ADD is a multi-step build, not a quick primitive — the remaining hole.
2. **ESD / talk scripts (EzState) — mostly unmapped.** Merchant shop↔NPC join, dialogue, talk-driven
   logic need an EzState bytecode evaluator (shelved after a spike — see
   `docs/plans/merchant_item_search_plan.md` Slice 3).
3. **regulation.bin / mod-VFS virtualization** — for the strong form of world-swap (vision #1). Only
   `windows_regulation_modroot_anchor_re_prompt.md` scratches it; the VFS-level swap isn't RE'd.
4. **3D models / FLVER** — completely untouched (vision #3). No asset/model RE at all.
5. **Custom mob PLACEMENT** — NpcParam is param-driveable, but placing a custom enemy is MSB write (#1).

### Smaller tactical gaps (see HANDOFF)
- **LotReader index rebuild** — snapshotted at init; newly cloned lots don't resolve (`refresh_markers` v2).
- **F2 fog-locate clamp** — the reticle-clamp bounds source in the `c32f0` subtree is unfound
  (`linux_f2_fog_locate_clamp_re_findings.md`).
- **Hidden Passage** (illusory walls, no static signal — `windows_group2_landscape`), **Wandering
  Mausoleum** (dynamic entity, no static MSB signal).
- **In-game pause** (`windows_ingame_pause_re_prompt.md`), **gamepad input device**
  (`windows_gamepad_input_device_re_prompt.md`), **silent deadlock freeze** (unsolved; watchdog shipped).

---

## Conventions
- Platform prefix: `windows_*` = done via the Windows Ghidra/CE project (`D:\ghidra_proj2\ER`);
  `linux_*` = done via in-DLL probes on the Proton box (`src/goblin_param_scan.cpp`, RPC probes).
- A findings doc supersedes its prompt; the prompt is kept for context. Offsets are per the ERR build
  (imagebase `0x140000000`) — verify live on the deploy build (struct offsets are far more patch-stable
  than RVAs; pin code sigs, never raw RVAs — see `common.md` AOB doctrine).
