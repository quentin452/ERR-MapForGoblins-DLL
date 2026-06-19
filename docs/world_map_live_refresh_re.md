# Live world-map icon refresh — RE notes (Thread 3 follow-up)

## Goal
Make the in-game per-section toggle (F7/F8) re-render icons **while the world map
is open**, instead of only on the next map open.

## Why it's needed
The game builds the world-map icon set **once** per map-open (on the CSMenuMan
`0 → 3` loading edge) and **never re-reads `WorldMapPointParam` while open** (see
`docs/ersc_hosting_and_map_autohide.md`). The section toggle flips each injected
row's `areaNo` to 99 / back on the live param table — correct, but invisible until
the game rebuilds its icon list (i.e. reopen). To refresh live we must invoke the
game's own icon (re)build / re-filter path after the flip.

## Subsystem map (from eldenring.exe RTTI / scope strings, cleartext)
Steam path: `…/steamapps/common/ELDEN RING/Game/eldenring.exe` (87 MB, VMProtect —
a 2nd `.text` at VA `0x144c0e000`; analyse the MSVC `.text` at `0x140001000`).

- `CS::CSWorldMapPointMan` / `CSWorldMapPointManImplement` — singleton
  (`FD4Singleton<CSWorldMapPointMan, …Implement>`) that owns the placed map-point
  instances. **Primary target: a rebuild/refresh/create-all method here.**
- `CS::CSWorldMapPointManImplement::_DiscoverMapPoint` — adds/reveals one point
  (runs on grace/fragment discovery). A per-point path, not a full rebuild.
- `CSWorldMapPointIns`, `CSWorldMapDiscoveryPointIns`, `CSWorldMapReentryPointIns`
  — the per-icon instance objects built from `WorldMapPointParam` rows.
- `CS::WorldMapPointParam::_VerifyEnableEventFlag` / `_VerifyDisableEventFlag` —
  per-row visibility evaluators (gate an icon on its enable/disable event flag).
  If these are re-run on demand, an event-flag-gated design could live-refresh;
  but the build-once-on-open behaviour suggests they run only at build time.
- `CS::MapItemManImpl` — related map item/icon manager (`MAP_ITEM_NODE`, lambdas).

### Anchor string VAs (for xref in a recursive disassembler)
Computed as `rdata_VA(0x1429af000) + (file_off − rdata_file_off(0x029ae200))`:

| String | VA |
|---|---|
| `CSWorldMapPointManImplement::_DiscoverMapPoint` | `0x142b48c1c` |
| `WorldMapPointParam::_VerifyEnableEventFlag`     | `0x142bb5d84` |
| `WorldMapPointParam::_VerifyDisableEventFlag`    | `0x142bb5db4` |

These are scope/name strings; the function that LEA-references one is (or sits at)
that routine — the same pin trick used for the `ShowTutorialPopup` trampoline
(`goblin_inject.cpp`, "LEA xref to `CS::CSPopupMenu::_CanOpenTutorialParam`").

## Why it can't be finished on the Linux box
- The exe is packed; only `objdump`/`nm` are available (no Ghidra/IDA/decompiler).
- A linear `objdump -d` sweep of the 43 MB `.text` does **not** reliably resolve
  the LEA xrefs (misaligns without recursive descent / symbols) — attempted, no hit.
- Confirming a refresh call rebuilds live **without crashing** needs a debugger
  attached to the running game (Cheat Engine / x64dbg), not available headless.

## Procedure to land it (Windows, Ghidra/IDA + running game)
1. Import `eldenring.exe` into Ghidra (auto-analyse). Go to each anchor string,
   follow xrefs to the referencing function(s). Identify the
   `CSWorldMapPointManImplement` method that **iterates `WorldMapPointParam` and
   (re)creates the `CSWorldMapPointIns` set** — the map-open build routine.
2. Find how to reach it on demand: either (a) a public "rebuild/refresh" method on
   the singleton, or (b) re-drive the map-open build while state == 7.
3. Resolve it by a **patch-resilient AOB** (surrounding bytes), like the existing
   trampoline — never a hardcoded RVA (RVAs shift every patch).
4. Pin the `CSWorldMapPointMan` singleton (FD4Singleton getter / static slot).
5. Implement `goblin::refresh_world_map_icons()` in the DLL: resolve once, call the
   rebuild method on the singleton. Call it from `menu_auto_toggle_loop` right
   after `apply_section_visibility()` **only when the map is open** (CSMenuMan
   `+0xCD == 7`); on the closed path the next open already rebuilds.
6. Test live: open map → F8/F7 → icons must update with no reopen and no crash.
   Watch piece/kindling rows (areaNo 99) and the ERSC-hosting path.

## Fallbacks if no clean rebuild method exists
- **Programmatic reopen**: invoke the map menu close+open via CSMenuMan (1-frame
  flicker). Needs the map open/close entry, also an RE target but simpler.
- Keep v1 behaviour (applies on reopen) and document it.

## LIVE LEAD (2026-06-19): "Show All Grace" rebuilds the open map in real time

Empirical finding while live-testing in Cheat Engine: triggering **"Show All Grace"**
(a cheat-table grace-reveal action) **rebuilds the in-game world map live, with the
map already open** — exactly the behaviour this thread wants. This is strong evidence
that:
- A **callable live icon (re)build path exists** and can run while the map is open
  without a reopen (validates approach (a) above — no programmatic-reopen fallback
  needed).
- "Reveal all graces" works by setting the grace **discovery/event flags**, which
  then drive the per-point reveal — i.e. it almost certainly funnels through
  `_DiscoverMapPoint` (`CSWorldMapPointManImplement::_DiscoverMapPoint`,
  anchor VA `0x142b48c1c`) and/or the manager's refresh loop **`FUN_140a832a0`**
  (RVA `0xa832a0`) already identified as the per-point re-eval loop.

### Next action to convert this into `refresh_world_map_icons()`
Attach Cheat Engine / x64dbg to the running game, trigger **Show All Grace**, and
**breakpoint `FUN_140a832a0` (0xa832a0)** + `_DiscoverMapPoint` to confirm which one
fires on the live rebuild and with what args (the singleton `this` + per-point data).
Whichever fires is the on-demand rebuild entry to call from
`menu_auto_toggle_loop` after `apply_section_visibility()` (map open, CSMenuMan
`+0xCD == 7`). This is the cleanest path yet to live F7/F8 refresh — drive the same
routine "Show All Grace" uses, scoped to the toggled rows instead of all graces.
