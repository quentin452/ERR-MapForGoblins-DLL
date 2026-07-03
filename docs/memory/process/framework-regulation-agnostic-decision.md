---
name: framework-regulation-agnostic-decision
description: "LOCKED 2026-07-03 — the runtime-modding framework is regulation-agnostic BY CONSTRUCTION; vanilla-only was rejected. Running on a regulation.bin mod is the default (ERR is one), not a feature. The only write-path danger is ID collision on PERSISTED writes, governed entirely by Gap H."
metadata:
  node_type: memory
  type: project
---

**Locked decision (2026-07-03, user).** Settles "should the framework support mods that modify
`regulation.bin`, or focus on vanilla, or both?" → **BOTH — and it is nearly free, because the
architecture is regulation-agnostic by construction.** Don't re-litigate. See
[[reserved-id-and-load-contract]] (Gap H), `docs/runtime_modding_framework_vision.md`,
`docs/runtime_live_capabilities_audit.md`.

## The key distinction (do NOT conflate these two)

1. **RUN on a regulation-modding mod** → already the DEFAULT, not a feature. ERR / Convergence / ERTE
   ARE regulation.bin mods; **we develop and test on ERR.** The prime directive (read the ACTIVE
   install's live params/FMG, graft edits in RAM AFTER the game loads its own regulation) means the
   framework reads whatever the mod loaded — it never ships a vanilla snapshot forced on top. So
   "support regulation mods" = the graft-in-RAM design already gives it for free.
2. **WRITE safely into that mod's world** → the ONLY place danger lives, and only on the PERSISTED-write
   path (Gap C item grants / custom flags). Everything SHIPPED (`param_set_field`,
   `param_overrides.ini`, `inject_fmg_entries`) edits LIVE tables the mod already populated and
   persists nothing → zero problem on any regulation, today.

## Why vanilla-only was REJECTED

It would (a) betray the prime directive, and (b) BREAK on our own dev install (ERR, a regulation mod).
No upside. A modified regulation is simply "the active install" — the only thing the framework ever
reads.

## The write-path dangers to weigh at Gap C (nothing shipped is at risk)

1. **ID collision (main).** A regulation mod adds thousands of rows across many id ranges → a reserved
   band may overlap → **Gap H collision-check-at-inject** solves it (read live table, never claim an
   occupied id). Frozen.
2. **Paramdef divergence — largely SELF-HEALING.** Offsets come from `resolve_field_offset` (reads the
   game's OWN access instruction), so when a mod's paramdef shifts a field, the game's code shifts with
   it and the AOB still lands right. Only a mod that REMOVES/renames a targeted field breaks — and then
   we skip it (field-not-found), never corrupt.
3. **Behavioral assumptions.** Never bake vanilla semantics ("goodsType 8 = crafting"); read the mod's
   own param VALUES (sortGroupId, etc.). Already the approach.
4. **Save coherence across mod swaps.** Granted item on a Convergence save, then the user drops the mod
   but keeps our DLL (or vice versa) → orphan. Our part = Gap H DLL-at-load + the sidecar; the mod's own
   save-compat is not our problem.
5. **Two DLLs grafting the same row** (us + another RAM-patcher) → last-writer-wins race. Keep our edits
   idempotent + re-applied; don't assume exclusivity.

## Standing acceptance test

Every write: *"does this produce a correct result on a DIFFERENT regulation than ERR?"* Yes →
mod-agnostic. Works only because ERR's values happen to match → NOT done. The danger is not
compatibility (solved by design) — it is ID collision on persisted writes (governed by Gap H).
