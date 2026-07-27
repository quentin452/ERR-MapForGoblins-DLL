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

- **Enemy randomization** — enemy placement lives in the MSB, which `docs/re/README.md` classifies as
  a WALL (no MSB write). Not a scoping choice, a capability limit. An item-only randomizer, stated
  as such.
- **Shop lineups** — possible later (`ShopLineupParam` is a param) but not v1.
- **Starting loadouts, character edits, enemy stat scaling** — param-driveable, but they are balance
  mods bolted onto a randomizer, not the randomizer.
- **Their presets/config formats** — deliberately NOT reimplemented (see LEGAL).

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

The genuinely missing piece is **the reachability solver**. Everything else is plumbing we have.

---

## UNDECIDED — module of MapForGoblins, or separate mod?

Not decided. The trade-off as it stands:

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

**Do not start implementing before this is settled**, because it decides where the solver lives.

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
