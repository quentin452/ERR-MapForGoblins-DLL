# Windows RE — mine NPC "death" event flags from EMEVD (DarkScript3)

**For:** an agent on **Windows** with the Elden Ring game files + **DarkScript3**. Goal: build a
table `NPC name -> death event flag id` so we can fill `NpcQuest::fail_flag` in
`src/generated/goblin_quest_steps.cpp` (the Quest Browser greys an "unfinishable" questline
when its NPC is dead). This replaces the slow per-NPC save-diff capture we've been doing.

We already nailed 3 flags by save-diff (see "Ground truth" below). **Use them to bootstrap:**
grep the decompiled EMEVD for these known flags, learn the encoding pattern + the
entity-id→NPC mapping, then enumerate the rest.

---

## Ground truth (verified in-game, ERR 2.2.9.6 save) — your validation anchors

| NPC | death flag | notes |
|---|---|---|
| White Mask Varre | **1042369205** | his quest namespace 1042369* |
| Iji | **1042600001** | the 1042600* "cluster death" block |
| Seluvis | **1034509302** | = `seluvis_q99` in the EldenRingQuestLog ESD; his 1034509* namespace |

These are **cross-location-persistent** (true even when the player is far from the NPC's
region — NOT proximity flags). When you decompile the EMEVD, you MUST be able to find the
event(s) that `SetEventFlag(<one of these>, ON)` on the NPC's death. If your reading of the
EMEVD reproduces these three, your method + entity mapping are correct → extract the rest.

A useful cross-ref: the **EldenRingQuestLog** repo (github EldenRingQuestGiver/EldenRingQuestLog,
`src/erquestlog_quests.hpp`) lists per-NPC quest-step flags including a `*_q99` "concluded"
flag for many NPCs — Seluvis's (1034509302) matched his q99 exactly. Blaidd's q99 = 3603,
Kale alive/hostile/dead = 4700/4701/4703 (commented in that file) show the low-flag NPC
alive/dead convention too. So an NPC "dead" flag is sometimes a low id (e.g. 4703), sometimes
in the NPC's quest namespace — the EMEVD is the authority.

---

## Setup

1. **DarkScript3** (github.com/thefifthmatt/DarkScript3) — decompiles `*.emevd.dcx` to a
   JS-like script you can read/grep.
2. **EMEVD files.** Elden Ring stores events as `event/*.emevd.dcx`:
   - `common.emevd.dcx`, `common_func.emevd.dcx` (shared logic / macros)
   - `m{AA}_{BB}_{CC}_{DD}.emevd.dcx` per map block.
   Extract them from the game with **UXM** (unpack `eldenring.exe`) if not already loose.
   The user's profile is **ERR (Elden Ring Reforged)** via ModEngine — if ERR ships modified
   EMEVD (check `<ERR>/...` mod dir for `event/`), decompile THOSE; otherwise vanilla EMEVD
   is the baseline (NPC death flags are rarely changed by overhauls, but verify against the
   3 ground-truth flags).
3. Decompile every emevd to text once, so you can grep across all of them.

## Map blocks to focus on (where the quest NPCs live)
- m10/m11 Limgrave/Stormveil, m12 (Weeping/caves), m14 Liurnia (Caria/Three Sisters = Ranni
  cluster — Iji/Seluvis/Blaidd), m16 Roundtable?, m18 Leyndell, m19 Mt Gelmir/Volcano,
  m20/m21 Caelid, plus `common`/`common_func`. (Confirm exact ids in-tool; ER uses
  m60_XX_YY for overworld tiles — NPC events often live in the area emevd, not m60.)

---

## Method

### Step 1 — learn the pattern (bootstrap on the 3 known flags)
Grep the decompiled EMEVD for `1042369205`, `1042600001`, `1034509302`. For each:
- Find the event that calls `SetEventFlag(thatFlag, ON)` (or the bit-set instruction).
- Note what TRIGGERS it: typically an `IfCharacterDead(cond, <entityId>)` / a death-handling
  common_func macro keyed to the NPC's **entity id**. Record the entity id.
- Map entity id → NPC name (Varre/Iji/Seluvis) using the **MSB** (DSMapStudio map view) or the
  community entity list (soulsmods.github.io/data/er/entities.html).
This gives you the template: "NPC death event → SetEventFlag(deadFlag)".

### Step 2 — enumerate the target NPCs
For each NPC below, find its entity id (MSB / entity list), then its death event in the
map/common EMEVD, then the flag it sets. Prefer the flag that is:
- set ON when the character dies, and
- **persistent** (a "do not reset" flag — ER groups these; the `*_q99`/"concluded" or the
  NPC's dedicated dead flag — NOT a transient/region flag that resets).
If multiple flags set on death, give all + your best pick (the one the questline later checks
for "NPC gone", often the q99/concluded or a dedicated dead flag).

### Step 3 — deliver the table (see format below)

---

## Target NPCs (fill fail_flag for these — death = quest LOST)
Already done (validate only): **Varre, Iji, Seluvis**.
Need: **Patches, Boc the Seamster, Kenneth Haight, Gurranq/Beast Clergyman, Roderika,
Knight Bernahl, Nepheli Loux, Sage Gowry, Sorceress Sellen, Witch-Hunter Jerren, Diallos,
Yura, Thops, Latenna, Gostoc(if relevant), Kale(if a quest entry)**, and the DLC killable
ones: **Sir Ansbach, Moore, Thiollier, Dryleaf Dane, Freyja, Hornsent, Leda, Queelign, Igon,
Hornsent Grandam, Count Ymir, Jolan** (many DLC die in the Enir-Ilim finale or are
killable — give whatever death flag exists).

**SKIP (death = quest COMPLETION or unkillable, do NOT wire):** Ranni (invulnerable/
recoverable), Blaidd (death finishes Ranni's quest), Millicent / Fia / D / Rogier (their
quest concludes via death), Goldmask / Corhyn (Age of Order endpoint), Hyetta (Frenzied
ending), Dung Eater (choice ending). If unsure, still report the flag and tag it
"death=completion?" — we'll decide.

---

## Deliverable format (one block per NPC)

```
NPC: <name>
entity_id: <id>           # the chr entity whose death triggers it
death_flag: <flag id>     # the SetEventFlag the death event sets (our fail_flag)
also_sets: <other flags set on death, if any>
emevd_source: <file> event <id>   # where you found it
persistent: yes/no        # is it in the "do not reset" group? (must be yes to be usable)
confidence: high/med/low  # and why (matched q99? dedicated dead flag? multiple candidates?)
```

For the 3 ground-truth NPCs, confirm you can reproduce their flags and note the entity ids.

---

## Notes / caveats
- We only need flags that **persist regardless of player location** (the in-game grey-out
  reads the live flag anywhere). The save-diff method kept getting fooled by region/proximity
  flags that toggle — the EMEVD tells you which flag is the real persistent dead flag, so this
  is strictly better.
- ERR may rename/move events; if a ground-truth flag isn't found in vanilla EMEVD, the user is
  on ERR-modified EMEVD — decompile the ERR `event/` set.
- If an NPC has no dedicated dead flag (some only have an ESD "dead" state), say so — we'll
  fall back to save-diff for that one.
- Don't worry about wiring the DLL — just return the table; the Linux side fills
  `NpcQuest::fail_flag` and rebuilds.

## Repo references
- `src/generated/goblin_quest_steps.cpp` — the QUEST_BROWSER table (NPC names + the 3 wired
  fail_flags as examples).
- `tools/flagdiff.py` + `/tmp/ER-Save-Lib` errflags — the save-diff method we're replacing
  (use to spot-check an EMEVD flag against a save if in doubt).
- `docs/windows_re_briefing_playerpos_questbrowser.md` — the other Windows RE asks (player
  pos + map cursor), unrelated but same agent context.
