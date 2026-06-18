# EMEVD death-flag mining — results (ERR EMEVD, decompiled)

Built from the decompiled ERR EMEVD (`D:\tools\emevd_js\err\*.emevd.dcx.js`, DarkScript3 CLI),
cross-referenced against the EldenRingQuestLog `_q99` flags and DarkScript3's ER name tables.
Every flag below was found by grepping the decompiled EMEVD — `file:line` cited. Companion to
`docs/windows_re_emevd_death_flags.md` (the briefing). **The user runtime-tests before trusting.**

## The mechanism (the real prize)
NPC deaths are wired through a handful of **common-event death templates**. The registration call
encodes `(entity → death_flag)` directly, so the whole roster can be enumerated deterministically:

- **`90005702`** (the main NPC one) — `InitializeCommonEvent(0, 90005702, ENTITY, DEATHFLAG, batchLo, batchHi)`.
  Body (`common_func.emevd.dcx.js:5202`): `WaitFor(!EventFlag(DEATHFLAG) && CharacterDead(ENTITY)); BatchSetNetworkconnectedEventFlags(lo,hi,OFF); SetNetworkconnectedEventFlagID(DEATHFLAG, ON); SaveRequest();`
  → **arg #2 is the persistent, save-backed on-death flag.** Set ON only when the NPC actually dies.
- **`90005860`** — boss-ish template; `SetEventFlagID(arg, ON)` on death, **flag id == entity id** (+ item lot).
- **`90005300` / `90005301`** — generic enemy death template, flag id == entity id (used for minor/ambient chrs).
- **Resolution blocks** in `common.emevd` (e.g. `$Event(3049)`) map quest-progression flag-groups →
  the persistent `10345xxxxx`-namespace "concluded/gone" flags (Seluvis, Iji, Blaidd).

> ⚠️ **Shared-flag caveat:** for the merchant/quest NPCs the `90005702` death flag is the *same id*
> set on peaceful questline conclusion (it equals their `_q99` flag). So flag=ON means "NPC gone
> (dead **or** concluded)", not strictly "killed". For a grey-out-when-unfinishable UI that's usually
> fine, but it is not death-exclusive. The genuinely death-only ones are noted as such.

---

## WIRE these — `fail_flag` candidates (death-set flag found in EMEVD)

| NPC | death_flag | entity | evidence | q99 match | conf |
|---|---|---|---|---|---|
| **Iji** ⚠correction | **1034509403** | 1034500710 (m60_34_50) | `common.emevd:38146,38152` (event 3049, group 3740-3748); reset `m60_34_50:584` | parallels Seluvis | high |
| Seluvis ✓anchor | 1034509302 | 1034500701 | `common.emevd:38160` (event 3049, group 3565-3568) | =seluvis_q99 | high |
| Varré ✓anchor | 1042369205 *(keep)* | 1042360700 | ESD-set ON (EMEVD only OFF-resets it: `m12_05:694`, `m60_42_36:256`) | n/a (3183=corrob.) | high |
| Patches | 3683 | 31000701 (Murkwater) | `m31_00:37` `90005702(…,3683,…)` | =patches_q99 | high* |
| Boc | 3943 | 11050730 | `m11_05:57` `90005702(…,3943,…)` | =boc_q99 | high* |
| Kenneth Haight | 3583 | 1045380700 | `m60_45_38:4` `90005702(…,3583,…)` | =kenneth_q99 | high* |
| Sorcerer Thops | 3803 | 1039390700 | `m60_39_39:4` `90005702(…,3803,…)` | =thops_q99 | high* |
| Sorceress Sellen | 3463 | 14000713 (Academy) | `m14_00:181` `90005702(…,3463,…)` | =sellen_q99 | high* |
| Nepheli Loux | 4223 | 10000730 (Stormveil) | `m10_00:156` `90005702(…,4223,…)` | =nepheli_q99 | high* |
| Knight Diallos | 3443 | 1039440710 (Jarburg) | `m60_39_44:79` `90005702(…,3443,…)` | =diallos_q99 | high |
| Yura | 3623 | 1049530700 | `m60_49_53:13` `90005702(…,3623,…)` | =yura_q99 | high |
| Sage Gowry | 4163 | 1050380700 (Gowry's Shack) | `m60_50_38:6` `90005702(…,4163,…)` | (his ns 416x) | med-high |
| Knight Bernahl | 3883 | 16000800 boss → 3883 | `m16_00:575` `CharacterDead(16000800)`; resolves `common.emevd:40253` → `SetEventFlag(3883,ON)` | =bernahl_q99 | high |
| Kalé (merchant) | 4703 | 11000705 (Roundtable) | `m11_00:143` `90005702(…,4703,4700,4704)`; comment-documented | alive/hostile/dead 4700/01/03 | high |
| Gurranq / Beast Clergyman | 1051430800 | 1051430800 (Bestial Sanctum) | `m60_51_43:4` `90005860(…)`; body `common_func:7985` | flag==entity | med-high |

`*` = high confidence on the **flag**, but it's the shared "gone (dead-or-concluded)" flag (see caveat).

### One correction to apply (high confidence)
- **Iji: `1042600001` → `1034509403`.** The committed `1042600001` is a phantom: the literal appears in
  **none** of the 516 files, and `1042600000` is an `EventValue(1042600000, 19)` 19-bit **counter** base —
  so `1042600001` is a counter bit a save-diff mistook for a death flag. The real persistent flag is
  `1034509403`, set by the same `$Event(3049)` resolver that sets Seluvis's `1034509302` (exact structural
  parallel). Touches `src/generated/goblin_quest_steps.cpp` (Iji row). **Verify in-game before committing.**
- **Varré: keep `1042369205`.** It's a real flag in his namespace (EMEVD resets it OFF when he's restored
  alive; his ESD sets it ON on death — which is why your save-diff caught it). The EMEVD `90005702` path
  also sets `3183` (=varre_q99) on his death, but your verified anchor stands.

---

## DO NOT WIRE — death = quest COMPLETION / unkillable (classified, for reference)

| NPC | flag (if any) | why not | evidence |
|---|---|---|---|
| Millicent | 4183 | Haligtree resolution = quest's end, not failure | `m15_00:` `90005702(15000700,4183,…)`; =millicent_q99 |
| Blaidd | 3603 → 1045379209 | killing corrupted Blaidd *concludes* Ranni's quest | `m60_45_37_10:27` `90005702(1045370700,3603,…)` → `common.emevd:38164` |
| Dung Eater | 35000850 / 9222 | killable boss; killing him *is* the quest end | `m35_00:1570` `CharacterDead(35000850)` → EnemyFelled + 9222/61222 |
| Count Ymir (DLC) | 20010455 | boss of his own questline; kill = completion | `m20_01:620` `CharacterDead(20010455)` → GreatEnemyFelled |
| Ranni | — (immortal) | `EnableCharacterImmortality(1034500710)`; ends via Age of Stars | `m60_34_50:367`; no CharacterDead |
| Hyetta | 1043372718 | death-window flag decoupled from her Frenzied-Flame completion (3383) | `m60_43_38:102` |
| Fia / Rogier / D / Corhyn / Goldmask | none persistent in EMEVD | quests conclude via ESD/progression, no CharacterDead death-set | see agent notes |

---

## NOT FOUND in EMEVD — fall back to save-diff / ESD (no EMEVD death flag exists)

- **Roderika, Rya** — quest namespaces are only *read* in EMEVD, never set; death is NpcParam/ESD-driven.
- **Latenna** — she *warps away* (no `CharacterDead`); no death path exists in EMEVD at all.
- **Jerren** — unresolved (the `15000700→4183` hit was Millicent, not Jerren; Redmane Castle is an m60 tile,
  needs an MSB lookup for his entity).
- **Gostoc** — unresolved (the `10000730` hit was Nepheli).
- **DLC questline NPCs** (Ansbach, Moore, Thiollier, Dryleaf Dane, Freyja, Needle Knight Leda, Queelign,
  Igon, Jolán, Hornsent) — these are **TalkESD summon/invader-driven**; the m21 Enir-Ilim finale map has
  only one death-template entity (`21020450`, ambient). The flags near them in `common.emevd` are
  summon-availability/quest-progress, **not** death. To get true death state, mine their `.esd` TalkESD
  (look for the "dead" state transition) — not in the decompiled EMEVD set.

---

## MSB entity-bridge resolution (update — names now confirmed)
Resolved entity IDs → names via the MSBs (`tools/_resolve_entities.py` / `tools/_find_npc.py`:
entity → MSB Enemy `NPCParamID` → `nameId` → NpcName FMG in `item.msgbnd`). This **corrected two
agent mis-identifications and one of my own commits**:

| NPC | entity (MSB-confirmed) | death_flag | note |
|---|---|---|---|
| **Iji** | 1034490700 / 1034490711 (c4604, m60_34_49) | **1034499202** | ⚠ NOT `1034509403`. The agent's entity `1034500710` is **Ranni** (c2050); `1034509403` is Ranni's resolver flag. Iji's true flag (group 3760-3767, ns 1034499xxx) set ON at `common.emevd:38156`, OFF-reset `m60_34_49:117` — Seluvis-parallel. |
| **Irina** | 1045340700 (m60_45_34) | **3383** | "Irina of Morne", `90005702` |
| **Edgar** | 1045340705 / 1043310705 (Castle Morne) | **3403** | "Castellan Edgar", `90005702` |
| **Jerren** | 14000716 / 14000717 (m14 Academy) | **3363** | "Witch-Hunter Jerren", `90005702`; Sellen-finale |
| Nepheli | 10000730 / 10000732 | 4223 | confirmed (the "Gostoc=4223" guess was Nepheli) |
| Millicent | 15000700 / 15000703 | 4183 | confirmed (the "Jerren=4183" guess was Millicent) |
| Gostoc | **10000700-10000707** (c3665) | — | real entities; not a Quest Browser line, not wired |

**Now wired in `goblin_quest_steps.cpp` (23):**
- death-distinct base (10): Iji 1034499202, Seluvis 1034509302, Varré 1042369205, Yura 3623, Diallos 3443,
  Bernahl 3883, Gurranq 1051430800, Irina 3383, Edgar 3403, Jerren 3363.
- death-distinct DLC (6, via TalkESD pipeline): Ansbach 4403, Freyja 4423, Hornsent 4363, Leda 4443,
  Thiollier 4463, Dane 4563.
- `fail_conclusion=true` — shared concluded/dead flag, overlay shows "[concluded]" not "[unfinishable]" (7):
  Sellen 3463, Nepheli 4223, Kenneth 3583, Gowry 4163, Boc 3943, Patches 3683, Thops 3803.

## TalkESD pipeline (for the DLC followers) — SOLVED for 6/10
**Tool: `esdtool.exe`** (thefifthmatt/ESDLang v0.5.1, the ESD analogue of DarkScript3) at
`D:\tools\esdtool\esdtool-v0.5.1\`. Decompiles ER talk ESD → readable Python:
```
esdtool.exe -er -basedir "<game>" -writepy "<out>\%m\%e.py" -nobackup     # run from the esdtool dir (needs dist\); oo2core in cwd
```
Decompiled all 378 talk ESDs → `D:\tools\esd_py\<map>\t<TalkID>.py`. Map NPC→TalkID with `tools/_npc_talkids.py`.

**The pattern (bootstrapped from Patches):** each NPC's talk-template call is
`t..._x5(flag1=<dead>, flag2=<alive>, flag3=<state>, …)` — **flag1 = the dead/gone flag** (Patches flag1=3683
= his known death flag). For the DLC followers flag1 lands in a `44X0-44X3` networkconnected block
(dead = `44X3`), confirmed persistent in EMEVD (`BatchSetNetworkconnectedEventFlags(44X0,44X3,OFF)` resets;
Ansbach & Freyja also have explicit `90005702` death handlers at m21_01 — which the EMEVD-only pass missed).

**Cracked & wired (6):** Sir Ansbach **4403**, Redmane Freyja **4423**, Hornsent **4363**, Needle Knight Leda
**4443**, Thiollier **4463**, Dryleaf Dane **4563** (all death-distinct).

**Remaining tail (4):** Moore, Igon, Queelign, Jolán — their blocks exist (unmapped: 4480/4500/4520/4540/4580
+ 43xx) but need block→entity mapping (Moore=despair-merchant, Queelign=repeatable invader gate uses map-local
flags, Igon=post-Bayle, Jolán's NpcName didn't match the resolver). Plus base ESD-driven Roderika, Rya, Latenna.

## How to re-run / extend
Decompile (see `[[darkscript3-emevd-decompile]]` memory): `DarkScript3.exe /cmd -decompile -game er -indir <event> -outdir <out>`
(needs `oo2core_6_win64.dll` in cwd). Then enumerate every death registration:
`grep -E "InitializeCommonEvent\(0, 9000570[27]" D:\tools\emevd_js\err\*.js` → each line's args #3/#4 are `(entity, death_flag)`.
Bridge entity→name via the unpacked MSBs (`map/MapStudio`) for the unresolved ones (Jerren, Gostoc, DLC).
