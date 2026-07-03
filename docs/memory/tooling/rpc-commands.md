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
| `status` | `status` | Panel/map/pause/focus/hotreload state: `panel= hotreload= gen= reload_pending= map_open= menucover= paused= kbseen= fg= user_idle_ms= rpc_input_idle=`. |
| `open_f1` | `open_f1 [0\|1\|toggle]` | Open/close/toggle the F1 overlay panel. |
| `pause` | `pause [0\|1\|toggle]` | Pause/unpause the game (PauseTheGame branch; `err` if unresolved). |
| `set` | `set <ini_key> <value>` | Set a config ini key LIVE (same keys as `MapForGoblins.ini`). |
| `screenshot` | `screenshot <path.bmp>` | Grab the swapchain to a BMP (client-pixel space — matches `mouse_*` coords). |
| `dumpmenu` | `dumpmenu [tag]` | Dump menu/worldmap-probe state to the log (`[WMPROBE]`). |
| `reload_overlay` | `reload_overlay` | Hot-reload the overlay render DLL (hotreload build only; poll `status` gen bump). |

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
| `warp` | `warp <graceId>` | Fast-travel to a site of grace (e.g. `1042362951` = The First Step). Must be in-world + grace unlocked. |
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
| `mem_dump` | `mem_dump <hexaddr> <len>` | Raw RPM hex-dump of an absolute address (len ≤ 256). |
| `mem_fwa` | `mem_fwa <hexaddr> <len> [r\|w]` | Arm a HW find-what-accesses BP on an absolute address; trigger (e.g. a save via `warp`) → `[FWA]` logs the accessing RIP. Use a COLD target to avoid a VEH storm. |
| `equip_dump` | `equip_dump <off(0x..)> <len>` | Hex-dump `EquipGameData+off` (len ≤ 256). |
| `equip_fwa` | `equip_fwa <off(0x..)> <len> [r\|w]` | Arm FWA on `EquipGameData+off`. |
| `move_asset` | `move_asset <dx> <dy> <dz>` | LIVE test of the geom transform setter (`vtable[0xd0]`): moves a live geom instance by the delta via the engine's own virtual setter, reads back, restores. Reports `before/moved/restored` translations. Proves the MSB-write-free move primitive (`docs/re/windows_msb_placement_write_re_findings.md`). |
| `move_hold` | `move_hold <dx> <dy> <dz>` | Like `move_asset` but does NOT restore; remembers the instance (persistence probe). |
| `move_read` | `move_read` | Re-read the held instance's `+0x220` translation (poll to check the move persists). |
| `move_restore` | `move_restore` | Restore the held instance to its remembered pre-move transform. |
| `move_near` | `move_near <dx> <dy> <dz>` | Move the geom instance NEAREST the player (for an on-screen visual). NB `+0x220` is block-LOCAL per tile, so cross-tile "nearest" is approximate. |
| `move_all` | `move_all <dx> <dy> <dz>` | Move EVERY loaded geom instance by the delta (mass visual confirm; dirties the world). |

## Extending

Add a command: a new `if (cmd == "…")` branch in `execute()` (present-thread, may touch overlay/game
state) or `execute_input()` (listener-thread input). Keep the `err usage: …` convention on missing args
— that string is how this catalog stays machine-derivable. Update this file + the tooling README index.
