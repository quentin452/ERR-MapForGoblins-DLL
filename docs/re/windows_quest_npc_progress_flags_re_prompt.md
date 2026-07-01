# RE prompt — source real progress_flag/entity_id for the Quest NPC bootstrap set

**Status:** entity_id half DONE + progress_flag CONFIDENT STEPS WIRED (offline, 2026-07-01, schema
extended to register≥value). Remaining: mid-step flags (need per-gate RE / in-game) + in-game §7 verify.
Windows preferred (SoulsFormats/pythonnet param+MSB tooling, EMEVD decompile). Blocks:
`docs/plans/feat_quests_implementation_plan.md` Phase 2 verification — `QuestNpcLayer` is built and
wired (`feat/quest-npc-layer`, log-confirmed crash-free in-game).

**Done 2026-07-01 (offline, build-clang + build-erte green):** per-step `entity_id` MSB-sourced via
`tools/_find_npc.py` + `data/tile_region_map.json` region resolve and wired in
`src/generated/goblin_quest_steps.cpp` for the confident steps: Alexander 1–5 (Stormhill / Gael Tunnel /
Redmane / Mt. Gelmir / Farum Azula), Thops 1/2/3 (Church of Irith ×2 / Academy classroom), Boc 1/5/6
(Limgrave bush / Altus ×2). Resolved the prompt's open candidate question: `Boc 11050730` → Leyndell
Ashen Capital = **not** any of the 6 steps (correctly NOT wired); `Thops 1039390700` → Liurnia = step 1
(wired). Left at 0 (no offline source): Boc 2/3/4 (Coastal Cave + two ambiguous Liurnia placements),
Thops 4 (corpse). Also fixed two blockers found en route: a Windows file-lock bug in `_find_npc.py`
(`frombytes` temp-file reuse) and a schema-sync bug in `tools/gen_nonerr_stubs.py` (only-if-missing
stub never tracked the new `QuestStep` fields / `quest_step_done`, breaking every non-ERR build).

**progress_flag — TRACED, and the finding BLOCKS naive wiring (2026-07-01).** The EMEVD corpus IS
decompiled at `D:\tools\emevd_js\err` (516 `.emevd.dcx.js`; the old `D:\tools\DarkScript3` pointer was
stale/empty). Traced each NPC's block (TalkESD-isolated: Boc 3940–3949 / Thops 3800–3806 / Alexander
3660–3671) to its quest state-machine event (Boc ev3959, Thops ev3819, Alexander ev3679 in
`common.emevd.dcx.js`). **These flags are mutually-exclusive STATE REGISTERS** (`BatchSet...(lo,hi,OFF)`
then `Set...(value,ON)` — one ON at a time, advancing CLEARS the prior), NOT per-step booleans. So the
current `QuestStep::progress_flag` (one flag, SET==done) is structurally wrong for them: wiring e.g. Boc
3946 ticks the box then UNticks it at 3947. NOTHING wired as progress_flag. The register transitions are
gated by AREA flags matching the entity_id regions (Alexander Stormhill `1043399307` / Redmane
`1051369259` / Gelmir `1035539208` / Farum `1052520800`; Thops Irith `1039399220`) — which independently
CONFIRMS the wired entity_ids. See `docs/memory/features/quest-browser.md` (PROGRESS_FLAG STRUCTURAL
FINDING) for the full state machines. **DECISION (user): extended the schema to "register ≥ value" —
DONE.** `QuestStep::progress_flag_max` + `quest_step_done` OR-scan; `quest_npc_layer` active-step got a
`flag_floor`. Wired the confident steps (Alexander 1/3/4/5, Thops 4, Boc 6 — the rest stay manual: no
confident mapping / missable / no register granularity). build-clang + build-erte green. STILL TODO:
in-game §7 visual verify (game not running); mid-steps (Alexander step 2, Boc/Thops steps 1-3) need
per-gate-flag RE (the ESD/parameterized gate flags with 0 EMEVD literal setters) or in-game capture.

## Goal

For the 3 bootstrap NPCs already in `src/generated/goblin_quest_steps.cpp` (Boc, Alexander, Thops),
source a real, **per-step** `entity_id` (MSB EntityID of the NPC at that step's location) and
`progress_flag` (EMEVD event-flag id whose SET state means that specific step is done) for as many of
their 15 steps as can be confidently verified. Partial coverage is fine — wire whichever steps you can
verify, leave the rest at `0` (no marker, falls back to the existing manual ini checkbox).

**Do NOT guess.** A wrong `entity_id` pins the marker at the wrong location (or a stale/decommissioned
MSB part); a wrong `progress_flag` makes the Quest Browser checkbox lie about progress. Both are
silent-wrong, not crash-wrong — there's no compiler or runtime check that catches a plausible-looking
but incorrect id. Verify before wiring, the same standard the schema comments already hold this to.

## The 15 steps (what each one actually IS, for cross-referencing in-game/EMEVD)

`steps_boc[]` (6 — `goblin_quest_steps.cpp:17-22`):
1. Free Boc — bush by the road, western Limgrave (entity = the caged/bush Boc placement)
2. Coastal Cave — mouth of Coastal Cave, Limgrave west shore
3. Lake-facing Cliffs — eastern Liurnia
4. Tailoring tools — same Liurnia location, no relocation (give-item step, may share step 3's entity)
5. Altus Plateau highway
6. Resolve his wish — same Altus location, no relocation (gesture/item step, may share step 5's entity)

`steps_thops[]` (4 — `goblin_quest_steps.cpp:25-28`):
1. Meet Thops — Church of Irith, eastern Liurnia
2. Academy Glintstone Key — same location, give-item step (may share step 1's entity)
3. Schoolhouse Classroom — Raya Lucaria Academy (relocated)
4. His end — same Classroom, but Thops is DEAD here (a different entity state / no live entity, or a
   corpse pickup — verify whether this step even has a meaningful `entity_id`, may need to stay 0)

`steps_alexander[]` (5 — `goblin_quest_steps.cpp:83-87`):
1. Stuck in Stormhill — hole, northern Stormhill, Limgrave
2. Gael Tunnel — Caelid
3. Radahn Festival — Redmane Castle, Caelid
4. Lava pot — Mt. Gelmir
5. His end — Crumbling Farum Azula (boss-fight step, likely shares the boss encounter's own entity if
   one exists, or stays 0)

Give-item / no-relocation steps (Boc 4, Thops 2, possibly others) likely reuse the PRIOR step's
`entity_id` — confirm rather than assume; some "stay here" steps in this engine still re-trigger via a
distinct EMEVD region/condition even without a new MSB placement.

## Sourcing entity_id — use the existing toolkit, don't re-derive

`tools/_find_npc.py` already does exactly this (built for a prior NPC RE pass, generalizes fine):
```
py tools/_find_npc.py Boc Alexander Thops
```
Cross-reference its output (per-map MSB placements) against the step list above by map name (Limgrave =
`m60_*`, Liurnia = `m31_*` roughly — verify, don't assume area codes). `data/msb_entity_index.json` is
the same data pre-extracted if you want raw lookup instead of rerunning the tool — but it's keyed by
internal model-instance name (e.g. `c0000_9032`), not "Boc", so `_find_npc.py`'s NAME-based search is the
practical entry point, not a manual grep of that file.

**2 candidate values already exist** in `goblin_quest_steps.cpp` (lines 364-368, the `Confirmed:`
fail_flag verification comment; re-stated per-NPC at lines 405-406 (Boc) and 457 (Thops)):
`Boc 3943 (11050730)`, `Thops 3803 (1039390700)`. These were captured for a
DIFFERENT purpose (death/conclusion-flag verification) and the comment doesn't say which of the 6/4 steps
above that single MSB placement corresponds to. Verify which step it belongs to (likely an early one,
since most NPCs are first encountered at a fixed spot) before reusing it as that step's `entity_id` — do
not assume it's step 1 without checking the actual MSB map name against the step's zone.

## Sourcing progress_flag — empirical, via the existing live coverage tools

There's no static "step N done" flag table to mine — EMEVD doesn't expose one queryably (per the
`darkscript3-emevd-decompile` memory's death-flag work, even THAT required manual common-event tracing).
For PER-STEP progress (not death), the practical approach is empirical, using tools already built into
the overlay:

1. **`config::debugEventFlags`** ("Event-flag hook — coverage-gap detector", F1 debug section) logs every
   newly-observed event flag to `logs/MapForGoblins_events.log` (confirmed: `dllmain.cpp` near line 268-271
   wires it). Arm it, perform ONE step's specific in-game action (e.g. strike the bush to free Boc), and look for
   flags that flip in that narrow window. Multiple flags will fire (dialogue, animation, misc state) —
   cross-reference against the already-decompiled EMEVD JS corpus at **`D:\tools\emevd_js\err`** (516
   `.emevd.dcx.js`; the `D:\tools\DarkScript3` path in `docs/memory/tooling/darkscript3-emevd-decompile.md`
   is stale/empty — use `emevd_js\err`) to confirm WHICH flag is semantically "this step is done" rather
   than just "a flag that happened to fire". NOTE (2026-07-01): this cross-ref was DONE offline and found
   the quest progress flags are mutually-exclusive state registers, not per-step booleans — see the
   progress_flag status block at the top before re-doing this empirically.
2. `tools/resolve_emevd_positions.py` is a related EMEVD-search helper (built for the EMEVD-posless-award
   problem, not this exact task) — read it for the search PATTERN (how it greps the decompiled corpus for
   a given id), reuse the pattern, don't expect it to directly answer this question.
3. **Do not reuse `tools/generate_quest_gates.py`'s flags** (`"boc": (["Boc"], [1043379355])` etc.) — those
   are whole-questline "is the quest active at all" gates (EldenRingQuestLog-sourced), already checked and
   confirmed wrong-semantics for per-step progress in `goblin_quest_steps.cpp`'s comments. Don't re-derive
   that mistake.

## Output — exact edit target

`src/generated/goblin_quest_steps.cpp`, the `steps_boc[]`/`steps_thops[]`/`steps_alexander[]` arrays.
Current shape per entry: `{title, desc, zone}` (3 positional, the other 2 default to 0). Add the 2 new
trailing values only where verified:
```cpp
{"Free Boc", "...", "Limgrave", /*progress_flag=*/12345678u, /*entity_id=*/11050730u},
```
Update each row's surrounding comment to record the verification method used (which tool, what you
observed) — match the style of the existing `// MSB-confirmed: entity ...` comments already in this file,
so the NEXT person (possibly extending this to the other 31 questlines) can tell sourced-and-verified
apart from bootstrap-hand-authored-only.

## After wiring

Build (`build-erte`/`build-linux` whichever profile), deploy, and visually verify per
`feat_quests_implementation_plan.md` §7: exactly one `WorldQuestNPC` marker for the active step, correct
position, and (if you also touch a write-tested flag) `questAllowFlagWrite` OFF shows the checkbox as a
read-only `[auto]`-tagged mirror. Update `docs/HANDOFF.md` + this file's status line when done.
