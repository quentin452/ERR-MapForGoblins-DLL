# Windows RE findings — ER SAVE subsystem via LIVE RPM (sidecar Phase 2)

Sibling to `windows_save_function_rpm_re_prompt.md`. Solved the prompt's **Method B (RTTI vtable scan
via external RPM)** end-to-end against the running game — no Ghidra, no CE GUI. This cracks the
**indirect-dispatch wall** that stopped the Linux in-DLL stack walk
(`docs/re/linux_save_function_re_findings.md`): the outer save routine was virtual/worker-dispatched;
RTTI hands us the vtable directly.

Run env: Windows box, live `eldenring.exe` (App 2.6.x, base ASLR'd — **RVAs below are off imagebase
0x140000000 and are build-specific; the AOBs / RTTI recipe are the resilient primitives**). Tooling
(external `OpenProcess(VM_READ|QUERY)` + `ReadProcessMemory`, rebuilt from the GfxScan precedent) kept
in scratchpad — regenerate per boot (ASLR): `RttiScan.cs` (RTTI→COL→vtable), `CallScan.cs` (E8 call
BFS), `disah.py` / `xref.py` / `aobgen.py` (capstone disasm, GameDataMan xref, AOB gen).

## The answer — ER's save/load subsystem is `CS::SaveLoad2`

RTTI scan for `SaveLoad`/`SaveData`/`Serialize` type descriptors → CompleteObjectLocator → vtable
recovered the whole **`SaveLoad2`** namespace (positive controls `WorldMapDialog` + `GameDataMan`
resolved correctly, so the scanner is trustworthy). Key classes (vtable RVA → first meaningful method):

| Class (`…@SaveLoad2@@`) | vtable RVA | Role |
|---|---|---|
| `SLSystemImpl`      | `0x31FC080` | the save/load **system singleton** (vf0 `0x240DFE0` = deleting dtor, objsize 0x98) |
| `SLSessionManager`  | `0x31FBBF8` | creates/dispatches sessions (`0x240BE50`) |
| `SLSession`         | `0x31FBEE8` | base session (`0x240D410`, `0x240D9D0`) |
| `SLSessionRunnable` | `0x31FC0D0` | runnable base (`0x240EAB0`, `0x240EA50`) |
| **`SLSaveSession`** | **`0x31FC3A8`** | **the SAVE operation** — vtable {dtor `0x240FCC0`, getType→2 `0x240FD60`, **run `0x240FD70`**, getName `0x240FD00`} |
| `SLLoadSession`     | `0x31FC470` | the LOAD operation (`0x2410C20…`) |
| `SLDeleteSession`   | `0x31FC510` | delete (`0x24113A0…`) |
| `SLSaveContent`     | `0x31FB6E0` | serialized-content holder (vf0 `0x240A4E0` = deleting dtor, objsize 0x298) |
| `SLLoadContent`     | `0x31FB740` | (`0x240A920`) |
| `SLContentFormat`   | `0x31FB608` | (`0x2409070`) |
| `SLSaveConfirmOperation` / `SLOverwriteConfirmOperation` | `0x31FC828` / `0x31FC840` | UI-side confirm ops (own the `0x2412F60` "write helper" the Linux walk saw) |

Plus the full `SaveLoad2` UI operation zoo (`SL*ConfirmOperation` / `SL*ErrorOperation` /
`SL*NotifyOperation`) — the save-menu flow, not the data path.

## The outer save-WRITE routine — `SLSaveSession::run` @ RVA `0x240FD70`

`SLSaveSession`'s vtable slot 2 is the save state-machine. This **is** the "outermost game save fn"
the Linux stack walk was chasing — and it corrects that finding's mislabel: the CC-scan heuristic
guessed `0x253e4b0` (`SAVE_FN` in `re_signatures.hpp`), but an observer on it got **0 calls** because
it is reached by **vtable dispatch through the `SLSaveSession` vtable**, not by an `E8 rel32` call —
exactly why the rel32 stack walk couldn't reach it.

- **Call convention:** member fn, Win64 `__thiscall` — **`rcx = this` (SLSaveSession*)**, no other
  args consumed; stack-canary framed (`mov rax,rsp; push rbp/rdi/r12/r14/r15; sub rsp,0xB0`).
- **What it does (disasm confirmed):** walks a step state-machine, writing each save section from the
  session's content object at `[this+0xE0]` — `sub_0x2412CE0/0x2412E30/0x2412BE0/0x2414D30/0x2413230/
  0x2413860` emit sections, each followed by `sub_0x240DBF0` (record step result) + `sub_0x240DA40`
  (abort check). Reaches the known BND4 write tree root **`0x240DAA0`** (recursive with `0x240C530`).
- **CallScan reachability:** only the `SLSaveSession` vf's directly call `0x240DAA0`; every other
  candidate reaches only the shared node `0x240C530` (non-discriminating).

**AOB (unique in .text, from this build):**
```
48 8B C4 55 57 41 54 41 56 41 57 48 8D 68 A1 48 81 EC B0 00 00 00 48 C7 45 EF FE FF FF FF 48 89 58 10
```
Primary resolution should be **RTTI** (patch/mod robust, per the B3 precedent): find TD
`.?AVSLSaveSession@SaveLoad2@@` → COL → vtable → **slot 2** = run. The AOB is the fallback.

## CRUCIAL: this is the WRITE side, NOT the strip/reinject bracket

`SaveLoad2` **never reads `GameDataMan`** — a full GameDataMan static-slot xref scan (727 refs, slot
RVA `0x3D5DF38` via the known accessor AOB) has **zero hits in the whole `0x240xxxx` SaveLoad2 range**.
So `SLSaveSession::run` operates on an **already-serialized content buffer** (`[this+0xE0]`), populated
in an **earlier, separate phase**. This matches the Linux conclusion ("serialize ran before the write
stack; the write tree walks an already-serialized buffer") and the CreateFileW disproof (item already
serialized when the file opens).

Therefore hooking `SLSaveSession::run` is **too late to strip** — the inventory is already in the
buffer. It is still valuable as a **save-detection observer** and the write-bracket, but the
strip/reinject bracket must wrap the **game-data serialize** (the snapshot that reads `EquipGameData`).

## The remaining target — the game-data serialize (bracket point)

The serialize is where `GameDataMan+0x8 (PlayerGameData) +0x2B0 (EquipGameData)` is read into the save
content buffer. It is NOT in SaveLoad2. The dense GameDataMan-xref clusters (candidate serialize/copy
routines, entries found by CC-boundary scan) are `0xC96726`, `0xA4BD06`, `0x782955`, `0x5E929B` — but
static disambiguation from UI/status readers is unreliable, so this must be pinned **live**:

**Decisive method — find-what-accesses `EquipGameData` during a real save** (prompt Method A/C):
resolve `GameDataMan` (accessor AOB `48 8B 05 ?? ?? ?? ?? 48 85 C0 74 05 48 8B 40 58 C3 C3`,
slot = match+7+disp32) → `+0x8` PlayerGameData → `+0x2B0` EquipGameData → an item entry's bytes; HW
breakpoint (read) on those bytes (CE "Find out what accesses" or the in-DLL `goblin_field_probe`
observer); trigger a save; the accessor's containing function = the strip/reinject bracket.

> **UPDATE — this step was executed on Linux (b8755b3) and hit a WALL (5af1ee4, b0d2660). See
> `linux_save_function_re_findings.md` §"Update 2026-07-03 (PM #3)".** The serialize was located:
> **`SERIALIZE_FN = er+0x1ede700`** = the recursive object-serializer entry (6 E8 callers, fires ~150×
> per op on the save worker, tid 352). BUT it is a **generic REFLECTION walker shared by BOTH save AND
> load** (a `serclear` before a save still yields the boot/load chain) → a hook needs save-vs-load
> discrimination. The deepest common frame = the session-run **orchestrator `er+0x24fb88b`**, but its
> function entry **can't be pinned by offline CC/capstone heuristics** (ER `.text` has CC bytes
> mid-function → nearest-CC entry-find gives a false start with 0 E8 callers). **Cleanly bracketing
> the serialize (Variant A) needs real Ghidra**, not offline scripting. **Standing decision: ship
> Variant B** (reserved-id — the custom item lives in the `.err` under a high goods id, blank/unknown
> if loaded DLL-less; tolerable for the mod-locked `.err`) via the shipped `give_item` + sidecar store,
> no serialize hook. Revisit Variant A only with Ghidra.

## Deliverable status vs. the prompt

1. **SAVE routine RVA + AOB** — ✅ `SLSaveSession::run` `0x240FD70` + unique AOB + RTTI recipe;
   `re_signatures.hpp` `SAVE_FN` updated (replaced the proven-wrong `0x253e4b0`). ✅ **AOB re-verified
   unique on the ERR-Steam exe (3d3bcd2)** — the earlier ⚠ "re-verify on ERR build" is resolved.
2. **Call convention** — ✅ `rcx=this`, vtable-slot-2 dispatch, stack-canary framed.
3. **Pre-serialize ordering / strip bracket** — ⛔ the true bracket = the game-data serialize
   (`SERIALIZE_FN 0x1ede700`), located but NOT offline-hookable (shared save+load reflection walker +
   unpinnable orchestrator entry — see the UPDATE above). Needs Ghidra for Variant A; Variant B ships.
4. **Once-per-save / thread** — serialize runs on the save worker (tid 352); `SLSaveSession::run` is a
   ticked write state-machine (multiple invocations), not a once-per-save bracket.

Net: the indirect-dispatch wall is **broken** (SaveLoad2 mapped, `SLSaveSession::run` identified +
AOB-verified on ERR, corrects the Linux `0x253e4b0` mislabel; serialize `0x1ede700` located). The
remaining Variant-A atomic-strip bracket is **Ghidra-gated**; **Variant B is the pragmatic ship.**
