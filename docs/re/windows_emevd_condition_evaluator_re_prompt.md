# RE prompt — EMEVD condition-group evaluator (native logic oracle)

**Status: WAITING on RE.** Opened 2026-07-28 for the runtime randomizer
(`docs/plans/runtime_randomizer_scope.md`).

## Why this matters (the whole point, in one paragraph)

The randomizer's progression graph is derived by STATIC scan of the active install's own EMEVD
(`tools/_probe_progression_gates.py`, `tools/_probe_flag_gates.py`). Both scans **reimplement the
engine's condition semantics**, and both stop at *co-occurrence* rather than causation — the scope
doc states the limitation itself ("a flag counts as a gate if it is tested somewhere in the same
event … 2089 is a ceiling"). Every wrong edge in that graph is a candidate **softlock**, and a
softlock is the one failure this feature cannot ship with.

If we can OBSERVE (ideally, later, CALL) the game's own condition evaluator, we stop reimplementing
the semantics and start measuring them. That single capability retires the largest open risk in the
plan, and it does so **mod-agnostically** — it works on whatever EMEVD the loaded mod ships.

## Explicitly NOT the goal — do not hunt for a "solver"

There is no native reachability/progression solver in `eldenring.exe`, and looking for one is a
waste of a session. A game engine evaluates conditions **locally and reactively** ("is this flag
set? does the player hold this item?"); it never plans, never searches forward, and has no notion of
"reachable from the start". Reachability is a randomizer-author's question, not a runtime's. The
target below is an **oracle**, not a planner.

## What we already own — start here, do NOT re-derive

| Asset | Where |
|---|---|
| `IsEventFlag(EventFlagMan*, u32* id) -> bool` AOB | `src/re_signatures.hpp:25` (`IS_EVENT_FLAG`) |
| EventFlagMan singleton slot (2 resolves) | `re_signatures.hpp:28` / `:41` |
| `SetEventFlag` entry + its **decoy sibling** warning | `re_signatures.hpp:30-39` — read the comment before trusting any match |
| Live flag read wrapper | `goblin::ui::read_event_flag` → `orp_flag_set`, `src/goblin_inject.cpp:334` |
| ESD/EzState VM decode (a DIFFERENT vm — style reference only, not answers) | `docs/re/esd_ezstate_decoder_re_findings.md` |
| AOB-first doctrine, no fixed RVAs | `docs/re/rva_aob_hardening_backlog.md` |

Live RPC verbs already available to drive this: `flag` (read / `set <id> <0|1>` / `range <lo> <hi>`,
≤20000 ids), `mem_fwa` (find-what-accesses), `mem_scan_u32`, `mem_dump`, `mem_write`, `give_item`,
`goods_count`, `inv_probe`, `warp` / `warp_xyz` / `warp_local`, `loot_at`, `param_get` / `param_set`
/ `param_rows`, `er_base`, `er_version`, `status`, `mfg_build`.

## Targets, in priority order

- **T1 — the EMEVD instruction dispatcher / VM step function.** The loop that walks an event's
  instruction list and calls a handler per opcode. Finding it locates everything else.
- **T2 — the condition-group evaluator + the per-event-instance condition-group state.** This is the
  prize: it is the thing our static scan is *guessing at*. We want the wiring (which instruction
  feeds which group, which group gates the action), read rather than inferred.
- **T3 — the `IF Player Has Item` handler.** The item axis (the 19 `ObjActParam.spQualified*` locks
  plus the key-item tests). Gives exact semantics including quantity cases a static scan cannot see.

## Route A — live, no Ghidra required (preferred if the game is running)

1. **Freshness gate first** (`docs/memory/tooling/mfg-rpc-driver-hardening.md`): `mfg.py rpc
   mfg_build` — `ping` answers from a stale DLL too. Confirm **in-world**, not main menu, via
   `status`. Record `er_version` + `er_base`.
2. `mem_fwa` on the EventFlagMan bitmap (or a breakpoint at the `IS_EVENT_FLAG` entry) → collect
   **distinct caller RIPs** over ~60 s of ordinary play, plus a burst while crossing a scripted
   trigger (rest at a grace, open a door, enter a boss fog).
3. **Classify the callers.** Our own overlay reads flags per marker — expect our RIPs and menu code
   in the set, and discard them. The EMEVD VM has a distinctive signature: it reads **many different
   ids in a tight burst, every frame or every tick**, from one call site.
4. From that call site, walk **up one frame** (return address → the dispatcher). `mem_dump` the
   surrounding bytes and derive an AOB candidate in the house format.
5. Confirm the dispatcher by a second, independent behaviour: it should also be the caller of the
   item-possession test (T3) — arm `mem_fwa` on the inventory count path and check the caller set
   intersects.

**Route A deliverable:** a stable AOB for the flag-test instruction handler AND its calling
dispatcher, verified across at least one game restart (address changes, AOB must not).

## Route B — static (Ghidra), if a project for this exe exists on the box

1. Locate `IsEventFlag` by the `IS_EVENT_FLAG` pattern above (do not trust a name).
2. **Xrefs → the handler.** The interesting caller is the one reached from a big dispatch
   (switch / jump table on an opcode), not from UI code.
3. From the handler, walk up to the dispatcher; a jump table keyed on the EMEVD instruction
   `(bank, id)` pair is the confirmation.
4. From the dispatcher's `this`, map the **event-instance struct**: instruction pointer/cursor, and
   the **condition-group array** (the per-group evaluated state). That array is T2.

## Go / No-Go — answer these two explicitly, they are the point of the session

- **G1 (win condition): can we HOOK the evaluator and passively log
  `(event id, condition group, instruction, operands, result)` while a human plays normally?**
  If yes, the "co-occurrence ≠ causation" limitation is retired by observation, and the randomizer's
  level-2.5 passive corpus becomes semantically complete for free. **This is what success means.**
- **G2 (secondary, may well be NO): can the evaluator be CALLED on a synthesized state** ("in this
  state, is group G true?") without a live event instance? Expect this to be unsafe or meaningless
  out of context. **Say so plainly if it is — a documented NO is a good result.** Do not force it,
  do not crash the game chasing it.

## Guardrails

- **Read-only first.** No new writes into game memory beyond what existing verbs already do. G2 is
  the only write-adjacent item and it is explicitly allowed to end in "no".
- **No fabricated addresses.** If the game is not running and no Ghidra project exists on this box,
  do the part that IS possible (static reading of our own sources, the plan of attack, what tooling
  is missing) and **say clearly what was not measured**. A short honest finding beats a long guess.
- **AOB-first, never a fixed RVA** — the whole `rva_aob_hardening_backlog.md` exists because fixed
  RVAs rotted. Every address in the findings must carry the AOB that resolves it and the
  `er_version` it was seen on.
- **Single-instance discipline:** if this drives the game, it is a game gate — nothing else may
  drive the game concurrently.

## Deliverable

`docs/re/windows_emevd_condition_evaluator_re_findings.md`, house style: what was measured, on what
version, with which AOBs, the G1/G2 verdicts, and the next concrete step. If AOBs are found and
verified, propose the `re_signatures.hpp` block (do not wire it into the build in this pass).
