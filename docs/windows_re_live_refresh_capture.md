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

---

# RESULTS (2026-06-19, static, Ghidra headless `re_v38`–`re_v44`, app 2.6.2.0)

**TL;DR — the static map is fully solved; the trigger is shipped as a hook-capture-replay gated
behind a new OFF-by-default config flag, for the user to runtime-validate.** The build function was
recovered (Ghidra had truncated it; a mis-flagged `noreturn` allocator fragmented the body), the
instance container + layout + the per-frame reconcile are decoded, and a unique entry AOB for the
build fn is verified. The remaining unknowns (does the build run while the map is open? does its extra
pass remove now-hidden rows?) are runtime-only and the implementation is **safe + no-regression**
either way.

## Deliverable 1 — Instance container (SOLVED)
- **Manager singleton instance** lives at static `0x143d6e9b0` (the `CSWorldMapPointManImplement`
  instance ptr; the FD4Singleton getter slot from prior notes, `0x3d6e9d8`, resolves the same object).
  Build driver `FUN_14063d400` calls the refresh as `FUN_140a82a80(*(0x143d6e9b0), ctx)`.
- **Built-icon collection = `[manager + 0x398]`** — an MSVC `std::map<int id, CSWorldMapPointIns*>`
  (red-black `_Tree`). Sentinel/head node at `[mgr+0x398]`; root = `[head+8]` (parent),
  begin = `[head+0]` (leftmost). Per node: `[0]` left, `[8]` parent, `[0x10]` right,
  **`+0x19` = color/isnil byte** (0 = real node), **`+0x20` = key** (int point id),
  **`+0x28` = value** (`CSWorldMapPointIns*`). Both the reconcile and the build iterate it with the
  standard left/parent/right `_Tree` walk.
- A **second tree at `[manager + 0x8]`** holds the discovery/reentry-point instances — the ONLY set
  the per-frame reconcile adds/removes (gated by vmethods `+0x18`/`+0x28`). The earlier Linux note
  "`[singleton+8]` reads 0 → not the tree" was a false negative: that set was simply empty at the
  time. The placed-icon set is `+0x398`, not `+0x8`.
- Other manager fields seen: `+0x88/+0x90` and `+0x120/+0x128` = two intrusive lists (pending/free),
  `+0x158` a sub-object, `+0x11c`/`+0x148`/`+0x14c` counters.

## Deliverable 2 — `CSWorldMapPointIns` layout + cached visibility (SOLVED)
- Size **`0x110`** bytes (`FUN_141eb9ed0(0x110, 0x10)` at the build site). vtable
  `CS::CSWorldMapPointIns::vftable` (`0x142b487a8`).
- `[inst+0x18]` = embedded `CSWorldMapPoint` wrapper; **`[inst+0x80]` = source `WorldMapPointParam`
  row ptr** (= `inst+0x18 + 0x68`), confirmed by `_DiscoverMapPoint` reading `[inst+0x80]` (id @ +4,
  fields @ +0x30/+0x78). Position floats written at `inst+0x70/+0x90/+0xd0`; two bool fields
  zero-init in the ctor at `inst+0xC0` and `inst+0x100`.
- vtable methods (decompiled): **`+0x8`** = show-predicate `FUN_140a81450` (bool "display this point?"
  — reads `FUN_140cfbcb0(inst+0x18)`, then sets the position); **`+0x18`** = area/page getter
  `FUN_140a81440` (`return FUN_140d0f060(inst+0x18)`); **`+0x28`** = `FUN_140a811d0`
  (`[[inst+8]+0x90]()`).
- **CRITICAL (answers #2 / #4b):** the per-frame reconcile `FUN_140a832a0` does **NOT** re-evaluate
  the show-predicate on the `+0x398` set — its `+0x398` loop only runs `thunk_FUN_1457cd3df(inst, ctx)`
  (a position/animation tick) per node. The show-predicate (`vtable+0x8`) is evaluated **only at build
  time** (in `FUN_140a82a80`, right after each `new`). ⇒ **There is no per-frame instance field to flip
  for a live hide — deliverable #4(b) is not achievable.** Both show and hide require re-running the
  build.

## Deliverable 5 — areaNo vs event-flag gate (SOLVED: areaNo)
The build's per-row visibility derives from the `CSWorldMapPoint` wrapper (`FUN_140cfbcb0`/`FUN_140d0f060`
on `inst+0x18`), which is built from the `WorldMapPointParam` row at `[inst+0x80]`. Our hide flips that
row's **`areaNo` (0x20)** to 99; the rebuild re-reads the row, so the edit is honored on rebuild
(consistent with "shows on reopen today"). The reconcile's *discovery/reentry* path also consults event
flags via `FUN_140d09bf0(DAT_143d7d478, 0x21/0x26/0x27, 1)` (area-availability), but for the placed
icons `areaNo` is the effective lever the build honors. **No switch to a flag field needed.**

## Deliverable 3 — Build loop (SOLVED)
- The "build all points from params" function is **`FUN_140a82a80` (RVA `0xa82a80`)** — earlier seen as
  a 160-byte stub because Ghidra truncated it: the allocator `FUN_141eb9ed0` was mis-flagged
  `noreturn`, fragmenting the body. It is one function spanning `0xa82a80..~0xa82eb0`; the build call
  site **`0xa82d09`** (`new 0x110` + ctor `FUN_140a811e0`) sits inside it.
- **Signature/ABI:** `void __fastcall FUN_140a82a80(void *pointman /*rcx, this*/, void *ctx /*rdx*/)`.
  `ctx` is a **transient per-frame context**: `[ctx+0x34]` = int page/area filter, `[ctx+0x48]` =
  `ParamRepo*` for `FUN_140cf6300(repo, categoryId)` count lookups (categories `0x26/0x27/0x21`).
- **Behavior:** per category it gets the row count, constructs missing instances (running the
  show-predicate, destroying rows that fail), inserts/updates into `+0x398` via lower_bound +
  `FUN_140a80430`, and has a removal pass over `+0x398`. It is an **incremental reconcile** from current
  params, not a from-scratch wipe.
- **Invocation:** called every world-step from `FUN_14063d400` (the "step all map subsystems" driver,
  ~15 guarded singleton updates) as `FUN_140a82a80(*(0x143d6e9b0), *(driverArg+8))`, guarded by
  `driverArg+0x40 != 0`. `ctx` is owned by that driver's frame — **not a singleton** — which is why a
  blind direct call (with a fabricated ctx) is unsafe.

## Deliverable 4/6 — Trigger (analysis + shipped mechanism)
- **(4a) callable rebuild:** `FUN_140a82a80` IS the engine's rebuild, but its non-resolvable per-frame
  `ctx` makes a blind direct call unsafe.
- **(4b) live-hide-without-rebuild:** NOT possible — no per-frame predicate on `+0x398` (see #2).
- **(6) programmatic reopen:** opening the map = the menu framework constructs `CS::WorldMapDialog`
  (ctor `FUN_1409cef10` → setup `FUN_1409be5e0`) via a factory (`@0x9cfa05`) and pushes it on the menu
  stack — there is no simple `reopen()`, so a blind reopen is also unsafe to ship.
- **SHIPPED — hook-capture-replay:** hook `FUN_140a82a80`; the detour passively records the engine's
  own `(this, ctx)` every call, and when `refresh_world_map_icons()` has set a request (after an
  `areaNo` edit, map open) it **re-invokes the original once more with that captured real pair, inside
  the same natural call (so `ctx` is guaranteed live), on the engine's own thread.** No fabrication, no
  menu surgery. **Entry AOB (verified UNIQUE):**
  `40 55 53 56 57 41 54 41 56 41 57 48 8B EC 48 83 EC 60 48 C7 45 D0 FE FF FF FF 4C 8B F9 8B 42 34`.

## Deliverable 7 — C++ (committed, OFF by default)
`src/goblin_inject.cpp`: `install_live_refresh_hook()` (resolve+queue the hook; called from
`setup_mod` before `enable_hooks`), `wm_build_detour` (POD, SEH-guarded extra pass), and
`goblin::refresh_world_map_icons()` (sets the request when the map is open). Wired into
`menu_auto_toggle_loop` after the section/category `areaNo` applies. Gated by the new config flag
**`live_refresh_world_map` (default false)** — default builds are byte-for-byte unaffected (the hook
isn't even installed unless the flag is set).

## What remains for the user (runtime, you have the debugger)
The implementation is **safe and no-regression either way**; runtime testing decides how *prompt* the
refresh is:
1. **Does `FUN_140a82a80` run while the 2D map is open?** Bp `0xa82a80`, open the map, move around. The
   prior CONFIRMED-LIVE note ("reconcile fires every frame; ctor did NOT fire on reopen") implies the
   build driver *does* run continuously but finds all instances cached (no new ctor) — if so the replay
   fires within a frame of your toggle. **If the world-step driver pauses while the map dialog is up,**
   the replay won't fire until the next world step (≈ when you close the map) — i.e. it degrades to
   today's "applies on reopen" behavior, no worse. In that case the fallback is to hook the *reconcile*
   `FUN_140a832a0` (fires every frame with the map open) and replay the build from there using a ctx
   captured while the build last ran — only viable if that ctx is heap-persistent (verify), else the
   menu-reopen path.
2. **Does the extra build pass actually HIDE now-`areaNo`-99 rows** (its `+0x398` removal loop at
   `0xa82df0`), not just SHOW newly-passing ones? Toggle a section OFF with the map open and watch.
3. **No crash** across repeated toggles. Enable with `live_refresh_world_map=true` in the ini.

Scripts: `D:\ghidra_scripts\re_v38.java`–`re_v44.java`; dumps `out_v38.txt`–`out_v44.txt`.
