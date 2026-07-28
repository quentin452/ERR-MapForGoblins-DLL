# Runtime randomizer — scope, keep/drop, invariants

**Status:** SCOPING ONLY. Nothing implemented, nothing decided beyond what is written here.
Captured 2026-07-27 because the exploring conversation was running out of context.

**What this is:** a randomizer built on MapForGoblins' runtime param layer instead of on rewriting
`regulation.bin` + `map/` + `event/` files. Feature list reimplemented from the *observed behaviour*
of two existing mods; see LEGAL at the bottom before touching either.

References (the mods being learned from, NOT copied):

- Item and Enemy Randomizer — [nexus 428](https://www.nexusmods.com/eldenring/mods/428), source at
  [thefifthmatt/SoulsRandomizers](https://github.com/thefifthmatt/SoulsRandomizers) under
  `EldenRingRandomizer/`. Local copy of v0.11.4 lives in `~/Downloads/Sandbox/`.
- Fog Gate Randomizer — [nexus 3295](https://www.nexusmods.com/eldenring/mods/3295).

---

## Why runtime at all (the case, in one place)

Measured this session, not assumed:

1. **Mod compatibility.** The file-rewriting randomizer keys its placement tables on vanilla param
   row numbering; its own README warns "some mods cannot be merged with item randomizer currently,
   and will always result in errors" — which is what Convergence hits. Our param layer reads
   whatever regulation is loaded, so it is mod-agnostic *by construction*, not by effort.
2. **The save↔world mismatch disappears.** Today's failure mode is: re-roll → item locations move →
   but pickup flags are per-LOCATION and live in the save, so anything placed behind an already-set
   flag is permanently unobtainable, and anything already carried duplicates. If the permutation
   lives in the save's own sidecar, the world is DERIVED from the save and the two cannot diverge.
3. **Nothing is overwritten**, so uninstalling or switching mods cannot corrupt anything.

Determinism note (measured): the existing randomizer IS deterministic on
`seed + options + preset content + versions`. Two rolls of seed `986754587` were byte-identical
except the "Writing …msb.dcx" tail (output order). The trap is that the options string records
`--preset Custom` — the preset's NAME, not its CONTENT: two rolls with identical option lines and
identical seed produced different worlds because the custom enemy preset had been edited between
them. So "same settings" in the UI is not the same world.

---

## Item randomizer — keep / drop

### Keep (v1)

- Randomize **world item pickups** (the lot-backed loot the marker layer already enumerates).
- Randomize **enemy/boss drops** — same `ItemLotParam` family, different lot type.
- **Key-item logic**: key items placed only where they are reachable (see INVARIANTS).
- **Seed + options**, and an export/import of the *whole* configuration (learning from their trap:
  export must include everything that affects the roll, never a name pointing at mutable content).
- **Spoiler log**, reusing `tools/audit_markers_vs_spoiler.py`'s format so the existing audit works.
- **In-game hints / "what is here"** is nearly free for us: the map already resolves each lot's item
  live, so a randomized world is *already* readable on our map. This is the one feature we get for
  almost nothing that costs them a separate mod (randomizerHints).

### Drop, or defer

- **Enemy randomization** — out of v1, but **NOT walled** (see the correction below). Deferred on
  effort and unproven timing, which is a scoping choice, not a capability limit.
- **Shop lineups** — possible later (`ShopLineupParam` is a param) but not v1.
- **Starting loadouts, character edits, enemy stat scaling** — param-driveable, but they are balance
  mods bolted onto a randomizer, not the randomizer.
- **Their presets/config formats** — deliberately NOT reimplemented (see LEGAL).

### ★ Correction (2026-07-27) — the enemy "wall" was mis-stated

The first draft of this doc said enemy randomization is impossible because placement lives in the MSB
and MSB-write is a WALL. **That conflates two different things.** The wall is *adding* a placement or
*writing MSB files*. A randomizer does neither: it **re-assigns placements that already exist** — a
permutation, not a spawn. What it rewrites per placement is only **`ModelName` + `NPCParamID`**, and
we measured exactly that on the reference mod: it keeps the original part name and swaps those two
fields on 19439 of 22959 placements (84.7%) — [`msbe_parser.hpp:85`](../../src/worldmap/msbe_parser.hpp).

The mechanism, assembled from RE this repo already owns:

- The decompressed MSB is **resident in memory** (~25 blobs) and we already parse it, enemies included
  — `Enemy{ modelName, npcParamId, talkId, pos, entityId }`, `docs/re/windows_runtime_msb_resident_re_findings.md`.
- `docs/re/windows_msb_placement_write_re_findings.md` proves a resident-MSB write is inert **post-load**
  *because the data is snapshotted before the object exists*. Read in the positive, that says a write
  **before** the parse/spawn phase IS honoured — it is route 2 of that doc's own ADD section
  ("patch the loaded resource before the driver's phase").
- So: patch two u32 per Enemy part in the resident blob, in the window before the tile streams. No file
  written, no placement added, no spawn entrypoint needed — and mod-agnostic by construction, since we
  permute whatever the loaded mod placed.

**Three unknowns before this can be promised** (none measured yet):

1. **The patch window.** `CSMsbParts::ctor` already copies by value, so the patch must land before the
   *parts parse*, not merely before the instance spawn — a narrower window than it first looks. Does the
   existing load wedge (`docs/re/windows_load_wedge_mapid_writer_re_findings.md`) sit on the right side of it?
2. **`modelIndex` indexes the tile's own MODEL section**, so the candidate set is the models already
   declared in that MSB (else the chrbnd is not resident). That constrains us to an intra-tile shuffle
   unless the MODEL section can be grown in memory (fixed-size → hard). The file-rewriting mod dodges this
   by rewriting that section. **This is the real limiter, and it may make the result too weak to want.**
3. **Do chr placements even come from the resident blob**, or from a copy parsed earlier still? Proven for
   geom, only assumed for chr.

Status: an RE prompt, not a plan. Unknown 2 decides whether it is worth doing at all, so measure the
candidate-set size per tile *before* anything else — a shuffle that can only swap 3 models per tile is
not a randomizer. **Still out of v1 regardless:** items must work first, and this shares no code with them.

## Fog gate randomizer — keep / drop

Observed behaviour: boss gates, multiplayer gates and warps are all randomized; fog gates become
permanent and traversing one warps you to the far side of a *different* gate; warps are fixed per
seed; two modes (shuffle the whole worldspace, or exclude the open world for a dungeon-crawl); options
to pick which gates may be required, and extra requirements before the final boss.

- **Defer the whole thing to v2 at the earliest**, and only after the item randomizer works.
- Feasibility is unproven and probably worse than items: a fog gate's destination is a warp, and this
  repo can already warp (`warp`, `warp_xyz`, grace warps) — but *rebinding a gate's own destination*
  is event/asset-side, close to the MSB/ESD walls. **Investigate before promising.**
- If it ever happens, the reachability solver is shared with the item side — the graph is the same
  object. That is an argument for designing the solver graph-first rather than item-first.

---

## INVARIANTS (the part that must not be got wrong)

> These are the **what**. The **how** — the fixpoint model, assumed fill, the independent verifier and
> the directional-error policy that actually deliver 1/2/6 — is the SOLVER section below (2026-07-28).

1. **No key item behind itself.** The classic softlock: the item that opens a door placed behind that
   door. Requires a real reachability solve, not a shuffle with retries.
2. **No softlock, transitively.** Not just self-reference — no *cycle*. A behind B, B behind C, C
   behind A is equally fatal and is what a naive check misses.
3. **The permutation is immutable for a save.** Once a save has played one frame against a mapping,
   that mapping must never change — see the save↔world failure above. Store the FULL permutation in
   the sidecar, not just a seed: re-deriving from a seed assumes the input lot set is unchanged, and
   it changes the moment the player updates their mod, which recreates the exact bug we are fixing.
4. **Refuse rather than corrupt.** If the sidecar's mapping references lots that no longer exist
   (mod changed under the save), do NOT re-roll and do NOT silently drop them — warn and refuse to
   apply, before the player commits time.
5. **Never write the real save.** Everything DLL-owned goes in `<save>.mfg` (`goblin::sidecar`).
6. **Progression items must stay obtainable at all**, not merely placed: an item on a lot whose flag
   is already set is invisible. The solver must treat "already collected" as "not a valid slot" for
   any save that already exists — which is another way of saying invariant 3.
7. **Mod-agnostic acceptance test** (`AGENTS.md`): does it still produce a correct result on a
   DIFFERENT mod with different params? If it only works because values match vanilla, it is not done.

---

## What MapForGoblins already provides

| Need | Existing piece |
|---|---|
| Enumerate every loot location | the marker layer (lot id + resolved item + collected flag) |
| Read a lot's contents live | `goblin_loot_resolve`, `resolve_loot_item_textid`, `aeg_pickup_lot` |
| Item taxonomy without a table | `goods_ids_of_type`, `item_marker_category`, `classify_item_live` |
| Write the permutation | `paramedit::param_set_field` + the boot-time override loader (`goblin_param_overrides`) |
| Per-save state | `goblin::sidecar` (`<save>.mfg`, never touches the `.sl2`) |
| Event flags | `ui::read_event_flag`, `markers::set_event_flag`, RPC `flag` |
| Param inspection | RPC `param_get` / `param_set` / `param_rows`, `text` |
| Verification | `tools/audit_markers_vs_spoiler.py` |

The genuinely missing piece is **the reachability solver** — but the solve *algorithm* was never the
risk. The risk was its INPUT: the reference mod ships a curated logic table, which LEGAL forbids us to
take and the mod-agnostic doctrine forbids us to embed. So: **is the graph derivable from the loaded
game?** Measured below, before anything else was decided.

---

## GRAPH SOURCE — MEASURED 2026-07-27 (`tools/_probe_progression_gates.py`)

Method: scan the ACTIVE install's own `event/*.emevd.dcx` for the engine's own "is this locked for
you?" test (`IF Player Has/Doesn't Have Item`, +BBox variant), resolving the caller-supplied ids of
the generic templates through their `InitializeEvent`/`InitializeCommonEvent` args; then classify each
gating item by `goodsType` from the loaded regulation; then the second, disjoint lock class the EMEVD
scan misses — `ObjActParam.spQualified{Type,Id}`, the door/lift requirement carried by the param itself.
No bake, no table, no hardcoded id list.

| | ERR (517 emevd) | Convergence (592 emevd) |
|---|--:|--:|
| possession-test instructions | 3967 | 1114 |
| …caller-supplied id, resolved via initializer | 426 | 7 |
| …caller-supplied, no caller found | 42 | 32 |
| distinct items tested (raw, noisy) | 3098 | 295 |
| …of which **Key Items** (`goodsType==1`) | 242 | 23 |
| ObjAct slots with an item requirement | 29 | 28 |
| **distinct ObjAct item requirements** | **19** | **19** |
| ObjAct slots gated by an event flag | 31 | 28 |

Vanilla could not be measured on this box (game not UXM-unpacked) — a machine gap, not a script gap.

**Readings:**

1. **The traversal-lock axis is tiny and STABLE across two unrelated overhauls: 19 distinct item
   requirements in both.** These are the physical locks (lifts, sealed doors) — and they are the class
   the EMEVD scan *misses entirely*, which is why the naive single-source count was wrong twice before
   this table (a few of the 19 look like sentinels/non-goods — 0 appears — so 19 is a ceiling).
2. **The raw EMEVD count is not a lock count.** 3098 "items that gate something" collapses to 242 key
   items, and even those are mostly dialogue/quest conditions (ERR's `Grace of <NPC>` ×60, `Bounty N
   Complete`, whetblades — a whetblade gates a smithing menu, not a room). The ERR-vs-Convergence spread
   (242 vs 23) is exactly this quest-item noise, not a difference in world structure.
3. **⇒ The graph is FLAT on the axis that matters, and derivable.** A ~200-line script, one session,
   two mods, zero curated data. The doc's biggest unknown is answered: **we do not need their table.**

### The FLAG axis — measured 2026-07-27 (`tools/_probe_flag_gates.py`)

The item axis is only half the graph; **event flags** (boss doors, quest state) are what give it depth.
Method, same doctrine — instructions located by NAME in the EMEDF, never by hardcoded `bank:id`: an
event containing a traversal action (`Set ObjAct State`, `Change Asset Enable State`, the warp family)
is a traversal event, and every Event-Flag test inside it is a candidate gate; caller-supplied flag ids
resolved through the initializers; flags a traversal event both tests AND sets are bookkeeping
("this door is already open") and dropped.

| | ERR | Convergence |
|---|--:|--:|
| traversal events (door / asset / warp) | 645 | 584 |
| …gated by NO flag at all (free edges) | 81 (12%) | 98 (16%) |
| distinct EXTERNAL flags gating traversal | 2089 | 1797 |
| …appearing once, in one map (per-instance state) | 787 | 607 |
| flags set by a boss-defeat / death event | 676 | 642 |
| **…that gate traversal — the boss-chain depth** | **175** | **162** |

**Readings:**

1. **The flag axis is NOT flat like the item axis — but it is still derivable and still small enough
   to hold.** The structural core is ~175 boss-death edges + the 19 item locks, on both mods. That is a
   solver-sized graph, derived from the loaded install, with no curated table anywhere.
2. **12–16% of traversal is ungated** — consistent with ER being an open game where most edges are free.
3. **The boss-flag family had to be derived, not looked up.** `WorldMapPointParam.clearedEventFlagId`
   (the map-icon flag, Godrick = 510010) is NOT the flag the EMEVD sets and tests (10000800), and
   classifying against it found 21 gates instead of 175. The mod-agnostic derivation is "flags set by an
   event that announces a boss/miniboss defeat or tests a character's death".

**Limitation, stated so it is not mistaken for precision:** a flag counts as a gate if it is tested
*somewhere in the same event* as the traversal action — co-occurrence, not proven causation. Following
the EMEVD condition-group wiring would sharpen it. So **2089 is a ceiling**; the 175 death-gated edges
are the part that carries structure. A refinement, not a blocker.

**Verdict on the doc's biggest unknown: the graph is derivable on BOTH axes.** The solver can be
specified. What remains before implementation is the module-vs-separate-mod decision, which is now
unblocked.

---

## DECIDED 2026-07-27 — a MODULE of MapForGoblins

**Decision (quentin): module, not a separate mod.** Conditions that come with it, so the cost side of
the trade-off below is actually paid and not just acknowledged:

- **Off by default**, behind one hard gate. A user who never enables it must be unable to reach any of
  this code.
- **Its own config section**, not N new toggles sprinkled into the existing schema — the
  complexity-ceiling rule in `CLAUDE.md` applies the moment the randomizer starts growing options.
- **Write surface stays bounded by invariant 5**: params at boot + `<save>.mfg`. Nothing else, ever.

The argument that had been carrying the "separate mod" side **does not hold**: a separate mod runs in
the SAME process, so it isolates *code*, not *runtime* — a solver crash takes Elden Ring down either
way. Separation would have bought no containment while costing either a shared library (the unresolved
ER-DeathCounter Bug 6 conversation) or a second Present hook (`multimod-hook-coexistence.md`). What
actually contains the blast radius is the write surface, which invariant 5 already bounds identically
in both architectures. Symmetric with, and for the same reason as, the ER-DeathCounter absorb decision.

The trade-off as it stood, kept for the record:

**As a module.** Reuses all of the above with zero glue; ships with the map that already displays
the randomized world; one DLL to install. Cost: scope creep in a map mod, a much bigger blast radius
for bugs (a randomizer bug now sits in the same process as the map everyone uses), and the config
surface grows again — the `CLAUDE.md` complexity-ceiling rule applies.

**As a separate mod.** Clean boundary, opt-in, failure is contained. Cost: it needs the param layer,
the loot resolver, the sidecar and the flag reader — i.e. most of MapForGoblins — so it means either
a shared library (the Bug 6 conversation from the ER-DeathCounter side, still unresolved) or a second
Present hook in the process (the multi-mod hook war, `docs/memory/bugs/multimod-hook-coexistence.md`).

Note the symmetry with the ER-DeathCounter decision earlier today: the same question, and there the
answer was "absorb it, because the standalone would have to port half the data stack". The difference
here is blast radius — a tracker cannot break a save, a randomizer can.

This was the gate on implementation, because it decides where the solver lives. **It is now open: the
solver lives in MapForGoblins.** Next is its spec — graph-first (see the measured graph source above).

---

## SOLVER — how completability is GUARANTEED (design, 2026-07-28)

The INVARIANTS above say *what* must not break. This says *how*. Nothing here is implemented; the
algorithm choices are settled, the derivation gaps are named.

### The model — least fixed point over a monotone state

State = `(items held, flags set, nodes reached)`. Each edge carries a boolean formula over items and
flags (the 19 ObjAct locks + the ~175 boss-death edges measured above). Reachability = the **least
fixed point** of a monotone function, computed by Kleene iteration: take what is reachable, collect
its items/flags, repeat to stability.

**Invariant 2 (no transitive cycle) therefore needs no cycle detector.** A cycle `A←B←C←A` simply
never enters the fixed point — the iteration converges without it and the goal is not met. Cycle
detection bolted onto a shuffle is exactly what misses this; a fixpoint cannot be fooled by it.

### Monotonicity — the load-bearing hypothesis, stated so it can be attacked

The model assumes **reaching a node once grants access forever**. In ER that holds because of grace
fast-travel — a luxury a Zelda/Metroid randomizer does not have (there, "can I get back" must be
modelled). If this ever falls, the solver changes *nature*, not just a special case. So it is
recorded as a hypothesis, with its evidence:

- **Data point (quentin, 2026-07-28, not reproduced by us):** with a "reveal all graces" cheat,
  Leyndell Royal Capital remained warpable *after* burning the Erdtree. ⇒ the pre-burn map is **not
  destroyed**; Ashen Capital is an added map, not a destructive replacement.
- **Confound:** that cheat forces the very bit a filter would clear. Two worlds remain open —
  **(a)** the graces stay in the normal warp list post-burn, or **(b)** the game filters them and the
  cheat bypassed exactly that filter.
- **Cheap settle, no new probe:** we already read the game's own per-grace state byte
  (`registered/discovered/visible`, `warpData+0x8 +0x1E`) at warp-pin build time —
  `src/goblin_grace_suppression.cpp:52`, with its `[WARPPIN]` log. Burn, re-read, done. Static side:
  `BONFIRE_WARP_PARAM_ST` carries `eventflagId`, `clearedEventFlagId`, `dispMask0/1`.
- Worst case (b) costs little: pre-burn-only lots become junk-only. No content leaves the shuffle,
  only placement depth.

**⚠ Capability-dependence — do NOT let the mod into the logic.** MapForGoblins can warp anywhere
(`goblin_warp.cpp`), which would paper over (b). Tempting and wrong: a graph that assumes "any lit
grace is warpable *because the mod can*" becomes valid only while a toggle is on. Turning it off, or
loading the save without the mod, softlocks retroactively — and unlike invariant 3 it is the *graph*
moving under the permutation, invisibly. **Rule: the graph stays valid under VANILLA capabilities;
the mod's warp is a RESCUE (below), never an edge.** A capability is safe only while it is not
*assumed* by the proof.

### The placement algorithm — assumed fill

1. Split into a progression pool `P` and junk `J`.
2. Remove one item `i` from `P` at random. **Assume the player holds all of the remaining `P`**,
   compute the fixed point → the set `L` of reachable, still-empty locations.
3. Place `i` at random in `L`. Recurse until `P` is empty; fill the rest with `J`, unconstrained.

By induction, `i` always lands somewhere reachable *without* `i` and without anything placed deeper,
so no circular dependency can form. **Zero retries, zero rejection sampling**, and items land deep.
(Forward fill also guarantees completability but frontloads everything — a boring randomizer.)
`L` empty at any step ⇒ **loud failure, re-seed**. Never a degraded placement.

### The independent verifier — because the placer can be wrong

A second module sharing **no code** with the fill runs a sphere search from the empty state and
asserts: the goal flag is in the fixed point; (option `all-reachable`) every location is; and no `P`
item sits on a lot whose flag is already set in the target save (invariant 6 — an *apply-time* check,
not a roll-time one).

Falls out for free: the **spoiler log** (the spheres *are* the playthrough, in
`tools/audit_markers_vs_spoiler.py`'s format) and a **seed-quality metric** — sphere count, depth of
the last progression item, size of sphere 0 — so degenerate seeds can be *rejected*, not just
validated.

### Directional error policy — the real risk is the graph, not the algorithm

The algorithm is known-good. The risk is that the graph is derived by heuristic scan (this doc's own
"co-occurrence, not proven causation"). Errors are **not symmetric**:

| Derivation error | Effect | Verdict |
|---|---|---|
| Requirement too strong / lock invented | solver over-constrained | **safe** (worst case: infeasible roll → loud failure) |
| Whole edge missed | region believed unreachable | **safe** (smaller pool) |
| **Requirement missed on a known edge** | edge believed free | **SOFTLOCK** |
| **Edge invented** | idem | **SOFTLOCK** |

Hence three rules, to be applied literally:

1. **Edge requirements: OVER-approximate.** The 2089-flag ceiling stays as measured; do not prune it
   for elegance.
2. **Progression pool: OVER-approximate.** All 242 noisy ERR key items go in `P`. A useless quest
   item placed carefully costs nothing; a real lock classed as junk kills the save.
3. **Progression-eligible LOCATIONS: UNDER-approximate.** The decisive lever: the graph does not have
   to be right everywhere, only on the subset where a progression item may land. Everything else takes
   junk and can be shuffled freely.

**Corollary — default-deny.** The 42/32 "caller-supplied, no caller found" gates and the 12–16 % of
traversal with no flag found are **unknowns, not free edges**. An unknown is treated as blocked (or
its locations leave the eligible set). That is precisely where a softlock would hide.

### Two exclusions, not one

| Exclusion | Protects |
|---|---|
| cannot **RECEIVE** a progression item (junk-only) | the graph — nothing vital lands somewhere unproven |
| cannot **DONATE** its item to the pool (vanilla-locked) | the chain — the item stays where the game expects it |

Conflating these was a drafting error worth naming: quest-chain items and endings are preserved by
the **second**, not the first. Junk-only on a quest slot would still rip the quest item out of it.

### Runtime safety net

- The map already resolves each lot live, so a player is never *lost*, at worst spoiled.
- **Rescue, not re-roll:** if the verifier finds at load time that the current save can no longer
  reach the goal, grant the missing item (or warp) via the sidecar. **Grant ≠ remap** — invariant 3
  holds, the permutation never moves.
- **Refuse rather than corrupt** (invariant 4) — warn before the player invests time.

---

## SOFTLOCK TAXONOMY — ELDEN RING (design input, 2026-07-28)

> **⚠ The instances named below are from general ER knowledge, NOT measured on this box.** This is a
> **catalogue of CLASSES**; every instance is a hypothesis to be derived per install (doctrine: no
> embedded table). The classes are the deliverable; the examples only make them concrete.

**A — handled natively by the boolean fixpoint**

1. Key behind its own door, and any transitive cycle.
2. Boss chains (a defeat flag opening a region).
3. Physical item-gated doors — the 19 `ObjActParam.spQualified*` locks.
4. **Traversal items — Torrent above all.** Many routes require the horse. ⚠ *Invisible to both
   probes*: not an EMEVD test, not an ObjAct requirement — it is geometry.

**B — needs machinery beyond booleans**

5. **Counted consumables.** Stonesword Keys (finite, consumed per imp seal), Imbued Sword Keys. A
   boolean solver concludes "has one key ⇒ can open every seal". False. Needs **quantitative
   requirements** in the fixed point, and it is recursive since the keys are themselves shuffled.
6. **k-of-n thresholds.** Leyndell's "two Great Runes" is `|runes| ≥ 2` over a set that is itself
   randomized — neither a specific item nor a conjunction.
7. **Items consumed by handover** (given to an NPC and gone). Breaks monotonicity ⇒ **excluded from
   the progression pool**, no attempt to model.

**C — outside the item/flag graph entirely ⇒ exclusion, not modelling**

8. **One-way / world mutation** past a point of no return (the Erdtree burn) — see the monotonicity
   hypothesis above; maps look survivable, NPCs do not.
9. **NPC quests / ESD** — the repo's acknowledged wall. Failable, advanceable-past, killable.
10. **Online-gated progression.** One of the two chains into Mohgwyn (hence the DLC) runs through
    PvP invasions. On an offline or modded install that edge may not exist ⇒ **unknown = blocked**.
11. **Lot whose flag is already set** in the target save (invariant 6) — an apply-time problem.

**D — not a logic softlock**

12. **Power softlock:** logically completable, practically unbeatable (no upgrade materials, no
    usable weapon). No solver catches it; it is balance. Handled by junk-fill weighting.
13. **Menu gates, not room gates:** whetblades and friends gate a *menu*. This is most of the EMEVD
    noise (3098 → 242 → 19); over-approximating puts them in `P`, which is free and safe.

**Blind spots of the two existing probes**

| Class | Why invisible | Policy |
|---|---|---|
| Geometric traversal locks (Torrent, spiritsprings) | neither EMEVD nor ObjAct — collision | out of the eligible set, or an OPTIONAL user-supplied overlay |
| Counted consumables | possession is tested, quantity is not | quantitative requirements in the fixpoint |
| k-of-n thresholds | appears as N separate tests | encode the threshold |
| NPC quest slots | ESD, not parsed | vanilla-locked |
| Post-point-of-no-return mutation | the scan is atemporal | junk-only on the pre-mutation side |

---

## GOAL DEFINITION — multiple endings, and the DLC (2026-07-28)

**Multiple endings do not multiply the completability problem.** All ER endings share the same final
node (Radagon + Elden Beast). What differs is a **post-condition at the moment of choice**, not a
path. So the reachability target is **single**, and endings are optional extra requirements to
satisfy *before* it — the same shape as the fog-gate mod's "extra requirements before the final boss".

**Derivable, by the probe already written.** The ending choice is an **EMEVD decision tree in the
final map**: `IF Player Has Item (Mending Rune…)` / event-flag tests selecting a cutscene and the
end-of-game action — literally the instruction class `_probe_progression_gates.py` already counts.
Locate the end-of-game family **by name** in the EMEDF, read the branch conditions ⇒ the set of
endings *and* each one's prerequisite formula, derived. A mod that adds or changes an ending is
covered automatically; a hardcoded "6 endings" is not.

- **⚠ Here the condition-group wiring must actually be followed.** Co-occurrence is an acceptable
  over-approximation everywhere else; for endings we need *which branch*. Bounded to a handful of
  events, so affordable locally even though the doc defers the refinement globally.
- **Bonus — the goal flag itself is derived.** The target of the fixed point = the flag the ending
  tree tests as "the final boss is dead". No hand-designation of the final boss anywhere.
- **The endings survive for free** via the *vanilla-locked* exclusion above: quest-chain items stay
  in place ⇒ Ranni / Fia / Goldmask chains stay intact ⇒ every ending stays reachable, without
  modelling a line of ESD. Cost: a few dozen items out of the shuffle.
- Goal modes: `beatable` (default, single target, no extra cost) · `ending:<X>` (adds the derived
  branch formula) · `all-endings` (the conjunction, near-free given the vanilla lock).

### The DLC

**Expectation: SotE adds no new game ending** — its own final boss and closing cutscene, but the
end-of-game remains the base game's. **⚠ unverified.** Settle it with the same scan: look for the
end-of-game instruction family across *all* maps including DLC. This matters **more for mods than for
vanilla** — nothing stops an overhaul from adding an ending. While there: the 517/592 emevd counts
above do not split base vs DLC; a split by map-id prefix would size the DLC subtree.

The DLC is an **optional subtree** hanging off `Radahn ∧ Mohg ∧ cocoon`, with no required return into
base-game progression. Hence three roll modes:

| Mode | Effect |
|---|---|
| `dlc = excluded` (default) | DLC lots receive no base-game progression. Zero risk |
| `dlc = in-logic` | DLC lots eligible ⇒ the entry edge becomes a **hard requirement** of the run |
| `dlc = internal` | DLC randomized in a closed loop (its items stay there) |

**A base-game item placed in the DLC was never a logic softlock** — it is a normal edge and assumed
fill respects it. The real damage is **pacing**: an early-needed item behind end-game-scaled content
makes a world that is completable and unplayable. Fix = a **sphere-depth constraint at fill** ("an
item of sphere ≤ N may not land in a zone of sphere ≥ M"), not a solver change. Two problems, two
mechanisms.

**DLC-specific trap — a parallel power system.** Scadutree Fragments / Revered Spirit Ashes are
essential inside the DLC and useless outside; the two scaling systems are not interchangeable. Scatter
them into the base game and the DLC becomes brutal; the reverse starves the base game. ⇒ strong
candidates for **vanilla-locked** (or DLC-internal-only), which removes a whole family of unplayable
seeds at almost no cost to the shuffle.

---

## A THIRD PROBE — narrative milestones (2026-07-28)

Melina appearing at graces, the Guidance of Grace ray, the Roundtable invitation, Varré showing up:
all the same underlying thing — **the game's own set of story-milestone flags**, a distinguished
subset of the flag space representing macro-progression. It is the closest thing to a native
progression *order* that exists (there is no native solver — see the RE prompt below).

**Both existing probes miss this class by construction.** `_probe_flag_gates.py` keeps an event only
if it contains a *traversal action* (`Set ObjAct State`, `Change Asset Enable State`, warp family). A
Melina event contains none — it fires dialogue and a cutscene — so it is filtered at the door.

Derivation rule for the third probe, same doctrine (by NAME in the EMEDF, never `bank:id`):

> an event fired independently of a map (grace-side / `common.emevd`) that tests or sets flags without
> acting on geometry = a **milestone** candidate; the flags it tests are the milestone set.

Three uses, in decreasing value:

1. **Cross-check on the spheres — the best one.** An ordering from a **different data source** than
   the one that produced the graph. Solver says sphere 2, milestone chain says late game ⇒ signal of a
   graph error, obtained without playing. Exactly the redundancy that lets us trust the graph without
   proving it line by line.
2. **Native source for the pacing constraint** the DLC section needs — instead of inventing our own
   difficulty tiers (which would be curated data, forbidden), take the game's. A mod that re-cuts its
   progression is followed automatically.
3. **Live error detector** (validation level 2.5 below): a player reaching milestone N that the solver
   believed blocked is a free bug report.

**Caveat + direction rule:** the conditions mix real milestones with **counters** ("3 sites of grace
visited" is onboarding, not a story beat), so the derived set will be noisy exactly like the
3098 → 242 → 19 collapse. Injecting a milestone as an extra edge requirement is technically in the
*safe* direction (over-approximating requirements) but over-constrains the fill and fails rolls for
nothing. ⇒ **use as an oracle and a pacing metric, not as a logic input** — with an optional
`logic = conservative` mode for anyone who wants belt and braces.

---

## VALIDATION — "simulating a run", four tiers (2026-07-28)

| Tier | When | Cost | What it proves |
|---|---|---|---|
| **1 — sphere search** | every roll, systematic | ms | the placement respects the graph |
| **1.5 — their spoiler as a corpus** | when a reference log is at hand | minutes | our verifier does not reject a world thousands of people finished |
| **2 — in-game oracle** | once per mod / after a derivation change | hours, serialized | the graph matches the game |
| **2.5 — passive corpus** | continuously, free | ~0 | regression against real playthroughs |
| **3 — a bot that plays** | never | — | nothing useful |

Tier 1 is the standard technique of every serious randomizer (playthrough generation / spoiler
spheres) and is already specified above. Tier 3 is a non-goal: we never need to prove a boss is
*beatable*, only that **the world opens** — player skill is the class-D problem, not a logic one.

**Tier 1.5 — differential test against the reference randomizer.** Its worlds are validated by mass
play, so its spoiler log is a **known-completable placement**. Run OUR verifier over THEIR placement:
a "not beatable" verdict on a world thousands of people finished is a bug or an over-constraint in
our graph, found without playing. A "beatable" verdict proves little, so pair it with a **mutation
test** — move one key item behind its own door in that same known-good placement and require the
verifier to reject it. Two caveats, both load-bearing:

- **LEGAL:** their log is an **oracle, never a source.** Reading a locally-generated log to test our
  verifier is fine; tuning our graph until it agrees with theirs launders their curated table into
  ours through a feedback loop — forbidden by LEGAL below *and* by the mod-agnostic doctrine. A
  disagreement is a reason to go measure the game (tier 2), never to copy their verdict. Their logs
  never enter this repo.
- **COVERAGE:** it validates only what the log records — see MEASURED §1: unique drops only, not the
  ~two thirds of enemy lots a roll rewrites. And it is vanilla-keyed, so it says nothing about
  Convergence/ERR, which is exactly where the mod-agnostic claim lives.

### Tier 2 — what `set_flag` / `read_flag` must actually touch

**The trap: reading back a flag you just set is a tautology.** The oracle must come from a
**different channel** than the one written. Write into the *state* space, read from the *consequence*
space.

**Write side — three channels, not one**

| Channel | Verb | Note |
|---|---|---|
| Flags | `flag set <id> <0\|1>` | clearing works (`goblin_debug_rpc.cpp:1172`) |
| **Inventory** | `give_item`, `goods_count`, `inv_probe` | ⚠ **no remove verb** |
| Position / loaded map | `warp`, `warp_xyz`, `warp_local` | |

**⚠ The item axis is not flags.** `IF Player Has Item` and `ObjActParam.spQualifiedId` test the
**inventory**. Half the graph therefore simulates with `give_item`, not `flag set`.

**Read side — the oracle, most direct first**

| Observable | Answers | Available |
|---|---|---|
| **Grace state byte** (`registered/discovered/visible`) | "is this node a warp target" — *isomorphic* to what the fixpoint computes | ✅ `goblin_grace_suppression.cpp:52` |
| Lot present + collected flag | "is this slot live" (invariant 6) | ✅ `loot_at`, `refresh_markers`, marker layer |
| **ObjAct state** (item-gated door unlocked?) | the item axis, directly | ❌ no verb — **gap** |
| **Asset enable state** | the flag axis, directly | ⚠ `objects` / `assets_probe` / `move_read` exist — unverified |
| Position after an attempted traversal (`coords` + input) | universal; **the only oracle for the geometric class** | ✅ mechanically, but slow and fragile |

**`flag range` is the sleeper asset.** It snapshots up to 20000 flags at once ⇒ *snapshot → perform
the action → snapshot → diff* yields, empirically, **which flags an action actually sets**. That is a
direct attack on this doc's own "co-occurrence, not causation" limitation: the static scan proposes
candidates, the diff decides. Same trick in reverse for edges (set a candidate flag, re-snapshot the
grace/asset state; nothing moves ⇒ not the lock).

**Do NOT read `WorldMapPointParam.clearedEventFlagId`** — already measured as a decoy here (Godrick
510010 vs the EMEVD's 10000800; classifying on it found 21 gates instead of 175).

**Unit of test = ONE edge hypothesis with a MINIMAL state**, never "simulate sphere N". A state built
by mass flag-writing is reachable by no real run (cutscenes fired, NPCs teleported, incoherent world
mutation) and the oracle would then rule on a state that does not exist. Test = `edge E requires X` ⇒
point A: minimal state **with** X (must read open); point B: minimal state **without** X (must read
closed). B carries the value and B is what the missing item-removal verb blocks ⇒ **restart from a
fresh save** per test point rather than mutating downward. Slow, but sound — and it is a game gate,
so serialized either way.

**Tooling gap this exposes:** an **ObjAct-state read** and an **asset-enable read**. Everything else
(flag r/w, inventory write, warp, lots, graces, flag snapshots) already exists.

---

## MEASURED 2026-07-28 — the reference randomizer, on a real install

Measured on the user's own install (vanilla ER + the reference randomizer v0.11.4, 13 archived
spoiler logs and 6 hand-captured `regulation.bin` snapshots), with `tools/diff_regulation.py`
(written for this — param-level, because **byte hashes of `regulation.bin` are meaningless**: six
snapshots had six different sha256 while several were param-identical).

### 1. The spoiler log is NOT a record of the world it generated

A roll rewrites **67.3 %** of `ItemLotParam_enemy` and **75.3 %** of `ItemLotParam_map` rows (vs
vanilla). But the log reports only **unique** drops — 497 `Dropped by` lines, every one a boss, a
named NPC, or an individually-described enemy. **Generic respawning mob drop tables appear nowhere
in it.** And a re-roll overwrites `regulation.bin`, so the previous world is unrecoverable unless
someone snapshotted it first.

Two corrections to this doc follow:

- **`tools/audit_markers_vs_spoiler.py` has a structural blind spot** — it treats the spoiler as
  ground truth, so it can say nothing about two thirds of the enemy lots.
- **The "their spoiler = corpus of known-completable worlds" idea (VALIDATION, tier 1.5) is
  requalified**: an oracle for the *logged* subset only, not for the world. Still useful, not the
  broad safety net it was written as.

### 2. Determinism: CONFIRMED — and the real hazard is settings persistence, not RNG

Controlled experiment (6 snapshots, one variable each):

| transition | result |
|---|---|
| re-roll, no exe restart | **param-identical** |
| exe restart | **param-identical** |
| profile export + import round-trip | **param-identical** |
| ONE setting changed (shadow-realm blessing → anywhere) | 6 params differ: enemy **65.9 %**, map **75.4 %**, `ShopLineupParam` 51.4 %, `CharaInitParam` 6.2 %, `NpcParam` 1.2 % |
| exe restart after that | the setting **silently reverted to default**, and the world went **exactly back** (identical to the pre-change snapshot) |

So the RNG is reproducible and the `--preset Custom` note at the top of this doc, while true, is not
what bites. **What bites is a UI setting that does not persist.** One silently-reverted toggle swaps
~two thirds of the world with no user action.

**DESIGN RULE this forces on us:** the configuration that produced a permutation is written **BY
VALUE into the sidecar at roll time**, and the world is re-derived from the sidecar — never re-read
from the UI or the INI at the next launch. Same lesson as `--preset Custom`, sharper: there the user
edited something, here the tool forgot.

### 3. Invariant 3 has a demonstrated failure mode behind it

The reference tool has **no "are you sure you want to re-roll?" confirmation**, so the destructive
action sits next to the play action. Visible in the user's log folder: repeated rolls of an
*identical* seed minutes and days apart (nobody re-rolls the same seed on purpose), and a 0-byte log
one minute before a full roll.

The structural remedy is **not** a confirmation dialog — that lowers the probability and leaves the
category. It is making a re-roll *unreachable* for a save that has already been played, which is what
invariant 3 already says: the permutation lives in the save's sidecar, so the world is DERIVED from
the save and there are not two states that can desync.

### 4. ★ Invariant 6's risk surface is BOUNDED — and tiny on the enemy side

"An item on a lot whose flag is already set is invisible" applies **only to lots carrying a
`getItemFlagId`**. A repeatable probability drop has none, so it is structurally immune.

| | `ItemLotParam_enemy` | `ItemLotParam_map` |
|---|--:|--:|
| vanilla | **244 / 5135 flagged (4.8 %)** | 5047 / 5564 (90.7 %) |
| rolled | **13 / 4510 flagged (0.3 %)** | 5388 / 5905 (91.2 %) |

⇒ **the desync risk is concentrated almost entirely in WORLD PICKUPS, not enemy drops.** The solver's
"already-collected ⇒ not a valid slot" check (invariant 6) only has to run over the flagged subset;
the 95–99 % unflagged enemy lots need no save-state check at all.

Worked example that closed a live investigation: Smithing Stone [6] appears in **108** enemy lots,
**zero** flagged, 1–8 % chance each. A player who "stopped getting it" is unlucky, not desynced —
and no amount of spoiler-log reading could have shown that, because none of those lots are logged.

## NEXT — open items (2026-07-28)

- **★ RE ANSWERED (static) same day** — `docs/re/windows_emevd_condition_evaluator_re_findings.md`.
  **G1 = YES:** ONE hook at the bank dispatcher (`0x567d40`, app 2.6.2.0) yields
  `(event instance, bank, index, raw operands, result)` per executed instruction, with the condition
  group uniformly `arg[0]` ⇒ **the co-occurrence ceiling above is retirable by observation**, and
  `0x57ef10` turns out to encode the exact 4-way logical op (`AllOn/AllOff/AnyOn/AnyOff`) that
  `_probe_flag_gates.py` reimplements blind. **G2 = NO:** `Evaluate` reads the *live* world, so it
  answers "is G true now", never the counterfactual — observation, not interrogation, which is the
  "oracle, not planner" framing the prompt opened with. ⚠ **Nothing measured live yet**; the next
  action is a read-only `mem_fwa` check that the dispatcher fires in-world and stops at the main menu.
- **Measure:** the grace state byte before/after burning the Erdtree ⇒ settles monotonicity (a)/(b).
- **Measure:** the end-of-game instruction family across all maps ⇒ does the DLC add an ending; split
  the emevd counts base vs DLC.
- **Write:** the third probe (narrative milestones).
- **Specify:** counted-resource and k-of-n support in the fixed point (classes B5/B6).
- **Find:** an ObjAct-state read and an asset-enable read (tier-2 oracle gap).
- **Config:** everything above that changes a roll — goal mode, dlc mode, logic mode, sphere-depth
  constraint, assumed capabilities — must be in the exported configuration by VALUE, and written into
  the **sidecar at roll time**, never re-read from the UI/INI at the next launch (MEASURED §2: a
  silently-reverting toggle swapped two thirds of a world with no user action).
- **Scope invariant 6 to the flagged subset** (MEASURED §4): the already-collected check only has to
  run over lots carrying a `getItemFlagId` — ~91 % of map lots, but only 0.3–4.8 % of enemy lots.
- **Tooling gap found live:** no RPC exposes the LOCKED-ON target's identity (`combat` only counts
  enemies, `hp_probe` is the *player's* HP). A `target` verb — locked ChrIns → `npcParamId` → its
  `ItemLotParam_enemy` row → resolved items + flags — would answer "what does this thing drop, and is
  its flag set" directly, and is the natural in-game half of the tier-2 oracle.

---

## LEGAL

The reference randomizer is **source-available but NOT freely licensed**: no redistribution of the
program, of forks, or of its config files. Reimplementing *functionality* observed from the UI and
documentation is fine — functionality, languages and file formats are not protected by copyright
(EU: Software Directive; CJEU *SAS Institute v World Programming*, 2012). What must NOT be taken:

- their source, verbatim or transcribed;
- their **curated data** — placement/logic tables, presets, config files. This is the highest-risk
  item (effort-based compilation, and in the EU a *sui generis* database right on top of copyright),
  and it is also the thing our mod-agnostic doctrine forbids anyway: derive from the loaded game,
  never embed a table;
- their wording (option labels, tooltips, README), their screenshots, their name/branding.

Convenient alignment: the legally safest path is the technically better one, for the same reason —
no baked tables.
