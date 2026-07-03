# Shadow / sidecar save — plan

Status: **SCOPED 2026-07-03, DEFERRED — do NOT start yet.** This is the save-persistence mechanism for
FUTURE content modding (custom items / flags / progress). It is gated on **Gap H** (reserved-ID +
DLL-at-load policy) and only becomes real work when **Gap C** (item grants) starts. Kept here so the
design survives; the shipped param-override loader (`param_override_loader_plan.md`) needs NONE of this.
Origin: user idea + `docs/runtime_modding_framework_vision.md` ("Save persistence — the shadow/sidecar
save"). Prereqs in `docs/runtime_live_capabilities_audit.md` (Gap C gated on Gap H).

## Why (the problem this exists to solve)

ELDEN RING's `.sl2` has no legal home for framework-specific state. Three things need out-of-schema
persistence once the framework grants content:
- **Custom items** — inventory IDs the vanilla `.sl2` doesn't recognize (orphan / corruption if written).
- **Custom event flags** — beyond the vanilla flag block.
- **Framework progress** — mod quest state, counters, per-save config.

Param FIELD edits (the shipped loader) do NOT belong here — they reload from `regulation.bin` each boot
and never persist. The sidecar is ONLY for granted/persisted state.

## The idea: a DLL-owned `<save>.mfg` next to the `.sl2`

The framework keeps a small sidecar file as the SOURCE OF TRUTH for its state, synced to the game's own
save/load lifecycle. Two implementation variants:

### Variant A — strip-and-reinject (PREFERRED; keeps `.sl2` vanilla-clean)
- **On SAVE:** hook inventory + flag serialization, strip our custom entries out of what the game
  writes → the `.sl2` never contains a custom ID. Persist the stripped state to `<save>.mfg` instead.
- **On LOAD:** read `<save>.mfg`, re-grant the items / re-set the flags into the live session.
- **Payoff:** the `.sl2` stays a legal VANILLA save. This **downgrades the "DLL-must-be-present-at-load"
  hard contract to SOFT** — loading without the framework just omits the modded items that session (no
  orphan IDs, no corruption). Uninstall = delete the folder + `.mfg`; the save still loads clean. This
  is the "one folder, zero game files touched, clean uninstall" vision fully realized.

### Variant B — reserved-range + tolerate (simpler, dirtier; stopgap)
- Let custom items sit in the `.sl2` in a reserved high ID range; accept orphans if loaded DLL-less.
- No serializer RE needed, but violates "clean save". Only a fallback if A's RE proves too costly.

## The hard parts (RE unknowns — scope these before committing to A)

1. **Save/load serializer RE.** Where the game serializes the INVENTORY block and the EVENT-FLAG block
   into the `.sl2` (and the inverse on load). Needed to strip-on-write / reinject-on-read for Variant A.
   Unknown surface today — this is the gating RE. (Runtime RE is proven on Linux/Proton via in-DLL
   probes — `docs/memory/tooling/linux-runtime-re-options.md`; ERSC already hooks save-related paths,
   `docs/ersc_hosting_and_map_autohide.md` — a possible reference/reuse point.)
2. **Save/load lifecycle hooks.** The events to fire strip (pre-write) and reinject (post-load). Must be
   the RIGHT boundary so we neither miss a save nor corrupt a partial one.
3. **Binding key (anti-desync).** A `<save>.mfg` MUST pair unambiguously with its `.sl2` and survive the
   user copying / backing up / cloud-syncing saves. Candidates: steam id + character slot index + a
   GUID we stamp (into an unused `.sl2` field, or a vanilla-range flag pattern). Define the key + the
   "sidecar doesn't match this save" fallback (ignore + warn, never mis-apply another save's items).
4. **Atomicity / crash safety.** Write the sidecar in lockstep with the game's save event; tolerate a
   crash between the `.sl2` write and the `.mfg` write (temp-file + rename, a generation counter, or a
   pending-journal). Decide the recovery rule when they disagree.
5. **Multi-slot / new-game / delete.** One `.sl2` holds multiple character slots; handle per-slot state,
   new-character (empty sidecar), and character-delete (drop the sidecar slice).

## Sequencing (why it's deferred)

1. **Gap H — FROZEN 2026-07-03** (`docs/memory/process/reserved-id-and-load-contract.md`): reserved-ID
   bands + collision-check + DLL-at-load contract. (The binding-KEY for the sidecar↔`.sl2` pairing is
   scoped in "The hard parts" above, not in Gap H.) The sidecar removes the need for custom IDs to
   SURVIVE in the `.sl2`, but not the need to pick them from a reserved band to avoid colliding with
   overhauls in the LIVE session. Numeric bands finalize when Gap C starts.
2. **Then Gap C (item grants)** — the sidecar is the mechanism that makes granting SAFE. Prototype on
   ONE custom item end-to-end (grant → save → reload → item still present, `.sl2` still vanilla-legal).
3. Only THEN generalize to custom flags + framework progress.

## Acceptance (mod-agnostic)

- Variant A: grant a custom item → save → the `.sl2` remains a legal vanilla save (loads DLL-less with
  the item merely absent, no corruption); reload WITH the framework restores the item from `<save>.mfg`.
- Sidecar binds to exactly one save slot; a copied/renamed save either carries its sidecar or safely
  ignores a non-matching one (never applies the wrong save's state).
- Works on ANY install (vanilla / ERR / Convergence) — the serializer + flag block are engine-level, not
  mod-specific; reserved IDs don't collide with the loaded overhaul.

## Non-goals

- NOT for param field edits (those don't persist — shipped loader already handles them).
- NOT model/animation content (the separate "hard wall", see the vision doc).
- Not a save EDITOR / external tool — this is in-process, tied to the live session.
