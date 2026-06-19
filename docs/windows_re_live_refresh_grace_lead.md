# Windows RE — lift the LIVE world-map icon rebuild (via the "Show All Graces" lead)

**For:** an agent on **Windows** with **Ghidra/IDA** + a debugger that can attach to the
running game (Cheat Engine and/or x64dbg). Repo: **ERR-MapForGoblins-DLL** (Elden Ring
world-map icon mod, DLL injected via Mod Engine / ERSC). Work lands on **master**.

## The one ask
Find the game's **live world-map icon rebuild** path and give us a way to trigger it from
injected code (or a state byte that forces it). This unblocks three stuck features at once:
live section/category toggle, **runtime clustering** (no restart), and **proximity clusters
v2** (re-cluster around the player as they move).

We have a **confirmed, concrete lead** — this is no longer a blind hunt.

## The lead (CONFIRMED live in-game)
The **Hexinton all-in-one CT v6.0** toggle **Progression → "Show All Graces"** rebuilds the
world-map icons **LIVE while the map is open** (user verified: graces pop in with no reopen).
So the engine HAS a live re-evaluate path; the CT just flips one byte that sits inside it.

The CT script:
```
aobscanmodule(Finder, eldenring.exe, 0F B6 0D xx xx xx xx E8 xx xx xx xx 44 8B E0)
addr = Finder + readInteger(Finder+3) + 7      ; RIP-rel disp32 of the movzx -> a global BYTE
registerSymbol("NewMenuSystemWarp2", addr)
NewMenuSystemWarp2     = 01     ; flip 0 -> 1
NewMenuSystemWarp2 + 1 = 01     ; (and the next byte)
```
Disassembly at the AOB site:
```
0F B6 0D <disp32>   movzx ecx, byte ptr [rip + NewMenuSystemWarp2]   ; read the toggle byte
E8 <rel32>          call   <FUN_X>                                   ; <-- prime suspect: the rebuild
44 8B E0            mov    r12d, eax
```
So `NewMenuSystemWarp2` is consumed **inside the live map-icon evaluation path**, and the
adjacent `call` is the best candidate for the actual (re)build/re-filter routine.

## Deliverables (in priority order)
1. **Locate & name the site.** Resolve the AOB above in eldenring.exe, give the
   **containing function** (RVA + AOB-for-entry) and the **`E8` call target** (RVA). Decode
   what `NewMenuSystemWarp2` is (the global byte's RVA + how the function branches on it).
2. **Relationship to FUN_140a832a0.** An earlier static pass flagged `FUN_140a832a0`
   (RVA 0xa832a0) as the CSWorldMapPointMan point-list rebuild (calls `_DiscoverMapPoint`
   0xa84080; singleton slot 0x143d6e9d8). Is the `E8` target == 0xa832a0, or does the
   containing function call it? Map how the grace toggle reaches the point-list rebuild.
3. **Scope check (CRITICAL).** Does this path re-evaluate **ALL `WorldMapPointParam` rows**
   (then flipping our cluster/section `areaNo`=99 + re-running it shows live), or is it
   **grace-only**? If grace-only, find the general map-point rebuild it shares with
   CSWorldMapPointMan.
4. **The trigger — give ONE of:**
   - **(a) callable fn:** AOB + signature + args + preconditions (main/render thread only?
     map-state==7? sub-page arg?) for the rebuild, so we call it after editing `areaNo`; OR
   - **(b) state byte:** if simply writing a byte (like `NewMenuSystemWarp2`) forces a live
     rebuild each frame, give its AOB-resolved address + how the engine consumes it — we'd
     just toggle it after our edits (simplest, no fn-call ABI risk).
5. **areaNo caveat.** Confirm the live path reads `WORLD_MAP_POINT_PARAM_ST.areaNo` (offset
   0x20). If it gates on **event flags** instead, say so — we'd switch our hide from
   `areaNo`=99 to a flag field (`textDisableFlagId1` ↔ AlwaysOn 6001) before triggering.
6. **C++ snippet** in the mod's style: `modutils::scan<>` AOB-resolve (cached, SEH-guarded)
   for `goblin::refresh_world_map_icons()`, callable from `menu_auto_toggle_loop` right after
   `apply_section_visibility()` / the cluster apply when the map is open.
7. **Runtime evidence:** a note/screenshot that triggering it live refreshes icons with no
   reopen and no crash (test by flipping a section toggle, or our cluster expand).

## Conventions (match these so it drops into the DLL cleanly)
- Imagebase 0x140000000. **Analyse the MSVC `.text` at VA 0x140001000**, NOT the VMP `.text`
  at 0x144c0e000. **Resolve everything by AOB, never raw RVA** (RVAs drift per patch).
- Scan = `Pattern16::scan`; RIP-rel statics delivered as `(AOB, {{first,second}})` where the
  resolver reads `*(int32*)(&match[first]) + second`. Function entries = bare AOB.
- Exe: `…/steamapps/common/ELDEN RING/Game/eldenring.exe` (~87 MB). The user runs **ERR via
  Mod Engine** (vanilla exe) so AOBs match if the game version matches.

## Context references in the repo
- `docs/windows_live_refresh_re_prompt.md` — the original live-refresh ask (this lead is also
  appended there).
- `docs/world_map_live_refresh_re.md` — prior static findings (CSWorldMapPointMan map, the
  FUN_140a832a0 candidate, _Verify{Enable,Disable}EventFlag gates).
- `docs/re_findings_playerpos.md` — player pos SOLVED (`manager+0x70/+0x78`, marker space) —
  the other half of proximity v2; this live-refresh is the remaining blocker.
- Hide mechanism: `apply_section_visibility()` / cluster apply in `src/goblin_inject.cpp`
  flip `areaNo` 99↔orig on the live param blob.

---

# RESULTS (2026-06-19, static, Ghidra headless `re_v31`–`re_v37`, app 2.6.2.0)

**TL;DR — the lead resolves cleanly but is the WRONG subsystem.** The `NewMenuSystemWarp2`
AOB is the **map-FRAGMENT / overlay reveal** path (`WorldMapPieceParam` +
`CS::WorldMapDialogBase` map-number masks). It has **zero** connection to the icon/point
path (`WorldMapPointParam` → `CSWorldMapPointIns`). Writing that byte reveals the whole map
**terrain** live; it does **nothing** for our grace/item/cluster icons. The CT label "Show
All Graces" is a misnomer for "reveal entire map".

## Deliverable 1 — the site (RESOLVED, unique AOB hit in MSVC `.text`)
| thing | value |
|---|---|
| AOB site | VA `0x140889111` (RVA `0x889111`) |
| toggle byte `NewMenuSystemWarp2` | VA `0x143d6cfc0` (RVA `0x3d6cfc0`) — disp32 @ site+3, `byteVA = site+7+disp32` |
| `E8` call target | `FUN_1408882d0` (RVA `0x8882d0`) |
| containing fn | `FUN_1408890b0` (RVA `0x8890b0`); its caller = `FUN_140887870` (`0x887870`) |

`FUN_1408882d0(toggleByte, areaIdx)`: walks `CS::WorldMapPieceParam` rows
(`FUN_140d56d90` lookup, row IDs `areaIdx*100 + i`, `i` in `0..0x1f`) and builds a 32-bit
"revealed pieces" mask. **When the toggle byte ≠ 0 it returns `0xffffffff`** (= every piece
revealed); else it computes the legit set from flags/`_GetMapVariation`.
`FUN_1408890b0` diffs old-vs-new mask stored at `[manager+0x39c + idx*4]` and incrementally
creates the newly-revealed piece display objects (`CS::WorldMapPieceParam::vftable`). This is
a **per-frame diff-reconcile** — that's why flipping the byte refreshes live.

All 7 consumers of the byte are piece/overlay, none touch points:
`FUN_1409c6f70` / `FUN_1409c2470` read it inside
`CS::WorldMapDialogBase::_IsChangeableOverlayLayer` and `GetEnableMapNoMask`
(force overlay/map-number layers "enabled"); plus `FUN_1408855b0`, `FUN_140886910`,
`FUN_1409be5e0` (world-map menu setup).

## Deliverable 2 — relationship to `FUN_140a832a0` (NONE)
`E8` target `0x8882d0` ≠ `0xa832a0`. Automated bridge check over all 7 consumers: **zero**
refs to the point subsystem (`FUN_140a832a0` `0xa832a0`, `_DiscoverMapPoint` `0xa84080`,
point singleton `0x143d6e9d8`). The two subsystems are fully disjoint.

## Deliverable 3 — scope (PIECE-only, CRITICAL)
This path re-evaluates `WorldMapPieceParam` (map fragments) + overlay/map-number masks, **not**
`WorldMapPointParam` rows. Flipping our cluster/section `areaNo`=99 is invisible to it. So the
grace lead does **not** unblock the icon features.

## Deliverable 4/5 — trigger & `areaNo` caveat
- The byte is a valid live trigger **for terrain/pieces only**. Not usable for icons.
- This path never reads `WorldMapPointParam.areaNo` (offset 0x20). It reads `WorldMapPieceParam`
  IDs + map-variation/overlay masks. So the `areaNo`-vs-event-flag question is moot here.

## Deliverables 6/7 — N/A
No icon-refresh `goblin::refresh_world_map_icons()` falls out of this byte; no runtime test
warranted (it would only confirm terrain reveal).

---

# The REAL path forward (point subsystem map — the useful output)
The engine **does** have a per-frame point pipeline; it's just separate from the grace byte:

- **Per-frame driver:** `FUN_140623410` (RVA `0x623410`, takes FD4Time delta) → calls
  **`FUN_140a832a0`** (`0xa832a0`) every frame.
- **`FUN_140a832a0` = per-frame RECONCILE**, not a from-scratch rebuild. It walks the RB-tree
  of **already-built** `CSWorldMapPointIns` instances (tree root `[singleton+8]`, red/black
  byte at `+0x19`), reads player pos (`[DAT_143d65f88 + 0x1e508]`), calls each instance's
  virtual visibility/update methods (`vtable+0x18`, `+0x28` → deeper `+0x90`), and
  adds/removes/discovers via `_DiscoverMapPoint` (`0xa84080`), add `FUN_140a84210`, remove
  `FUN_140a850c0`. It does **not** re-read the param table to *create* instances.
- **`_DiscoverMapPoint` (`0xa84080`):** reads the row ptr at `[inst+0x80]` (id @ `+4`,
  fields @ `+0x30`/`+0x78`), checks discovery via `FUN_140d09bf0`-style flag query, marks revealed.
- **Build-from-params (constructs `CSWorldMapPointIns`):** constructor `FUN_140a811e0`
  (`0xa811e0`) is `new`-ed at call site **`0xa82d09`**, which lives in an **un-analyzed code gap**
  between `FUN_140a82a80` and `FUN_140a82eb0` (Ghidra never created the function). This is the
  "build all points from `WorldMapPointParam`" loop = the analog of the piece path's
  `FUN_1408890b0`. **Pin its exact entry/signature at runtime** (breakpoint constructor
  `0xa811e0`, read the return address / call stack — quentin's runtime step).
- Point singleton slot = `0x143d6e9d8` (per prior notes, AOB
  `48 8B 05 ?? ?? ?? ?? 48 85 C0 75 05 E8`).

### Recommended runtime options (in order)
1. **Re-drive the point build loop** (the `0xa82d09` container) on the singleton after our
   `areaNo` flip while map open (`CSMenuMan +0xCD == 7`). Cleanest if its ABI is simple
   (likely `(manager_this, area_arg)`). Pin via the constructor breakpoint above.
2. **Test the cheap case first:** for *hiding*, flipping `areaNo`→99 on an already-built
   instance may already take effect through the per-frame `FUN_140a832a0` visibility predicate
   — verify in CE before building anything. *Showing* (creating a new instance) will still
   need the build loop.
3. **Fallback — programmatic map reopen** via CSMenuMan close+open (1-frame flicker); already
   scoped in `docs/world_map_live_refresh_re.md`.

Scripts: `D:\ghidra_scripts\re_v31.java`..`re_v37.java`; full dumps `out_v31..v37.txt`.
