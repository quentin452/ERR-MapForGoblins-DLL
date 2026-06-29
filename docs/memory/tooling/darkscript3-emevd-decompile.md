---
name: darkscript3-emevd-decompile
description: DarkScript3 install + hidden CLI batch-decompile of ER EMEVD to grep-able JS (for mining NPC death flags)
metadata: 
  node_type: memory
  type: project
---

For the EMEVD death-flag RE task (see [[mapforgoblins-pipeline-setup]], docs/windows_re_emevd_death_flags.md).

**DarkScript3** (vawser fork, build 2024-11-02 — the `thefifthmatt` repo has no releases) installed at
`D:\tools\DarkScript3\DarkScript3.exe`. Self-contained WinForms; bundles ER defs (`Resources\er-common.*`).

**Hidden CLI batch mode** (no GUI; from `Program.Main`): decompiles a whole folder of `.emevd.dcx` at once.
```
DarkScript3.exe /cmd -decompile -game er -indir <event_dir> -outdir <out_dir>
```
- valid -game: ds1/ds1r/ds3/sekiro/er/ac6. Output = `<name>.emevd.dcx.js` (grep-able).
- **REQUIRES `oo2core_6_win64.dll` in the cwd** (ER .dcx is KRAK-compressed) — copied next to the exe.
  Run with `-WorkingDirectory 'D:\tools\DarkScript3'`. Without it: Oodle exit code -532462766.
- The copy + run must use **dangerouslyDisableSandbox** or writes/child-process land in the sandbox
  overlay and vanish (see [[windows-tooling-gotchas]]).
- PowerShell gotcha: never name the arg array `$args` (reserved automatic var); use `$dsargs`.

**Decompiled output:** `D:\tools\emevd_js\err\` — 516/517 ERR files (6.7 MB).
One file fails: `m12_02_00_00` (Weeping Peninsula, no priority NPCs) — DarkScript3 "Fancy" decompiler
CFG bug "trying to place #165 L4: twice in structuring 12022872". Workaround: open in GUI w/ Fancy off.

**Bootstrap validation vs the 3 ground-truth flags:**
- **Seluvis 1034509302** ✓ reproduced. Set by `common.emevd` `$Event(3049)` (Ranni-cluster resolution):
  `WaitFor(flag||flag2||flag3||flag4)` over quest-progression groups; `if(!flag3.Passed) SetEventFlagID(1034509302,ON)`
  where flag3 = `AnyBatchEventFlags(3565,3568) && EventFlag(3561)`. So death/conclusion flags here are set by a
  group-resolution event, NOT a direct `IfCharacterDead` — richer/subtler than the briefing assumed.
- **Varre 1042369205** ~ only OFF-resets appear as literals (region init in m12_05/m60_42_36). ON-set is via
  a parameterized/other path — not yet traced.
- **Iji = `1034499202`** (FINAL, MSB-confirmed). Two earlier wrong answers: `1042600001` (a 19-bit counter
  bit) and `1034509403` (that's RANNI's resolver flag — the agent mis-mapped entity `1034500710`, which the
  MSB shows is Ranni/c2050). Iji's real entities are `1034490700`/`1034490711` (model c4604, m60_34_49);
  his flag (group 3760-3767, ns 1034499xxx) is set ON at `common.emevd:38156`, OFF-reset `m60_34_49:117` —
  Seluvis-parallel. **Lesson: always MSB-confirm entity→name before trusting an agent's flag attribution.**

**The mechanism (lets you enumerate ALL NPC death flags deterministically):** death is wired via common-event
templates. `InitializeCommonEvent(0, **90005702**, ENTITY, DEATHFLAG, batchLo, batchHi)` — arg#4 = the
persistent save-backed on-death flag (body `common_func:5202`: `WaitFor(!EventFlag(F)&&CharacterDead(E)); ...
SetNetworkconnectedEventFlagID(F,ON); SaveRequest()`). Also `90005860` & `90005300/01` (boss/generic, flag==entity).
Grep `InitializeCommonEvent\(0, 9000570[27]` over the JS → every `(entity,flag)` pair. Caveat: for merchant NPCs
the 90005702 flag == their `_q99` "concluded" flag (set on death OR peaceful conclusion), not death-exclusive.

**Entity→name bridge tools (reusable):** `tools/_resolve_entities.py` (entity IDs → name) and
`tools/_find_npc.py` (name substring → entities) — MSB Enemy `NPCParamID` → `nameId` → NpcName FMG
(in **item.msgbnd**, not menu). Run `MFG_PROFILE=vanilla py -3.14 ...` from `tools/`. The MSB is the
authority — it caught the agent's Iji=Ranni mix-up. **NpcParam has NO per-NPC death-flag field** (checked).

**TalkESD pipeline (DLC followers) — use `esdtool.exe`** (thefifthmatt/ESDLang v0.5.1, ESD analogue of
DarkScript3) at `D:\tools\esdtool\esdtool-v0.5.1\`. Decompile: `esdtool.exe -er -basedir "<game>" -writepy
"<out>\%m\%e.py" -nobackup` — RUN FROM the esdtool dir (needs `dist\`), oo2core in cwd, quote the
ELDEN-RING-with-space path. Output: `D:\tools\esd_py\<map>\t<TalkID>.py` (378 ESDs). Map NPC→TalkID with
`tools/_npc_talkids.py` (MSB Enemy `TalkID` field). **Pattern (bootstrapped from Patches flag1=3683=dead):**
the talk-template call `t..._x5(flag1=<dead>, flag2=<alive>, …)` → flag1 = the dead/gone flag. DLC followers
land in EMEVD networkconnected blocks `44X0-44X3` (dead=44X3). **Cracked & wired (6):** Ansbach 4403,
Freyja 4423, Hornsent 4363, Leda 4443, Thiollier 4463, Dane 4563 (Ansbach/Freyja also have 90005702 handlers
at m21_01 the EMEVD-only pass missed). Tail: Moore/Igon/Queelign/Jolán (blocks exist, need entity mapping).
`tools/mine_talkesd_flags.py` (raw int-extractor) is superseded by esdtool but kept. **23 fail_flags wired**
(16 death-distinct + 7 `fail_conclusion`).

**Full results table → `docs/emevd_death_flags_results.md`** (wire-list, do-not-wire classification, not-found set).
Notable: Varré `1042369205` is ESD-set (EMEVD only OFF-resets it) so the save-diff anchor is correct — keep it.
Unresolved (need MSB entity bridge): Jerren, Gostoc, + DLC questline NPCs (Ansbach/Thiollier/Leda/etc = TalkESD-driven).
