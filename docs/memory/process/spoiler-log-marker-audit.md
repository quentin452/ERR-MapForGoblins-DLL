---
name: spoiler-log-marker-audit
description: "External ground-truth check: diff the markers our map draws against an Elden Ring Randomizer spoiler log (tools/audit_markers_vs_spoiler.py). Method, spoiler hygiene, the 2026-07-27 baseline, and which 'anomalies' are already ruled out."
metadata:
  node_type: memory
  type: process
---

# Auditing our markers against a randomizer spoiler log

**Why this beats every self-check we had.** Re-deriving markers from the regulation/MSB only proves our
reader agrees with itself. A randomizer **spoiler log is an INDEPENDENT statement of what the seed
placed**, produced by another program, so a diff against our live marker set can actually catch a
coverage gap, a phantom or a duplication. A boss/item-randomized install is also the ideal mod-agnostic
test bed — anything of ours that is secretly keyed on a vanilla model or an ERR id shows up immediately.

## How to run

```
python tools/mfg.py rpc vmap dump_markers C:/tmp/markers.csv     # in-game, markers built
python tools/audit_markers_vs_spoiler.py C:/tmp/markers.csv <randomizer_dir>
```

The CSV carries only `name_id`, so the tool resolves marker names OFFLINE by replaying
`goblin_messages.cpp decode_textid`'s bands against the install's own FMGs (that band table is
duplicated in the tool — **if decode_textid's bands change, update `BANDS` there too**). Spoiler-log
entries read `<Item>[ [qty]] in <Region>, <detail>: <prose>.`, and the indented sub-lines are the only
reliable class signal: `(cost: N)` = shop stock, `Drop chance for X: N%` = a drop table. With no log
path the newest one in `<randomizer_dir>/spoiler_logs` is used.

**SPOILER HYGIENE (why the tool prints only aggregates).** This is normally run FOR a player who is
mid-run and does not want to be spoiled. Every number printed is a count, a family or a category —
never an item name or a placement. Tool output is visible to the user, so keep any addition aggregate;
dump per-item detail to a FILE if you need it while debugging.

## Baseline — 2026-07-27, vanilla + Randomizer v0.11.4, seed 986754587

8558 markers over 67 categories vs 6239 parsed placements. Coverage of items the log places in the
world, split by how precise the log's location is:

| log entry | distinct items | on our map |
|---|---|---|
| world, **with a location detail** (the trusted signal) | 330 | **97.6 %** |
| world, region only (mixes in drops/rewards that are not markers by design) | 1352 | 86.2 % |
| shop stock | 423 | 39.2 % (not markers) |
| drop tables | 493 | 35.1 % (not markers) |

Read the DETAILED subset as the verdict: when the log names a precise spot, we draw it 97.6 % of the
time. Also clean: **0 markers failed projection near the origin**.

## Already ruled out — do NOT re-investigate these

Each looked like a bug and is not. Check these explanations before filing anything from a new run:

- **128 "phantom" items drawn but absent from the whole log, incl. 81 Ashes of War.** That seed has
  **0 `GemName` placements** — ashes of war were not randomized at all, so the log never mentions them
  and our markers are the correct vanilla placements. **Rule: an item family with 0 log placements is
  not randomized in this seed; its phantoms are correct.** The tool prints the per-family placement
  counts precisely so this is checkable at a glance.
- **1197 `name_id`s resolving to no text, all in `WorldBosses`.** That is the synthetic per-type id
  `0x60000000` (= 1610612736, inside the NpcName hi band) that boss markers carry because their name
  travels out-of-band in `Marker::live_name`. By design — see [[mod-agnostic-boss-markers]].
- **52 `WorldElevator` markers with no name**: `name_id = 0` deliberately (category icon only).
- **29 items drawn far more often than placed**, all in harvestable categories
  (`WorldFarmableCollectible`, gloveworts, crafting materials). The log lists the LOT once; the world
  holds hundreds of nodes of it. Expected.

## Real leads found (open)

- **7 `LootCraftingMaterials` markers** carry a GoodsName-band `name_id` that resolves to no text — the
  only case where our lot→item chain yields a dead id. Smallest, sharpest lead.
- **17 `WorldInteractables` markers** point at ActionButtonText ids that do not exist off-ERR (e.g.
  7041, 9520) → anonymous interactables on vanilla/randomizer. Same class as the known ERR-only text
  debt; mod-agnostic prime directive.
- **19 markers out of bounds** (|world coord| > 40000).
- **194 world-placed items never drawn** (8 of which are also listed as shop/drop elsewhere, so they
  are likely misclassified). Treat as SOFT: the comparison is by item NAME, and the residue sits almost
  entirely in the region-only entries — the weakest evidence class. Needs a per-item drill-down (to a
  file) before it means anything.

## Gotchas that already cost a run

- **SoulsFormats keeps a mapped section open on the temp file it reads**, so every read needs a FRESH
  temp path. `extract_all_items._read_from_bytes` reuses ONE path: calling it twice in a process makes
  the SECOND load fail silently and return partial data (it produced a bogus "0 % coverage" pass here).
  The tool has its own `_read` with a counter.
- **Some real item names END in a bracketed number** ("Golden Rune [7]"). Stripping a trailing `[N]` as
  a quantity before matching loses them — and with them ~600 of our markers. Match the RAW left-hand
  side first, strip only as a fallback.
- The randomizer's **"Basic placements" section is ENEMIES, not items** (its trailing `(scaling A->B)`
  gives it away). Items live under `-- Spoilers:`.

Related: [[nobake-coverage-scoreboard]] (our own provenance tracking), [[disk-parser-coverage-gaps]],
[[mod-agnostic-boss-markers]].
