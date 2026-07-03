# Shadow / sidecar save — plan

Status: **PHASE 1 SLICE 1 DONE + IN-GAME VERIFIED 2026-07-03 (ERR/Proton, full RPC round-trip PASS).**
Approach chosen (transient-grant via inventory API + save-detection, NOT serializer parsing) + RE targets
nailed — see "Implementation phasing" below. **Phase 1 (ini state-store, no inventory strip) = the
buildable start — slice 1 (core module + save-path detection + RPC) landed; Phase 2 (inventory
strip-and-reinject) = the hard RE.** Phase 1 uses mINI (flat state); JSON is only needed for
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
- **Inventory accessor — CAPTURE BOOTSTRAP DONE (2026-07-03), in-game capture pending.**
  `WorldChrMan=[er+0x3D65F88]`, `LocalPlayer=[WCM+0x1E508]` (`goblin_world_position.cpp:463`). Rather than
  guess an AOB for the MapItemMan global, the shipped AddItemFunc observer now CAPTURES the live `inv`
  (rcx) the game passes on any grant → `goblin::debug_events::last_inventory_accessor()` (MapItemMan is a
  session singleton, so the captured pointer is reusable to CALL AddItemFunc ourselves). On first capture
  it logs `[INVACCESS]` — inv vs LocalPlayer/WCM + a `safe_copy` scan of both objects (0..0x2000) for a
  slot HOLDING inv, to promote the pointer to a STATIC path (LocalPlayer+off or a two-hop WCM global).
  Raw getters `goblin::get_{world_chr_man,local_player}_ptr()`; RPC `inv_probe` reports it live.
  **IN-GAME CAPTURE DONE 2026-07-03 (ERR/Proton, user picked up an item):** captured
  `inv=0x25b1ea00 LocalPlayer=0x2f2bc080 WCM=0x2ee50080` (`logs/MapForGoblins_events.log`). **NO offset
  hit** (no `LocalPlayer/WCM +0xNNN -> inv` line) → `inv` is a SEPARATE CS singleton (MapItemMan), ~159 MB
  below LocalPlayer, NOT a member off the player chain within 0x2000. Conclusion: **reuse the captured
  singleton pointer** for the grant (session-stable) — a MapItemMan static AOB is optional polish, no
  short static path exists. **BONUS — goods id encoding CONFIRMED** from the same grant line
  (`item grant: entry+0=0x00000001 entry+4=0x40003bec`): `entry+4 = 0x40000000 | goodsId`, qty in
  `entry+0`. So the Gap C grant is RE-complete: `inv`=captured singleton, entry=`{qty@0,
  0x40000000|goodsId @4}`, call AddItemFunc. Shared with the Gap C grant; grant still gated on the
  sidecar (save-clean). **RemoveItem (Phase 2 strip) is now the only open inv RE.**
- **AddItem — CALLABLE + convention NAILED (2026-07-03).** From the Hexinton CT ItemGib:
  `AddItemFunc(rcx=inv, rdx=&entry{qty:i32@0,id:u32@4}, r8=scratch buffer, r9=0)` — `r9`/`count` is
  ALWAYS 0 (the quantity is the entry's `qty@0`, which explains the observer's `count=0`). Built
  `goblin::inventory::give_item(id, qty)` (`goblin_inventory.{hpp,cpp}`), inv resolved from the
  **`INVENTORY_ACCESSOR` static AOB** (also from the CT — no captured pointer needed; `[SIG] PASS`).
  RPC `give_item <id> <qty>`. IN-GAME VERIFIED: `give_item(0x40003bec,+5)` on a char at the item cap
  triggered the game's own **"Unable to acquire … exceed the maximum able to be held"** dialog → the
  real acquisition path ran.
- **RemoveItem — RE DONE 2026-07-03: ER has NO dedicated RemoveItem** (the comprehensive Hexinton CT
  exposes none). **Removal = AddItemFunc with a NEGATIVE quantity** (`give_item(id, -N)`). VERIFIED
  in-game: at the item cap, `+5` was totally rejected ("unable to acquire every"); after `-5` the count
  dropped below cap (acquisition toast `×994`) and a re-`+5` PARTIALLY succeeded ("some items could not
  be acquired") — proving the `-5` decremented the live stock. SEH-guarded; negative calls don't crash.
  So the Phase-2 strip = `give_item(id, -qty)`, reinject = `give_item(id, +qty)`. **The last inventory
  RE unknown is closed.**
- **Save-event DETECTION — TRACTABLE via existing infra.** The mod already hooks `CreateFileW`
  (`diag_boot_io` / [BOOTIO]). The game opens/writes `ER0000.err` (ERR) / `ER0000.sl2` (vanilla) on
  save — watch that path to fire a "save starting/ending" signal WITHOUT RE'ing the save function.
  (A cleaner hook on the actual RequestSaveData fn is a later refinement.)
- **Load-complete DETECTION — ALREADY HAVE signals** (world-entered: player pos resolves, collected
  refresh runs). Re-grant on first in-world frame after a load.
- **Character identity (binding key) — NEW small RE.** SteamID + character slot from the loaded save
  state in memory (or stamp our own GUID into a reserved event-flag pattern).

### Phase 1 — sidecar STATE STORE (no inventory strip; buildable without the hard save-hook)
The `<save>.mfg` file + binding + flat state, storing framework state the DLL FULLY OWNS: custom event
flags we set (`SetEventFlag`), per-save mod config, progress counters. Persist on our own schedule
(own-state change + world-exit) + on the CreateFileW save signal; load + re-apply on world-enter. Needs:
save-PATH resolution (`.err`/`.sl2`, dynamic), character-identity read (binding), a parser (mINI flat —
JSON deferred to Phase 2 / Gap C records). **Useful immediately** (persist custom flags/progress across
sessions), and it's the skeleton Phase 2 rides on. Does NOT touch the inventory → no strip, no RemoveItem,
low risk.

**Slice 1 DONE + IN-GAME VERIFIED 2026-07-03 (ERR/Proton, automated foreground RPC round-trip).**
PASS: loaded a save (nav below) → sidecar bound `path=…\ER0000.mfg loaded=1` (dynamic path resolved from
the opened `ER0000.err`); `setkv foo=bar` + `addflag 90000001` + `save` wrote a 116-byte `ER0000.mfg`
with correct `[meta]`/`[flags] custom`/`[kv]`; then `setkv foo=ZZZ` (dirty memory) → `load` → `getkv
foo=bar` (the DISK value, proving the read path) + `flags: 90000001` reloaded. Atomic temp+rename worked.
**KEY: the save file is NOT opened at the TITLE screen — `sidecar status` = `path=(none)` there;** it
opens only on actual character load, so binding requires being in-world. Load nav that worked from a
cold boot (undocumented before): RPC `key Return` (dismiss splash) → `key e` (confirm Continue) →
`key Return` → in-world in ~4s. New `src/goblin_sidecar.{hpp,cpp}` (`goblin::sidecar`):
`src/goblin_sidecar.{hpp,cpp}` (`goblin::sidecar`):
- **Save-path resolution — DONE, dynamic.** Wired into the existing CreateFileW hook
  (`worldmap/loot_open_probe.cpp` `hk_create_file_w`): on a successful open of an ER save
  (`ER*.sl2`/`ER*.err`, extension+basename filter) it binds `<save>.mfg` (extension swapped) and
  loads it; a `GENERIC_WRITE` open (= a save in progress) triggers a sidecar save alongside. The hook
  now also arms when `sidecar_save` is on. NB save path is resolved from the file the GAME opens, so the
  ME3 `.err` redirect is handled for free — never hardcoded.
- **State store — DONE.** mINI-backed `<save>.mfg`: `[meta]` (version/guid/savefile), `[flags] custom`
  (CSV of custom event-flag ids), `[kv]` (free-form progress/config). Atomic write (temp + rename).
  Thread-safe (internal mutex — the CreateFileW hook is multi-threaded). API: add/remove/has_custom_flag,
  custom_flags, set_kv/get_kv, load/save, status_line.
- **Config** `[Sidecar] sidecar_save` bool, default OFF (opt-in; every entry point no-ops when off).
- **RPC** `sidecar status|setkv|getkv|addflag|rmflag|flags|save|load` (`tools/mfg_rpc.py` reachable) to
  drive + verify a disk round-trip in-game.
- **DEFERRED to later Phase-1 slices:** (1c) character-identity binding — v1 binds by the `.mfg` living
  next to the save (copy the save without it = no sidecar, safe; a stale `.mfg` left by a
  deleted+recreated save with the same filename is the known gap the identity RE closes; guid is stamped
  now for that cross-check). Multi-slot per-character state (one `.err` holds all slots) — v1 stores
  per-save-file (global); add a slot dimension later.

**Slice 1b DONE + IN-GAME VERIFIED 2026-07-03 (auto lifecycle — replay on load + autosave on exit).**
`sidecar::tick(world_loaded)` (poll thread, `dllmain` loop, `world_loaded` = `get_player_world_pos`
resolving) detects the title→in-world EDGE → queues a replay; the in-world→title edge autosaves dirty
state. `sidecar::pump_present()` (present thread, next to `debug_rpc::pump` in `hk_present`) drains the
queue and re-applies each custom flag via `markers::set_event_flag` (SetEventFlag must run present-side).
SEH-guarded per-flag call (`seh_set_flag` — a hand-edited/foreign `.mfg` id can't fault the present
thread). Idempotent (re-firing an edge just re-sets flags). **VERIFIED:** boot 1 wrote a benign flag to
the `.mfg`; boot 2 log showed `loaded …(1 flags)` → `world entered — flag replay queued` → `replayed 1/1
custom event flags into the session`, game alive. So a framework-set event flag now survives a full
save→quit→reload cycle. **Remaining Phase 1: only (1c) identity binding + multi-slot.** After that the
sidecar is ready to back the Gap C GRANT (Phase 2 adds the inventory strip/reinject).

### Phase 2 — inventory ITEMS via strip-and-reinject (the hard part)

**PROGRESS 2026-07-03: data layer + strip/reinject primitives BUILT; CreateFileW trigger DISPROVEN.**
Built (all in `goblin_sidecar.cpp`): the `[items]` store (`add_custom_item`/`remove_custom_item`/
`custom_items`, persisted in the `.mfg`), `strip_items()` (give_item −qty) / `reinject_items()`
(give_item +qty), reinject-on-world-enter (present pump, with the flag replay), and an after-save
reinject drain with a `g_stripped` debounce (one game save opens the save file ~5× → strip once).
RPC `sidecar additem|items`. give_item from the CreateFileW (save) thread is stable (SEH-guarded, no
crash). **BUT the CreateFileW-triggered strip does NOT yield a clean save** — empirically DISPROVEN
(cap-oracle test, `.mfg` deleted so no reinject on reload): after grant→warp-save→reload the char was
still at the item cap → the item was serialized at full count DESPITE the strip. Two fatal reasons:
(1) ER serializes the inventory buffer before/around the file-open, so stripping at CreateFileW is too
late; (2) even if timed, `reinject-after-save` restores the item and ER's frequent AUTOSAVE re-serializes
it. **So the strip trigger must be a SYNCHRONOUS hook on the save/serialization routine**
(`strip → original_save() → reinject`, one atomic bracket, no autosave window) — NOT CreateFileW.
`CreateFileW` remains right for the sidecar `.mfg` write signal + path resolution (ERR/alt-save also
work there — but for REDIRECT of the whole buffer, not strip). **NEXT: RE the ER save function** (the
routine triggered by `GameMan+0xB42`; find via find-what-accesses on 0xB42 or the CreateFileW caller's
return address) and move the strip/reinject bracket there. The data layer + give_item primitives are
done and reusable; only the trigger point changes.

Original design (trigger point now known to be the save fn, not CreateFileW):
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
