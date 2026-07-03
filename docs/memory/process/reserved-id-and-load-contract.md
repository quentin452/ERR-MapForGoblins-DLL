---
name: reserved-id-and-load-contract
description: "Gap H FROZEN policy (2026-07-03) — the reserved-ID bands + collision rule + DLL-required-at-load contract that ANY framework write of PERSISTED state (custom items/flags, Gap C) must obey. Principles locked; numeric bands provisional until Gap C surveys target mods."
metadata:
  node_type: memory
  type: project
---

**Gap H — FROZEN 2026-07-03 (user).** The prerequisite policy for the runtime-modding framework's
persisted-state writes. Referenced by `docs/runtime_modding_framework_vision.md`,
`docs/runtime_live_capabilities_audit.md` (Gap C gated on H), and
`docs/plans/shadow_sidecar_save_plan.md`. **Principles are LOCKED; the exact numeric bands are
PROVISIONAL** — finalize them when Gap C (item grants) actually starts, after a live survey of the
target overhauls (ERR / Convergence / ERTE) so the chosen bands are collision-free in practice.

## What this gates (and does NOT)

- **GATES (must obey this before shipping):** anything that writes state that PERSISTS or that the
  game treats as a real content id — **Gap C (item grants → `.sl2` inventory)**, custom **event-flag
  writes** that persist, custom **param ROWS** the game/save references by id (Gap B when used for
  grants).
- **Does NOT gate (already save-safe, no reserved id needed):** **param FIELD edits**
  (`param_set_field`, the `param_overrides.ini` loader) and **FMG injection** (`inject_fmg_entries`) —
  both operate on tables that RELOAD from `regulation.bin` / the msgbnd each boot, persist nothing to
  the `.sl2`, and are already shipped. They need none of this policy.

## Principle 1 — Reserved high-ID bands (LOCKED principle)

Framework-owned ids live in a **high, configurable band per id-family**, chosen to sit ABOVE vanilla
and outside the ranges surveyed overhauls use. Rationale (from the vision doc): compat ≠ coherence —
we only need to avoid a COLLISION with another mod's row, not logically integrate.

- **Per-family, id-structure-aware.** ER item ids are not arbitrary — the game derives
  category/behaviour from id structure (weapon-id digits encode type, etc.). A custom row must live in
  a band its own param family/paramdef accepts, NOT a blind global constant. So reserve a band PER
  param (custom GoodsName rows, custom EquipParamWeapon rows, …), not one global number.
- **Configurable base (ini).** Every reserved band's base is an ini key (default below) so a user on a
  band-conflicting overhaul can shift it without a rebuild — mod-agnostic escape hatch.
- **PROVISIONAL default bands (finalize at Gap C):** reserve a contiguous high slice per family, e.g.
  custom goods `20,000,000+`, custom weapons at the top of the weapon category space, framework event
  flags in a reserved high flag band. NUMBERS TBD — pick them from a live `[SHOPDIAG]`/param-scan
  survey of the loaded regulation so they're empty on the actual target installs. Existing precedents
  to stay clear of: FMG textId encoding (`+500M` goods / `+100M` weapon in `goblin_loot_resolve.cpp`),
  the NpcName boss band `9e8` (`goblin_enemy_names.cpp:kBossBandBase`), and the injected cluster/section
  rows that live in the param table's reserved SLACK pool (`goblin_section_visibility.cpp`).

## Principle 2 — Collision-check at inject, NEVER silent-overwrite (LOCKED)

The band choice is best-effort; the HARD guarantee comes from checking the LIVE table at inject time:

- Before claiming an id (row or flag), read the live param/flag state (`get_param::try_get`,
  `IsEventFlag`). **If it is already occupied by the loaded regulation/save, do NOT claim it** — pick
  the next free id in the band, or abort that entry + `[RESERVE]` warn. This makes the framework
  mod-agnostic-safe regardless of whether the band guess was perfect.
- The ONE allowed overwrite is a DELIBERATE field OVERRIDE — that is the `param_overrides.ini`
  loader's explicit, opt-in, non-persistent behaviour (Principle-0 tables), not a persisted claim.

## Principle 3 — DLL-required-at-load contract (LOCKED)

- **Default = HARD.** Any custom id written into the `.sl2` (a granted item, a persisted custom flag)
  requires the DLL to be present at the NEXT load: without it the id is an orphan (item vanishes or
  the game chokes). Until the sidecar exists, shipping a grant means "DLL must stay installed" is a
  hard contract the user is told about.
- **The sidecar DOWNGRADES it to SOFT.** `docs/plans/shadow_sidecar_save_plan.md` variant A
  (strip-and-reinject) keeps the `.sl2` VANILLA-CLEAN — custom ids never persist there; they live in
  `<save>.mfg` and are re-granted on load. Then loading without the DLL merely omits the modded items
  that session (no orphan, no corruption), and uninstall = delete folder + `.mfg`. **This is the
  intended end state**; the reserved band still matters (to avoid colliding in the LIVE session), but
  the ids no longer need to SURVIVE in the `.sl2`.

## Order of operations (frozen)

1. This policy (done — principles locked).
2. Gap C starts → **finalize the numeric bands** from a live survey of the target regulation(s), wire
   the ini bases + the collision-check helper, prototype ONE granted item under the HARD contract.
3. Sidecar (shadow-save plan) → downgrade the contract to SOFT, ship grants cleanly.

Discipline unchanged: mod-agnostic acceptance test on every write ("correct on a DIFFERENT mod?");
never silently overwrite; log every reserved claim under `[RESERVE]`.
