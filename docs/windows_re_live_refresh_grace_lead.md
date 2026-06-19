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
