# Shadow / sidecar save — plan

Status: **SCOPED + PHASED 2026-07-03 (user chose "sidecar first").** Approach chosen (transient-grant
via inventory API + save-detection, NOT serializer parsing) + RE targets nailed — see "Implementation
phasing" below. **Phase 1 (ini state-store, no inventory strip) = the buildable start; Phase 2
(inventory strip-and-reinject) = the hard RE.** Phase 1 uses mINI (flat state); JSON is only needed for
Gap-C custom-item records (Phase 2 territory). Gap H frozen; param-override loader needs NONE of this.
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
3. **Binding key (anti-desync).** A `<save>.mfg` MUST pair unambiguously with its save file and survive
   the user copying / backing up / cloud-syncing saves. Candidates: steam id + character slot index + a
   GUID we stamp (into an unused save field, or a vanilla-range flag pattern). Define the key + the
   "sidecar doesn't match this save" fallback (ignore + warn, never mis-apply another save's items).
   **SAVE-PATH IS DYNAMIC — do NOT hardcode `.sl2`.** ERR writes **`ER0000.err`** (ModEngine3 `savefile`
   redirect in the `.me3` profile), NOT `.sl2` — but it is the SAME format (BND4/sl2 layout, see
   [[../memory/tooling/err-save-file-format]]). Vanilla = `.sl2`, co-op (ERSC) may differ again. Resolve
   the ACTIVE save file the game opened (hook the save open / read the ME3 redirect) and place `<save>.mfg`
   next to it; key off the real file, whatever its extension. Because the serializer work is IN-MEMORY,
   the redirect doesn't touch the strip/reinject RE — only where the sidecar lands.

   **ERR nuance (relaxes variant choice for ERR):** an ERR `.err` save is ALREADY mod-locked (needs ERR's
   regulation; vanilla can't load it), so the sidecar's "keep the save vanilla-clean / loads DLL-less"
   payoff matters MOST for vanilla / value-only-mod installs. For ERR specifically, variant B
   (reserved-id + DLL-required) is more tolerable — the only orphan case is dropping just MapForGoblins
   from an ERR profile, a smaller blast radius than dirtying a precious vanilla `.sl2`.
4. **Atomicity / crash safety.** Write the sidecar in lockstep with the game's save event; tolerate a
   crash between the `.sl2` write and the `.mfg` write (temp-file + rename, a generation counter, or a
   pending-journal). Decide the recovery rule when they disagree.
5. **Multi-slot / new-game / delete.** One `.sl2` holds multiple character slots; handle per-slot state,
   new-character (empty sidecar), and character-delete (drop the sidecar slice).

## Implementation phasing + RE targets (2026-07-03 — user chose "sidecar first")

**Approach chosen: transient-grant via the inventory API + save-DETECTION, NOT raw serializer
parsing.** Rationale: the game serializes whatever is in the live inventory, so instead of RE'ing the
`.sl2`/`.err` inventory-serialize function + its 11 checksums (huge, format-fragile), keep custom items
OUT of the live inventory across a save: re-grant on load, strip right before the game writes the save,
re-grant after. The strip/reinject uses the inventory ADD/REMOVE functions (game APIs) — far more
tractable than buffer surgery.

**RE targets (state of each):**
- **Inventory accessor — PARTLY KNOWN.** `WorldChrMan=[er+0x3D65F88]`, `LocalPlayer=[WCM+0x1E508]`
  (`goblin_world_position.cpp:463`). The inventory (EquipInventoryData / the MapItemMan the grant `inv`
  arg points at) hangs off LocalPlayer or a sibling global — finish this chain first (shared with the
  Gap C grant).
- **AddItem — AOB KNOWN** (`sig::ADD_ITEM_FUNC`, observer-hooked read-only, `goblin_debug_events.cpp:344`;
  convention: `AddItemFn(inv, entry{qty@0,id@4}, base, count, pad)`). **RemoveItem — NEW RE** (the
  consume/discard counterpart).
- **Save-event DETECTION — TRACTABLE via existing infra.** The mod already hooks `CreateFileW`
  (`diag_boot_io` / [BOOTIO]). The game opens/writes `ER0000.err` (ERR) / `ER0000.sl2` (vanilla) on
  save — watch that path to fire a "save starting/ending" signal WITHOUT RE'ing the save function.
  (A cleaner hook on the actual RequestSaveData fn is a later refinement.)
- **Load-complete DETECTION — ALREADY HAVE signals** (world-entered: player pos resolves, collected
  refresh runs). Re-grant on first in-world frame after a load.
- **Character identity (binding key) — NEW small RE.** SteamID + character slot from the loaded save
  state in memory (or stamp our own GUID into a reserved event-flag pattern).

### Phase 1 — sidecar STATE STORE (no inventory strip; buildable without the hard save-hook)
The `<save>.mfg` file + binding + JSON, storing framework state the DLL FULLY OWNS: custom event flags
we set (`SetEventFlag`), per-save mod config, progress counters. Persist on our own schedule (own-state
change + world-exit) + on the CreateFileW save signal; load + re-apply on world-enter. Needs: save-PATH
resolution (`.err`/`.sl2`, dynamic), character-identity read (binding), a JSON parser (shared with Gap C
— add here per [[../memory/process/authoring-format-decision]]). **Useful immediately** (persist custom
flags/progress across sessions), and it's the skeleton Phase 2 rides on. Does NOT touch the inventory →
no strip, no RemoveItem, low risk.

### Phase 2 — inventory ITEMS via strip-and-reinject (the hard part)
On the CreateFileW save signal: for each sidecar item, `RemoveItem` from the live inventory (record
qty), let the game write the (now-clean) save, then `AddItem` back. On load: `AddItem` each from the
sidecar. Needs: the finished inv accessor + AddItem/RemoveItem + robust save-window timing (catch EVERY
save incl. autosave; strip before the write, restore after the close). This is what makes granted
custom items save-clean. Prototype on the ONE Gap-C item (goods 90000001).

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
