# Windows RE — capture the world-map POINT pipeline & deliver `refresh_world_map_icons()`

**For:** an agent on **Windows** with **Ghidra/IDA** + a debugger that can attach to the running
game (x64dbg / Cheat Engine with WORKING execute breakpoints). Repo: **ERR-MapForGoblins-DLL**
(Elden Ring world-map icon mod, DLL via Mod Engine / ERSC). Work lands on **master**.

## Why this prompt exists
We need the game's **live world-map icon (re)build** path so injected code can refresh icons
**while the map is open** (no reopen). This unblocks at once: live section/category toggle,
runtime clustering, and proximity clusters v2 (player pos already SOLVED — `manager+0x70/+0x78`,
marker space, see `docs/re_findings_playerpos.md`).

Prior static work (`docs/world_map_live_refresh_re.md`, `docs/windows_re_live_refresh_grace_lead.md`,
Ghidra `D:\ghidra_proj2\ER`, scripts re_v31–v37) mapped the subsystem and **ruled out** the
"Show All Graces" byte (that's map-FRAGMENT/terrain reveal — `WorldMapPieceParam`, disjoint from
points). The remaining gaps are below. **A Linux Cheat-Engine session this round CONFIRMED several
facts but its execute-breakpoints FREEZE the game under Wine (no working `continueFromBreakpoint`),
so the runtime steps must be done on Windows.**

## CONFIRMED LIVE (app 2.6.2.0 / ERR 2.2.9.6 via Mod Engine, this session)
- **PointMan singleton:** slot RVA `0x3d6e9d8` → value `0x143d6e9f0` (note: object sits in module
  static data, not heap). `*(singleton+0x00)` vtable = `0x142b48c68` = **CSWorldMapPointManImplement**
  (the `_DiscoverMapPoint` scope string is at `0x142b48c1c`, adjacent). So the singleton/slot is RIGHT.
- **`[singleton+8]` is NOT the instance tree** — it reads `0` even with the map state warm. (The old
  "RB-tree root at +8" note is wrong for this build, or that field is empty.) `singleton+0x18` =
  `0x144842c40` (non-null sub-pointer — candidate sub-manager). The instance collection offset is
  UNKNOWN and is deliverable #1.
- **Execute BPs work** (the reconcile BP froze = it hit). `FUN_140a832a0` (reconcile) fires every
  frame; `FUN_140a811e0` (ctor) did NOT fire on map *reopen* → **instances are built once and persist**
  across map open/close (only the per-frame reconcile re-shows them). `_DiscoverMapPoint` (`0xa84080`)
  fires only on NEW discovery, not redisplay.
- **Our hide mechanism:** the DLL flips `WORLD_MAP_POINT_PARAM_ST.areaNo` (offset `0x20`) to 99 on the
  live param table. It shows on **reopen only** → the built instance does NOT re-read the param row's
  areaNo each frame; the reopen rebuild re-reads params. So a live areaNo edit is currently invisible.

## Known function map (resolve all by AOB, RVAs drift)
| thing | RVA | role |
|---|---|---|
| per-frame driver | `0x623410` (`FUN_140623410`) | takes FD4Time delta → calls reconcile every frame |
| reconcile | `0xa832a0` (`FUN_140a832a0`) | walks ALREADY-BUILT `CSWorldMapPointIns`, per-instance visibility/update vtbl calls (`+0x18`,`+0x28`→`+0x90`), add/remove/discover |
| `_DiscoverMapPoint` | `0xa84080` | reveal one point; reads row ptr at `[inst+0x80]` (id @ `+4`, fields `+0x30`/`+0x78`) |
| instance add / remove | `0xa84210` / `0xa850c0` | |
| **CSWorldMapPointIns ctor** | `0xa811e0` (`FUN_140a811e0`) | builds one instance from a `WorldMapPointParam` row |
| **build-loop call site** | `0xa82d09` | `new`s the ctor inside an **un-analyzed code gap** between `FUN_140a82a80` and `FUN_140a82eb0` (the "build all points from params" loop = the SHOW path) |
| PointMan singleton slot | `0x3d6e9d8` | getter AOB `48 8B 05 ?? ?? ?? ?? 48 85 C0 75 05 E8` |

## Deliverables (priority order)
1. **Instance container.** Disassemble `FUN_140a832a0` (`0xa832a0`): which field OFF THE MANAGER
   (`RCX`/param_1) holds the `CSWorldMapPointIns` collection, its **type** (vector / intrusive list /
   `std::_Tree`), and **how to iterate** it (offset + element stride + where the instance ptr sits).
   Give the offset(s) and node/element layout. (We dumped `mgr+0..0x300` from Linux but only while the
   container was empty/map-closed — you can dump it live with a working debugger.)
2. **`CSWorldMapPointIns` layout.** Confirm `[inst+0x80]` = source `WorldMapPointParam` row ptr.
   **Critically: does the instance CACHE `areaNo` (or a visibility/enabled bool) at build time?** If yes,
   give that field offset — editing it (not the param) + the per-frame reconcile may hide live with NO
   rebuild. If the reconcile re-reads `[inst+0x80]→param+0x20` every frame, say so (then our param edit
   alone should already live-hide — but it doesn't, so it almost certainly caches; pin the cached field).
3. **Build loop.** Name/RVA + **entry AOB** of the function containing call site `0xa82d09` (the loop that
   news `CSWorldMapPointIns` from `WorldMapPointParam`). Its **signature/ABI** (this=manager? an area /
   sub-page arg?), **preconditions** (main/render thread only? map-state `CSMenuMan+0xCD==7`?), and how
   map-open normally invokes it. This is the SHOW path (recreate instances from current params).
4. **The trigger — deliver ONE:**
   - **(a) callable rebuild fn:** AOB + signature + args + preconditions, so we call it after editing
     `areaNo` while the map is open; OR
   - **(b) live-hide-without-rebuild:** the instance cached-areaNo/visibility field offset from #2, proven
     to make the icon disappear next reconcile frame when flipped. (Hiding is the common case — section/
     category/cluster toggles, distance-adaptive — so even hide-only is a big win.)
5. **areaNo vs event-flag gate.** Confirm whether the build/reconcile gate visibility on
   `WORLD_MAP_POINT_PARAM_ST.areaNo` (`0x20`) or on **event flags**. If flags, name the field — we'd switch
   our hide from `areaNo`=99 to a flag field (`textDisableFlagId1` ↔ AlwaysOn `6001`).
6. **Programmatic-reopen FALLBACK.** If no clean rebuild is callable: the world-map menu open & close
   entry fns (via `CSMenuMan`, state `+0xCD==7`; menu setup is `FUN_1409be5e0`) — AOB + signature — so we
   force a close+reopen (1-frame flicker) to re-trigger the build loop.
7. **C++ snippet** in the mod's style: `modutils::scan<>` AOB-resolve (cached, SEH-guarded) implementing
   `goblin::refresh_world_map_icons()`, callable from `menu_auto_toggle_loop` right after
   `apply_section_visibility()` / the cluster apply, only when the map is open.

## Runtime test plan (you have a working debugger — we couldn't, Wine froze)
1. Arm a one-shot BP at `_DiscoverMapPoint` (`0xa84080`) or inside `FUN_140a832a0` where it dereferences an
   instance; capture a live `CSWorldMapPointIns*` (`RCX`). Read `[inst+0x80]`, dump `inst+0x00..0x100`.
2. **Hide test A (param):** with the map open, set that instance's source row `areaNo`(+0x20)=99 → does
   the icon vanish next frame? (We expect NO — instance caches.)
3. **Hide test B (instance):** flip the suspected cached areaNo/visibility field on the INSTANCE → icon
   vanish live? If yes → that's the live-hide lever (deliverable 4b).
4. **Show test:** call the build-loop container (#3) on the singleton while open after a param edit → do
   new/edited icons appear without reopen? Confirm thread/precondition safety (no crash).

## Conventions (so it drops into the DLL cleanly)
- Imagebase `0x140000000`. **Analyse the MSVC `.text` at `0x140001000`, NOT the VMP `.text` at
  `0x144c0e000`.** Resolve EVERYTHING by **AOB, never raw RVA** (RVAs drift per patch).
- Scan = `Pattern16::scan`; RIP-rel statics as `(AOB, {{first,second}})` where the resolver reads
  `*(int32*)(&match[first]) + second`. Function entries = bare AOB.
- Exe: `…/steamapps/common/ELDEN RING/Game/eldenring.exe` (~87 MB). User runs ERR via Mod Engine
  (vanilla exe) → AOBs match when the game version matches (app 2.6.2.0 / ERR 2.2.9.6).
- Hide code: `apply_section_visibility()` / cluster apply in `src/goblin_inject.cpp` flip `areaNo`
  99↔orig on the live param blob; the watcher (`menu_auto_toggle_loop`) is the single owner of mutation.
- Ghidra project persists `D:\ghidra_proj2\ER`; prior scripts `D:\ghidra_scripts\re_v31..v37.java`.

## Definition of done
A committed `goblin::refresh_world_map_icons()` (AOB-resolved, SEH-guarded) that, called after an
`areaNo` edit while the map is open, updates icons live with no reopen and no crash — verified by
flipping a section toggle or a cluster expand in-game. If only HIDE works live (not SHOW), ship that and
document the SHOW-needs-reopen limitation.
