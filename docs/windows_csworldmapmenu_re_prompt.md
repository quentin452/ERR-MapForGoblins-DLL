# Task: RE CSWorldMapMenu — live icon refresh + map cursor (Windows, Ghidra)

You are on **Windows** with **Ghidra/IDA** and a **debugger that attaches to the running
game** (Cheat Engine + x64dbg). Repo: **ERR-MapForGoblins-DLL** (Elden Ring world-map icon
mod). This task unlocks **two** mod features that turned out to share **one** subsystem —
`CS::CSWorldMapMenu` / `CSWorldMapPointManImplement`. Do them in one Ghidra session.

Supersedes/extends `docs/windows_live_refresh_re_prompt.md` (read that too — its subsystem
map + anchor string VAs still apply). Branches in play: `feat/section-toggle` (in-game
per-section toggle) and `feat/clustering` (marker clustering); both need this RE.

## The shared problem

The mod hides/shows markers by flipping each `WorldMapPointParam` row's `areaNo` to 99
(hide) / back (show) on the live param table — used by BOTH the per-section toggle (F7/F8)
and clustering (F6 expand, collapse). **But the game builds the world-map icon set ONCE
per map-open (the CSMenuMan `0→3` loading edge) and never re-reads `WorldMapPointParam`
while the map is open** (see `docs/ersc_hosting_and_map_autohide.md`). So any toggle made
while the map is open only shows on the next reopen.

We need two capabilities, both inside `CSWorldMapMenu`:

1. **Live refresh** — re-run the game's icon (re)build/re-filter after we flip `areaNo`, so
   section/cluster toggles update the open map with no reopen.
2. **Map cursor position** — read the world-map cursor's position (in map/marker space) so
   clustering can expand the cluster the player is looking at (proximity clustering). The
   cursor is stable and already in marker coordinate space — far cleaner than the player's
   world coords (which are chunk-local; the "global" read is unstable/NaN — see below).

## Goal 1 — live icon refresh

Find the routine that **(re)builds or re-filters the world-map icon set from
`WorldMapPointParam`** and a way to invoke it on demand while the map is open.

Anchors (cleartext RTTI/scope strings in `eldenring.exe`; analyse the MSVC `.text` at VA
`0x140001000`, NOT the VMProtect `.text` at `0x144c0e000`):
- `CS::CSWorldMapPointManImplement` — `FD4Singleton`, owns the placed `CSWorldMapPointIns`.
  **Primary target: a rebuild/refresh/create-all method here.**
- `CS::CSWorldMapPointManImplement::_DiscoverMapPoint` — reveals ONE point (per-point xref
  neighbour). String VA `0x142b48c1c`.
- `CS::WorldMapPointParam::_VerifyEnableEventFlag` / `_VerifyDisableEventFlag` — per-row
  visibility evaluators. String VAs `0x142bb5d84` / `0x142bb5db4`.
- `CSWorldMapPointIns`, `CSWorldMapDiscoveryPointIns`, `CSWorldMapReentryPointIns` — the
  per-icon instances built from param rows.

Method: import exe → auto-analyse → go to each anchor string → follow xrefs (the same LEA-
to-class-string pin used for the existing `ShowTutorialPopup` trampoline; see the comment
in `src/goblin_inject.cpp`). Identify the method that iterates `WorldMapPointParam` and
(re)creates the `CSWorldMapPointIns` set. Find how to reach it on demand: a public
rebuild/refresh method on the singleton, or re-driving the build while map state == 7
(`CSMenuMan + 0xCD`, already AOB-resolved by the mod).

## Goal 2 — map cursor position

Find the **world-map cursor's current position** in the `CSWorldMapMenu` instance (the
moving reticle on the map). We want it in the SAME space as `WorldMapPointParam` posX/posZ
+ gridXNo/gridZNo (see "coordinate facts" below). Look in the `CSWorldMapMenu` instance for
the cursor/camera/center fields; cross-ref the params `worldMapCursorSpeed` /
`worldMapCursorSnapRadius` (consumers of these touch the cursor state). Deliver the offset
chain from the menu singleton to the cursor X/Z (+ the menu singleton getter).

(Why not player position: the WorldChrMan chain is already RE'd — see below — but the live
player coords are chunk-local and the world/"global" read is a CE-computed buffer that goes
NaN live. The cursor avoids all that by being in map space.)

## Already-known facts (use these — don't re-derive)

- **CSMenuMan world-map-open byte** at `CSMenuMan + 0xCD` (== 7 when world map open; `0→3→7`
  on open). Singleton AOB `48 8B 05 ?? ?? ?? ?? 33 DB 48 89 74 24` (`{{3,7}}` RIP-rel).
- **WorldChrMan / player position** (extracted from Hexinton all-in-one CT v6.0; reader is
  committed DORMANT as `goblin::get_player_world_pos` in `src/goblin_inject.cpp`):
  - WorldChrMan static: AOB `48 8B FA 0F 11 41 70 48 8B 05`; the trailing `48 8B 05` is
    `mov rax,[rip+disp32]` at +7 (ends +0xE) → static slot = `finder + 0xE + *(i32*)(finder+0xA)`.
  - Live local (chunk-relative) pos: `[[[[[WorldChrMan]+0x10EF8]+0]+0x190]+0x68]+0x70/74/78`
    = X/Z/Y (order X,Z,Y). LocalPlayerOffset `0x10EF8`.
  - "Global" coords in the CT are a CE buffer (go NaN live) — not a clean live read.
- **Coordinate facts (decoded from our marker data):** marker `posX/posZ` are **tile-LOCAL**
  (bounded ±~500, map cell ≈ 1000 units), NOT world coords; `gridXNo/gridZNo` = the tile
  index; world tile `XX = gridXNo/2`, `YY = gridZNo/2` (each `m60_XX_YY` = a 2×2 grid-cell
  block, from `src/goblin/goblin_map_tiles.hpp` comments). So markers live in (tile +
  tile-local) space; the cursor read should resolve to this same space.

## Resolve by AOB, not RVA

RVAs shift every game patch (the May-2026 patch moved `ShowTutorialPopup` from 0x80DA50 to
0x80D960). Pin every function + singleton getter by a stable surrounding-byte signature, the
way the mod already does (`modutils::scan<>`). Give the resolved RVAs only as reference.

## Deliverables

1. **Goal 1:** AOB for the icon rebuild/refresh method + the `CSWorldMapPointMan` singleton
   getter; its signature (this/args/return) + preconditions (thread? map state == 7?). A
   minimal `modutils::scan`-style, SEH-guarded C++ snippet for
   `goblin::refresh_world_map_icons()` we call from `menu_auto_toggle_loop` right after the
   `areaNo` flip when `CSMenuMan+0xCD == 7`.
2. **Goal 2:** the menu-singleton → cursor X/Z offset chain (+ singleton AOB), with a C++
   reader snippet, and a note on which coordinate space it returns (confirm vs marker
   posX/posZ + gridXNo/gridZNo).
3. **Runtime evidence** (debugger/screenshot): calling refresh live updates icons with no
   reopen and no crash (toggle a row's areaNo, then call); and the cursor read tracks the
   reticle and matches marker space.
4. If a clean rebuild method doesn't exist, deliver the **fallback**: the map-menu
   close+open entry for a programmatic reopen (1-frame flicker), same AOB format. State
   which path you delivered.

Caveats: confirm everything against the running game (offsets are version-specific). Watch
the ERSC-hosting path and the collected-piece / kindling areaNo-99 rows (don't fight them).
