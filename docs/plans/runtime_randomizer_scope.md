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
