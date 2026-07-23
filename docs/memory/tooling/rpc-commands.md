---
name: rpc-commands
description: Catalog of every in-game debug-RPC command (the runtime driver surface) + how an agent drives the live game with it
metadata:
  node_type: memory
  type: project
---

# In-game debug RPC — command catalog

The DLL runs a **dev-only TCP loopback listener** (`src/goblin_debug_rpc.cpp`) that lets an agent (or a
human) drive and inspect the LIVE game at runtime: read/write params, grant items, warp, inject input,
screenshot, rebuild markers, arm find-what-accesses breakpoints, etc. This is the primary way runtime
RE and in-game verification happen on this repo (proven on Linux/Proton; see [[mfg-rpc-driver-hardening]]
and `docs/memory/tooling/linux-runtime-re-options.md`).

**This file is the catalog** — the authoritative list of commands lives in code (the `execute()` and
`execute_input()` dispatchers in `goblin_debug_rpc.cpp`). Regenerate/verify against that file after any
change; keep this doc in sync when you add a command.

## Enabling it + the driver

- Enable: ini `[Debug] debug_rpc_port = 38700` (empty = **disabled**, the ship default). One client at
  a time, loopback only (`127.0.0.1`), line-oriented (`\n`), reply is one `ok …` / `err …` line.
- **Driver: `tools/mfg.py`** (the unified entry — wraps the older `mfg_rpc.py`/`mfg_session.py`). Port
  resolves `--port` → `$MFG_RPC_PORT` → `38700`.
  ```
  python tools/mfg.py rpc <cmd> <args…>   # one-shot against a running game
  python tools/mfg.py repl [--boot]       # interactive shell (blank-line tab = list verbs)
  python tools/mfg.py run <script> [--boot]  # replay a file of command lines (# comments ok)
  python tools/mfg.py test [names…]       # in-game regression suite (tools/rpc_tests/)
  ```
  `--boot` cold-boots ER + loads a save for the session (needs Steam already running — see
  [[mfg-rpc-driver-hardening]]).
- Input commands (`key/type/mouse_*`) run on the listener thread; everything else marshals to the
  present thread. Read the driver-gotchas note before scripting input (AZERTY scancode dance, mouse
  re-warp, auto-idle when a human touches kb/mouse): [[mfg-rpc-driver-hardening]].

## Inspect / control

| Command | Usage | What it does |
|---|---|---|
| `help` | `help` (or `?`) | One-line list of all verbs (in-band discovery). Full usages = this file. |
| `ping` | `ping` | Liveness of the listener (**not** the game — a frozen game still answers; gate on real liveness). |
| `mfg_build` | `mfg_build` | **Freshness guard.** Compile time of the RPC unit (`goblin_debug_rpc.cpp`) → detects a STALE DLL. `ping` answers even from an old DLL; `mfg_build` reveals the actual build. **Check before RPC-verifying new code** (a rebuild needs a game RESTART/hot-reload — a redeploy alone keeps the old DLL resident). ⚠ **FALSE-STALE**: only advances when *that* TU recompiles; an incremental relink of OTHER TUs leaves the stamp frozen → looks stale while the DLL is fresh. See `mfg-rpc-driver-hardening.md` (verify by behaviour or `/proc/<pid>/maps` identity). |
| `status` | `status` | Panel/map/pause/focus/hotreload state: `panel= hotreload= gen= reload_pending= map_open= menucover= paused= kbseen= fg= user_idle_ms= rpc_input_idle=`. |
| `idlediag` | `idlediag` | Why `rpc_input_idle` fires: per-source tally of what moves the auto-idle clock — `recorded[wm_input_kbd= wm_mousemove= legacy=] guard_dropped= | idle_ms= auto_idle= suspended=`. Poll twice over a gap; a counter climbing while idle = the culprit. NB mouse-move is now TALLIED but no longer moves the clock (phantom moves during headless boot were false-suspending scripted input — only key/click suspends). |
| `open_f1` | `open_f1 [0\|1\|toggle]` | Open/close/toggle the F1 overlay panel. |
| `pause` | `pause [0\|1\|toggle]` | Pause/unpause the game (PauseTheGame branch; `err` if unresolved). |
| `set` | `set <ini_key> <value>` | Set a config ini key LIVE (same keys as `MapForGoblins.ini`). |
| `screenshot` | `screenshot <path.bmp>` | Grab the swapchain to a BMP (client-pixel space — matches `mouse_*` coords). |
| `dumpmenu` | `dumpmenu [tag]` | Dump menu/worldmap-probe state to the log (`[WMPROBE]`). |
| `reload_overlay` | `reload_overlay` | Hot-reload the overlay render DLL (hotreload build only; poll `status` gen bump). |
| `death_mark` / `death_clear` / `death_state` | `death_mark` sets a STICKY manual DropSoul bloodstain at the player pos (survives `tick()`'s auto-clear); `death_clear` removes it; `death_state` one-line live dump: `store[active= manual= world= g souls= raw=area/grid]` (the mod's death-marker store) + `bloodstain[read= exists= souls= map= xyz=]` (the game's GameDataMan+0x40 flag / +0x48 record — `exists` is the ENGINE gate; souls=0 with exists=1 is a real 0-rune stain) + `player[world= g]` (live map-pos). The boot/load-timing probe — poll during a load to see when/where the marker is applied. |
| `bloodstain_probe` | `bloodstain_probe [dbg]` | Read the game's own persistent bloodstain: `exists= xyz= map= souls= read=`. `dbg` dumps the resolved chain instead — GAME_DATA_MAN match/slot er-RVAs, gdm ptr, flag byte, gdm+0x50 companion id, raw 0x40-byte record hex (2.6.2.0 layout: docs/re/windows_bloodstain_read_drift_re_findings.md). |
| `coop` | `coop` | Co-op session diag (`goblin_coop`): `count=` PlayerIns in the WorldChrMan session array, `others=`/`markers=`, `freeze_skip=` (vmap freeze skipped when >1), then `local area= w(x,z) g<group>` + per-buddy `b<i> chr= area= w(x,z) g<group>` via `get_chr_map_pos` — the exact partner-marker chain. Solo test: summon an NPC gold-sign phantom and see if it shows as a buddy. |

## Params (regulation-free live edit)

| Command | Usage | What it does |
|---|---|---|
| `param_get` | `param_get <Param> <rowId> <offset(0x..)> <type>` | Read a field by raw OFFSET. type = `u8\|s8\|u16\|s16\|u32\|s32\|f32\|f64\|u64\|s64`. |
| `param_set` | `param_set <Param> <rowId> <offset(0x..)> <type> <value>` | Write it (returns read-back). Dev command — offset-addressed. |
| `param_getf` | `param_getf <Param> <rowId> <fieldName>` | Read by field NAME (offset resolved from the live exe via the `goblin::paramedit` registry). |
| `param_setf` | `param_setf <Param> <rowId> <fieldName> <value>` | Write by name. Unknown field → `err` (extend the registry in `goblin_param_edit.cpp`). |
| `param_clone` | `param_clone <Param> <srcRowId> <newRowId>` | Add a row by cloning one (Gap B table-expand). Read-back proves it's findable. |

Registry fields today (name-addressed): `EquipParamGoods.{goodsType,sortGroupId}`,
`AssetEnvironmentGeometryParam.pickUpItemLotParamId`, `BonfireWarpParam.textId1`,
`ItemLotParam_{map,enemy}.{lotItemId01,lotItemCategory01,lotItemBasePoint01}`.

## Loot / markers / world

| Command | Usage | What it does |
|---|---|---|
| `loot_at` | `loot_at <aegRow>` | Resolve LIVE what the map's loot marker for an AssetEnvironmentGeometry row shows: `pickUpItemLotParamId → ItemLotParam_map → item name` (the exact map-build chain). Verify a repoint headless. |
| `refresh_markers` | `refresh_markers` | Force a fresh marker/bucket build so a live param edit shows on the drawn map (disk worker, async). NB a newly CLONED lot won't resolve yet (LotReader-index reset — see HANDOFF v2). |
| `vmap graces` | `vmap graces [group]` | Dump every grace marker (name / dimension group / warp `rowId`) to the `[VMGRACES]` log — a warp-target list (`rowId` is what `warp <id>` + the vmap double-click take). `group` filter: 0 OW / 1 base-underground / 2 DLC. Lists only DISCOVERED (warpable) graces; the summary reports discovered-by-group counts. |
| `vmap group` | `vmap group [0-3]` | With an arg: switch the vmap PAGE. With NO arg: REPORT the current page (0 OW / 1 base-UG / 2 DLC). The read form lets a test confirm the player-dimension auto-follow switched pages on a crossing. |
| `vmap find` | `vmap find <name\|name_id>` | Dump EVERY marker whose name matches, across ALL layers, with its provenance → `[VMFIND]` log: `[L<layer>] src=<Baked\|DiskMSB\|Live> lot=<lotId>/<lotType> area<raw> grid(gx,gz) g<group> [category] w(X,Z) <onmap\|OFFMAP>`. The tool for "why is X on the map twice / off-map": same `lotId`+layer twice = one entity DOUBLE-EMITTED (e.g. a lot present in both `ItemLotParam_map` type 1 AND `_enemy` type 2); different lotId/layer = a genuine extra source. `name` = case-insensitive substring of the in-game text, or a numeric FMG name_id for an exact match. ⚠ loot markers only exist after the native map has been opened once (`on_map_opened_path` triggers the disk-loot build). |
| `vmap ename` | `vmap ename <substr\|area gx gz>` | Dump the live MSB-scanned enemies (`g_parsed.enemies`) matching a query, each with `npc=<npcParamId> model=<c-model> area<A> grid(gx,gz) pos(x,z) -> name='<resolved>' tier=<1\|2\|3>` → `[ENAME]` log. Three ints = tile-cell filter; else case-insensitive substring on part-name OR resolved name. Reuses `build_live_bosses`' exact model-parse + `enemy_display_name`, so it shows what the boss-supplement pass sees — the tool for "why does a CLONE get its base's boss name / a duplicate boss marker": **tier=2 = bestiary codex keyed on MODEL**, so two entities sharing a chr model (e.g. Godefroy npc=47500030 + Godrick npc=47500014, both `c4750`) both resolve to the model's name ("Godrick the Grafted") despite different npcParam. |
| `warp` | `warp <graceId>` | Fast-travel to a site of grace (e.g. `1042362951` = The First Step). Must be in-world + grace unlocked. |
| `coords` | `coords` | Player position in BOTH frames: `local=` tile-local Havok (`LocalPlayer+0x6C0`, = er_console_mod's `coords`/`tp` frame) + `world=` unified marker frame (`grid*256+local`) + `area`/`grid`. The teleport-harness read + streaming-gate probe. |
| `warp_local` | `warp_local <x> <y> <z>` | Teleport to a tile-local Havok pos (the `coords` `local=` frame). WORKS (SOLVED 2026-07-07): writes the player's HAVOK BODY Vec3 directly (`*(*(LocalPlayer+0x190)+0x68)+0x70/74/78`) via `goblin::warp::teleport_coords` — the move HOLDS (unlike a raw `+0x6C0` store, an output mirror physics reclaims → the old snap-back bug). GUARDED (2026-07-07): `teleport_coords` live-checks the target column (`ground_check_sync` present-thread cast) and REFUSES when no walkable collision exists there (void / deep-water kill column / unstreamed — a hop there free-falls and the mid-fall autosave POISONS the save → every load wedges); when the check is unavailable (loading/native map open) a conservative 1500 m delta cap applies instead. Cross-map = `warp <graceId>`. |
| `warp_xyz` | `warp_xyz <worldX> <worldZ> [worldY]` | Teleport to a unified WORLD/marker XZ (converts world→tile-local then drives `teleport_coords`, same working body-write path + ground-check guard as `warp_local`). Intra-region. Live-verified (+20 m held). ⚠ its world→local delta needs `get_player_raw_pos`; a transiently invalid MapId now makes it return `err` (was: silently mis-framed → the 11 km void jump). |
| `warp_far` | `warp_far <worldX> <worldZ>` | STREAMED far teleport (unified world frame): LuaWarp to the nearest DISCOVERED overworld grace (the game's own streaming/area-load, no void risk), then ONE ground-checked hop to the exact target (state machine on the present tick, `goblin_warp.cpp`). ASYNC — reply is `ok … started`; a load runs; poll `coords` until it answers near the target (~5–15 s). Refused when no discovered grace is within ~1200 m of the target. The vmap click-to-warp falls back to this automatically when the direct hop is refused. |
| `hp` | `hp` / `hp set <v>` / `hp fill` | Read (`cur/max immortal=`) or write the player's HP (module chain from the `hp_probe` RE: `LP+0x190 → +0x138`). `hp set` clamps to `[1,max]`; `hp fill` = to max. Unlike the position mirror, this WRITE HOLDS (it's the live stat). |
| `immortal` | `immortal [0\|1\|toggle]` | Dev god-mode: per-present-frame HP top-up to max (`immortal_tick`). "Practical immortality" — survives normal combat/falls; a same-frame overkill (≥max damage applied+death-checked in one frame) or a kill-plane can still kill. No arg = report. |
| `we_scan` | `we_scan` | Build the World Editor picker lists (pickup assets + named goods) from live params; reports `assets= goods= total=`. Same scan the F1 "Browse" button runs. |
| `bundle` | `bundle <sub>` | World-bundle persistence (World Editor edits saved as TOML, re-applied at boot). Subs: `status`, `clone <param> <src> <new>`, `set <param> <row> <field> <value>`, `save [path]`, `load <path>`, `apply [path]`, `clear`. Default path `<mod>/world_bundle.toml`. |

## Inventory / custom items (Gap C / sidecar)

| Command | Usage | What it does |
|---|---|---|
| `give_item` | `give_item <id(0x..)> <qty(+grant/-remove)>` | Call AddItemFunc. id is category-encoded (goods = `0x40000000\|goodsId`). **AddItemFunc is add-only** — negative qty no-ops; N>1 single-call clamps to the ~1000 stack cap, so grant N via N× `+1`. |
| `goods_count` | `goods_count <id(0x..)>` | How many of the encoded id the player HOLDS (read-only). Reports `err not in-world` vs a real `n=0`. The sidecar clean-save oracle. |
| `strip_test` | `strip_test <id(0x..)>` | Strip round-trip WITHOUT a save: before→strip(zero node)→0→restore→before. Proves `inventory::strip_goods/restore_goods`. |
| `inv_probe` | `inv_probe` | Report the captured inventory accessor + live player chain (needs a grant this session first). |
| `fmg_set` | `fmg_set <slot> <id> <text>` | Inject/override an FMG string. Slot = BASE tier: GoodsName **10**, WeaponName 11, PlaceName 19. Do NOT use DLC/menu tiers (419/319/111…) — guarded → fast `err`, not a freeze. |
| `sidecar` | `sidecar <sub>` | Drive/verify the sidecar state store. Subs: `status`, `setkv <k> <v>`, `getkv <k>`, `addflag <id>`, `rmflag <id>`, `flags`, `additem <id(0x..)> <qty>`, `items`, `serclear`, `save`, `load`. |

## Input injection (drive menus / the map)

Coordinates are CLIENT pixels of the game window (same space as `screenshot`). Auto-idle suspends these
while a human is actively using kb/mouse (`rpc_input_idle=1` in `status`; ini `rpc_auto_idle`).

| Command | Usage |
|---|---|
| `key` | `key <name> [hold_ms]` (hold 1–5000ms; e.g. `key M`, `key Escape`) |
| `type` | `type <text>` (layout-agnostic literal text into a focused ImGui field) |
| `mouse_move` | `mouse_move <cx> <cy>` |
| `mouse_click` | `mouse_click [left\|right] [<cx> <cy>]` |
| `mouse_drag` | `mouse_drag <x0> <y0> <x1> <y1>` |
| `mouse_wheel` | `mouse_wheel <notches>` (±1..20) |

## Raw memory / find-what-accesses (runtime RE)

| Command | Usage | What it does |
|---|---|---|
| `er_base` | `er_base` | Absolute base of `eldenring.exe` so a Python RPM client can turn `er+RVA` anchors into absolute addresses for `mem_dump`/`mem_fwa` (used by `tools/hf_hook_scout.py`). |
| `er_version` | `er_version` | `eldenring.exe` file version (`a.b.c.d`) — the build fingerprint the fixed RVAs/AOBs are pinned to. Verify it matches before trusting an RVA-derived address; twin of the `[BUILD]` boot-log line (`docs/re/patch_diff_maintenance.md`). |
| `proj` | `proj <area> <gx> <gz> [px] [pz]` | Call the LIVE engine converter (`worldmap_probe::project`): raw area/grid/pos → map-space `u,v` + page. Test primitive for converter RESIDENCY — must return the same valid `u,v` AFTER the native map closes (VM cached + persists). `test_converter_residency.py` guards it; the vmap underground projection + M5 native-draw cull depend on it. `err` if the VM isn't resolved yet (map never opened this session). |
| `proj_conv` | `proj_conv <area> <gxbase> <gzbase> <ox> <oz> <bx> <bz> <scale> <gx> <gz> [px] [pz]` | **OFF-VM** projection (fd0ad45 validation): build a converter slot in OUR memory from the given exe-invariant fields (`legacyNode=0`, base affine only — legacy fold stays with `legacy_fold`) and run the engine per-converter fn `FUN_140876140` on it, the native map **never opened / no live VM**. `du/dv==0` vs the map-open `proj` reference ⇒ the affine is reproducible map-closed → the VM/prime coupling is droppable. `test_converter_offvm.py` drives it. AOB-resolves map-closed, so it works with no VM. |
| `conv_affine` | `conv_affine <area>` | Dump the LIVE per-area converter slot fields (`gxbase/gzbase/origin/bias/scale`) from the resolved VM (needs the map opened once). Capture-and-replay partner to `proj_conv`: capture live → close map → replay off-VM → assert `du/dv==0`. |
| `proj_nvm` | `proj_nvm <area> <gx> <gz> [px] [pz]` | Force the map-CLOSED **no-VM** projection (`worldmap_probe::project_no_vm`): base off-VM affine for overworld/DLC-OW, else `legacy_fold` + base affine for legacy-dungeon / underground / DLC-UG areas. Must equal the live `proj` (same `u,v,page`) for any placed area. Lets the test check off-VM+fold equivalence even with the map open (`proj` would use the cached VM and shadow this path). |
| `mem_dump` | `mem_dump <hexaddr> <len>` | Raw RPM hex-dump of an absolute address (len ≤ 256). |
| `mem_write` | `mem_write <hexaddr> <u8\|u16\|u32\|u64\|f32\|f64> <value>` | Raw in-process write to an absolute address (flips page protection if needed), returns the read-back bytes. The A/B poke lever for runtime RE. ⚠ a bad address can crash — `mem_dump` first. |
| `mem_scan_f3` | `mem_scan_f3 <x> <y> <z> [tol] [max]` | In-process scan of committed PRIVATE rw pages for a consecutive float triplet within `tol` (default 0.05) of (x,y,z) — finds every live copy of a position without touching `/proc` (safe on Linux; external full-space scans D-state Proton). 4-byte stride, ≤64 hits, 6 GB cap; also logs `[SCAN3]`. |
| `mem_scan_u32` | `mem_scan_u32 <hexval> [max]` | Same region walk for a 4-byte-aligned u32 (e.g. a MapId dword `0x3C2A2300` = m60_42_35) — the load-wedge RE tool: find every struct holding a mapId, then `mem_dump` neighborhoods for the position floats. ⚠ SELF-HITS: the needle echoes in the RPC/log buffers (hits near ASCII `mem_scan_u32` are your own machinery); common small values (e.g. `0x000B0000`) flood — prefer a before/after DIFFERENTIAL scan. Logs `[SCANU32]`. |
| `mem_fwa` | `mem_fwa <hexaddr> <len> [r\|w]` | Arm a HW find-what-accesses BP on an absolute address; trigger (e.g. a save via `warp`) → `[FWA]` logs the accessing RIP. Use a COLD target to avoid a VEH storm. |
| `equip_dump` | `equip_dump <off(0x..)> <len>` | Hex-dump `EquipGameData+off` (len ≤ 256). |
| `equip_fwa` | `equip_fwa <off(0x..)> <len> [r\|w]` | Arm FWA on `EquipGameData+off`. |
| `move_asset` | `move_asset <dx> <dy> <dz>` | LIVE test of the geom transform setter (`vtable[0xd0]`): moves a live geom instance by the delta via the engine's own virtual setter, reads back, restores. Reports `before/moved/restored` translations. Proves the MSB-write-free move primitive (`docs/re/windows_msb_placement_write_re_findings.md`). |
| `move_hold` | `move_hold <dx> <dy> <dz>` | Like `move_asset` but does NOT restore; remembers the instance (persistence probe). |
| `move_read` | `move_read` | Re-read the held instance's `+0x220` translation (poll to check the move persists). |
| `move_restore` | `move_restore` | Restore the held instance to its remembered pre-move transform. |
| `move_near` | `move_near <dx> <dy> <dz>` | Move the geom instance NEAREST the player (for an on-screen visual). NB `+0x220` is block-LOCAL per tile, so cross-tile "nearest" is approximate. |
| `move_aeg` | `move_aeg <aegRow> <dx> <dy> <dz>` | Move the nearest loaded placement of a SPECIFIC asset (AssetEnvironmentGeometry row) — targeted move (World Editor "Move this asset"). |
| `geom_dump` | `geom_dump` | Read-only recon: dump a live geom instance + its CSMsbParts record (name, aegRow, hex) to the `[GEOMDUMP]` log. For ADD-placement RE. |
| `move_all` | `move_all <dx> <dy> <dz>` | Move EVERY loaded geom instance by the delta (mass visual confirm; dirties the world). |
| `geom_stats` | `geom_stats` | Count loaded geom instances + class (vtable) histogram. Explains "some objects moved, others not": move_all only touches CSWorldGeomIns-family (~17k), never map parts (walls/terrain); LOD dupes an asset. |
| `add_collision` | `add_collision [recon \| <hx> <hy> <hz> [<x> <y> <z>] [go]]` | Route D walkable collision body, STAGED: no args = resolve hknpWorld/bodyMgr; `recon` = phase-1 layout dumps (`[ADDCOL]`); 3 half-extents = build+dump the `hknpBodyCinfo` (shape BORROWED from a live body, position defaults to player+40u), NO mutation; append `go` = allocateBody+addBody into the live broadphase. Verify with `hf_probe_present` (hit at body-top Y). In-world, map CLOSED. `docs/re/add_collision_linux_impl_brief.md`. |
| `objects` | `objects [list\|reload\|realize\|clear\|collision on\|off\|load <path>]` | The **objects.toml greybox realizer** (`goblin_objects`, virtual-world 3D backend slice 1): materialize `[[object]]` defs into mod-owned 3D boxes (`r3d`, per-axis extents). `list` = loaded defs; `realize` (IN-WORLD) = draw the greybox at each def's pos (an OFFSET from the player when `relative=true`, else absolute); `reload` = re-read `<mod>/objects.toml` + realize; `clear` = remove the rendered boxes; `collision on` = arm the EXPERIMENTAL walkable `add_collision` pass (off by default — borrowed shape + havok frame ≠ r3d frame, mis-align is slice-2). `load <path>` = realize a specific file. Boot-loaded via mod.toml `[sections] objects`. Template: `assets/objects.toml.example`. |
| `ground_at` | `ground_at <x> <z> [y_hint]` | LIVE ground-existence check at a tile-local column (the teleport guard's primitive, exposed for warp drivers/pathing): synchronous present-thread cast, wide relief window (+3000/−10000 around `y_hint`, default = player Y). `ok ground y=…` = walkable collision exists; `ok miss` = void / deep-water kill column / unstreamed (a teleport there free-falls — `teleport_coords` refuses those itself); `err unavailable` = loading / native map open / cast unresolved. In-world, map CLOSED. |

## Extending

Add a command: a new `if (cmd == "…")` branch in `execute()` (present-thread, may touch overlay/game
state) or `execute_input()` (listener-thread input). Keep the `err usage: …` convention on missing args
— that string is how this catalog stays machine-derivable. Update this file + the tooling README index.
