# ER save-function RE — for the sidecar Phase-2 strip/reinject bracket

Goal: find the ER save routine to hook so the sidecar can strip custom items right before
serialization and reinject right after — an ATOMIC bracket (`strip → original_save() → reinject`),
which the CreateFileW trigger can't provide (ER serializes before the file-open + autosave re-dirties;
disproven 2026-07-03, see `docs/plans/shadow_sidecar_save_plan.md` Phase 2).

## Method — stack walk from the CreateFileW save-write hook

`goblin::sidecar::probe_save_callstack()` (one-shot, called from `note_save_file_opened(for_write)`):
`RtlCaptureStackBackTrace` from inside the save's CreateFileW open → the frames above us ARE the game's
save chain. Each return address is resolved to its containing function ENTRY by scanning back to the
CC-padding boundary with a prologue-shape validator (`looks_like_prologue`). Logged as `[SAVERE]`.

## Result (ERR/Proton, 2026-07-03, er_base varies per launch — RVAs are stable)

Save call chain, OUTER→INNER of the eldenring.exe frames (RVAs off imagebase):

| Frame | Fn RVA | Prologue | Role (inferred) |
|-------|--------|----------|-----------------|
| #09 | **`0x253e4b0`** | `40 53 48 83 EC 30 44 8B D1 48 85 D2 75 ?? E8` | **outermost game save fn — hook target** |
| #08 | `0x1f374a2` | `48 8B C4 48 81 EC 08 01 00 00 F3 0F 10 59 40` | big fn (serialize a section, then write?) |
| #07 | `0x240daa0` | `40 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF` | recursive BND4/sl2 **write tree** |
| #06 | `0x240c530` | `48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 40` | write-tree node |
| #05 | `0x240daa0` | (same as #07 — RECURSION → confirms a tree write) | write tree |
| #04 | `0x2412f60` | `48 89 5C 24 08 57 48 83 EC 20 48 8D 05 …` | write helper |
| #03 | `0x1ee59a9` | `40 57 48 83 EC 30 …` | stream helper |
| #02 | `0x1fc0b70` | `48 89 5C 24 08 57 48 83 EC 20 48 8B 01 0F B6 FA` | low-level file open (calls CreateFileW) |

Above #09: `0xb28bb31` / `0xb2fff78` (very high RVAs) + an extern frame = the worker-thread task
dispatch (the save runs on a worker thread — the CreateFileW is NOT on the main thread).

Key deductions:
- `0x240daa0` appears at two stack depths (#05 and #07) → the write side is a RECURSIVE tree walk
  (BND4 nodes), operating on an ALREADY-SERIALIZED buffer. So serialization is NOT on this stack —
  it ran before the write. Stripping at the write (CreateFileW) is therefore too late (matches the
  earlier disproof).
- The strip must bracket BEFORE serialization. If serialize+write are nested under ONE save routine,
  that routine's entry is before serialize → **`0x253e4b0` (#09) is the outermost candidate**. If
  serialize is a wholly separate earlier phase (not reachable from a write-time stack), `0x253e4b0`
  brackets only the write and hooking it won't help — must be confirmed empirically.

## Update 2026-07-03 — the outer bracket is INDIRECT-dispatched; serialize is a separate phase

- The `find_fn_entry` CC-scan heuristic MISLABELED the outer frames. Verified: an observer hooked on
  `0x253e4b0` (the heuristic's #09) got **0 calls during real saves** → it is not the caller. So the
  heuristic entries for the outer frames are wrong.
- Reliable method = resolve each frame's callee from its call-site (`ret-5 == E8` → `callee = ret +
  int32(ret-4)`). Only DIRECT calls resolve; results: #03 `0x1ee5c10`, **#04 `0x24142e0`** (big fn,
  prologue `40 55 56 57 41 54 41 55 41 56 41 57` = pushes rbp/rsi/rdi/r12-15), #06 `0x240c530`. The
  OUTER frames (#08/#09) and the worker frames (#10/#12) show `callee=0` → they call via **INDIRECT
  dispatch** (`FF /2` / vtable) — the ER save system is a virtual/task-based worker. So the outer
  save-routine entry is NOT reachable by rel32 from this stack, and the reliable inner entries
  (`0x1ee5c10`/`0x24142e0`/`0x240c530`) are all on the WRITE side (post-serialize) — too late to strip.
- Combined with the CreateFileW disproof (item already serialized when the file opens), the inventory
  SERIALIZE runs in a phase BEFORE the write stack — a separate function not on the write-time
  callstack. Hooking it needs a different entry point (below), not the write chain.

## Update 2026-07-03 (PM) — the SERIALIZE is located via a live find-what-accesses (Linux)

Ran the prompt's decisive method IN-DLL on Linux (`goblin_field_probe` HW breakpoint + new RPC
`equip_dump`/`mem_dump`/`mem_fwa`, `GAME_DATA_MAN` sig, the GameDataMan→PlayerGameData→EquipGameData
resolve). Key moves + results:
- `GameDataMan` (accessor AOB, sig `GAME_DATA_MAN`) → `+0x8` PlayerGameData (PGD) → `+0x2B0` EquipGameData.
  Both `[SIG] PASS`; `SAVE_FN` (SLSaveSession::run) also PASS on the ERR build (RVA 0x240FD70).
- The goods inventory is NOT a flat array in PGD/EGD (0..0x2000) — it is HANDLE-indirected / separately
  allocated, so it's a poor FWA target. Instead watched a COLD, always-serialized field: the CHARACTER
  NAME at **`PlayerGameData+0x9c`** (UTF-16; read only in menus / on save, NOT the in-world HUD).
  ⚠ A HOT byte (EquipGameData+0x8, equipped id, read every frame by ~104 threads) CRASHED the game via
  the VEH single-step storm — always pick a cold byte.
- FWA armed on the name + warp (save): **hit** `READ by rip=er+0x251bee7`, whose bytes end in `F3 A4`
  (**rep movsb**) → the serialize copies PlayerGameData via a **memcpy** (fn `er+0x251b83c`).
- Enhanced the FWA VEH to log the stack return-address chain → the memcpy's callers (the real serialize):
  `er+0x1edeb0b` (fn **`0x1ede9d0`** — a field-serializer: `mov rdi,rcx; add rcx,8; call memcpy`) ←
  `er+0x1ede71e` (fn **`0x1eddec0`** — section dispatcher: `mov rcx,[rcx+8]; test; call`) ← higher
  (`0x260ce0`→fn `0x25f2d0`, `0x257f55`→fn `0x2573c0`).

**So the serialize is the `er+0x1edexxxx` cluster.** The strip/reinject bracket = a function that wraps
the player-data serialize (strip at entry so the inventory section serializes clean, reinject at exit).
Candidate bracket entries (prologues confirmed via offline PE-mapped CC-scan):
- `0x1ede9d0` `48 89 5C 24 08 57 48 83 EC 20 48 8B F9 8B DA 48 83 C1 08 E8` (per-field — too granular)
- **`0x1eddec0`** `48 83 EC 28 48 8B 49 08 48 85 C9 74 1B E8` (section dispatcher — likely per-section)
- `0x25f2d0` / `0x2573c0` (higher — candidate whole-content orchestrators; the true once-per-save bracket)

**NEXT (final step):** hook the outermost of these as an OBSERVER (once-per-save + reads GameDataMan →
it's the player-data serialize), then wire strip-at-entry / reinject-at-exit and re-run the cap-oracle
clean-save test. The exact bracket among `0x1eddec0` / `0x25f2d0` / `0x2573c0` needs the observer
(once-per-save = the orchestrator) — one focused boot. All the FWA/dump RPC infra is committed + reusable.

## Update 2026-07-03 (PM #2) — observer-to-bracket hit an entry-pinning wall

Tried to promote the serialize find to a hookable bracket. Results:
- `SERIALIZE_FN` = `0x2573c0` (unique AOB) — hooked, but fires 2× at BOOT and 0× during a save → it's a
  GENERIC/load serialize, not the save path. Its FWA stack frames (`0x260ce0`/`0x257f55`) were STALE
  stack garbage, not real callers of the name-copy.
- The RELIABLE save-serialize frames are only the tight innermost: memcpy `0x251b83c` ← `0x1ede9d0`
  (field copy, ret `0x1edeb0b`) ← `0x1eddec0` (ret `0x1ede71e`). But hooking `0x1eddec0` AND `0x1ede9d0`
  by exact RVA both got **0 calls during a save** → the offline CC-scan MISLABELS their entries (ER
  .text isn't cleanly int3-padded; a hook at a non-entry never fires). No reliable offline way to pin a
  mid-function caller's entry without a disassembler.
- The serialize runs on a WORKER thread (FWA tid=352). The hookable SaveLoad2 functions are either the
  WRITE side (`SLSaveSession::run` 0x240FD70 — too late, post-serialize) or session-create/dispatch
  (likely async-queues the worker → a bracket around it wouldn't hold synchronously around serialize).

**WALL:** the serialize is located (er+0x1edexxxx, worker thread, memcpy-based) but NOT reliably
hookable for an atomic strip bracket without proper function-boundary analysis (Ghidra, or capstone
linear-disasm from a known start to pin the enclosing function of `0x1edeb0b`). Infra built + committed:
`GAME_DATA_MAN`/`SAVE_FN`/`SERIALIZE_FN` sigs, `equip_game_data` chain, RPC
`equip_dump`/`mem_dump`/`equip_fwa`/`mem_fwa`, FWA stack-chain logging, SAVE_FN/SERFN observers.

**RECOMMENDED next:** either (A) use capstone (already used on the Windows box) to pin the enclosing
function entry of `er+0x1edeb0b` (disassemble the SaveLoad2 serialize path from a known SaveLoad2 vtable
method forward, following calls, until reaching the fn that iterates the player-data sections) → hook
THAT; or (B) ship **Variant B** (reserved-id: the custom item lives in the `.err` under a high goods id,
blank/unknown if loaded DLL-less — tolerable for ERR) using the shipped `give_item` + sidecar store, no
serialize hook. Given the diminishing returns of the hook hunt, B is the pragmatic ship.

## Update 2026-07-03 (PM #3) — capstone pin: serializer is a shared reflection walker; wall confirmed

Installed capstone; added call-target (E8-rel32) verification + a serialize observer with stack-chain
dedup + `sidecar serclear` (reset the capture right before a save to isolate save-only chains). Results:
- Call-target analysis nailed the reflection serializer: `0x1ede9d0` has **0 E8 callers** (mid-block —
  why RVA-hooking it fired 0×); **`0x1ede700` has 6 E8 callers** = the real recursive object-serializer
  entry. Hooking `0x1ede700` FIRES — ~150+ distinct call sites per operation, all on the save worker
  (tid 352). It is a GENERIC reflection/schema serializer used by **both save AND load** (a `serclear`
  before the warp still produced the same deep chain as boot/load).
- The deepest common eldenring frame across ALL save serialize chains = **`er+0x24fb88b`** (142/142),
  below the worker-thread thunks (`er+0xb290d59`/`er+0xb3001ff`). That's the save/load session-run
  orchestrator. But capstone-pinning its function entry gives `0x24fb797` with **0 E8 callers** (false
  CC boundary) — ER `.text` has CC bytes MID-function, so neither CC-scan nor capstone-from-nearest-CC
  reliably finds these entries.

**WALL (confirmed, deeper):** (1) the save serializer is a generic REFLECTION walker shared by save AND
load → even a working hook needs save-vs-load discrimination (session type / direction arg); (2) the
orchestrator entries can't be pinned by offline CC/capstone heuristics (mid-function CC bytes). Cleanly
solving Variant A needs **real Ghidra** (auto-analysis / manual function bounds) on the SaveLoad2 +
reflection paths, not offline scripting. Reusable infra committed: capstone entry-finder + E8 call-target
counter (this file's recipe), `0x1ede700` = the reflection serializer entry, `er+0x24fb88b` = the
session-run orchestrator return addr.

**DECISION STANDS: ship Variant B** (reserved-id, item tolerated in the `.err`, blank if DLL-less) with
the shipped `give_item` + sidecar store — no serialize hook. Revisit Variant A only with Ghidra.

## Update 2026-07-03 (PM #4) — Ghidra check: the PM#2/#3 "wall" addresses were MISIDENTIFIED

Ran the reusable Ghidra tooling (`D:\ghidra_proj2\ER`, `query.java`) on the PM#3 lead addresses. **Both
were wrong**, so the "wall" premise collapses:
- **`er+0x24fb88b` is NOT the save orchestrator** — it is inside `FUN_1424fb774` = the CRT
  `__scrt_common_main` (`_initterm` / `_get_wide_winmain_command_line` → calls WinMain). The "142/142
  deepest common frame" was the **thread/CRT stack base** — stack-walk garbage, confirming PM#2's own
  "FWA frames were stale stack garbage" note. There is no save orchestrator here.
- **`er+0x1ede700` is NOT a reflection serializer** — the whole `0x1edexxxx` region is
  **`DLIO::DLOutputStream`** (Ghidra shows the vftable store `*this = DLIO::DLOutputStream::vftable` in
  the 0x1ede9d0 dtor). `0x1ede700` is a low-level **stream write-bounds primitive** (`if (used < need)
  vcall(stream, -12); else vcall(stream, 0)`), which is why it fires ~150×/op and on boot — every byte
  write goes through it. It is NOT the game-state serializer, and "shared by save AND load" just means
  "it's the shared stream writer."

Corrected picture from Ghidra (clean function boundaries — the "mid-function CC" limitation is an
OFFLINE-heuristic problem, not a Ghidra one):
- `SLSaveSession::run` (`0x240fd70`) decompiles cleanly as the file-**WRITE** state machine: builds
  `L"%s\\%s%s"` paths and writes each section from the pre-filled content at `[this+0xe0]` (a container:
  `FUN_142409120`=count, `FUN_14240a7e0`=get-by-index of pre-serialized sub-blocks). Confirms the write
  side operates on already-serialized data.
- **SaveLoad2 is entirely vtable-dispatched** — the ctors/methods/wrappers have **0 direct E8 callers**
  (verified on the SLSaveSession/SLSaveContent ctor wrappers + SLSystemImpl methods). THIS is the real
  reason the stack-walk couldn't reach the outer routine — not "mid-function CC". Climbing needs vtable
  xrefs, not call-site walks.
- The CT's `GameMan+0xB42` save-request offset does **not** exist as a static displacement in this build
  (resolved GameMan slot `er+0x3D69918` via AOB; 0 `[reg+0xB42]` mem-operands) → CT offset is
  version-drifted; Method A needs the live `[reg+0xB42]` found by a HW-bp, not a static scan.

**Net:** Variant A is **tractable in Ghidra** (clean boundaries; wall was a misdiagnosis), but the real
game-data SERIALIZE (GameData → the content sub-blocks) is a **separate, still-unpinned** function on
the save-request path — NOT any previously-cited address. Finding it wants a bespoke Ghidra pass
(iterate the GameDataMan data-xrefs → containing functions → the one that writes a `DLOutputStream` into
the save buffer), which `query.java`'s per-address mode doesn't do. **Variant B still the pragmatic ship;
Variant A now unblocked for a focused Ghidra session** (no longer "impossible offline" — just not yet
done). Full check log: `docs/re/windows_save_function_rpm_re_findings.md` §Ghidra.

## Update 2026-07-03 (PM #5) — SERIALIZE FOUND: `FUN_14067dc00` (er+0x67dc00)

Did the bespoke Ghidra pass — `tools/ghidra/find_serialize.java` (GameDataMan data-xref ∩
DLOutputStream-writer). Top hit decompiled to the **whole-slot save serializer** `FUN_14067dc00`: builds
a `DLIO::DLOutputStream` over the out-buffer, writes header → `FUN_140257f20` (player data incl. inventory
— reads `GameDataMan`→PlayerGameData live) → ~11 section serializers → trailer, then seeks to 0 and
rewrites the size header (unmistakable write). **Save-specific** (write-only DLOutputStream — no shared
read/write, so no save-vs-load discrimination), **synchronous, direct-called** (4 save-trigger callers).
Convention `rcx=save_ctx, rdx=out_buffer, r8d=size, r9=&out_written`. Strip@entry / reinject@exit = the
atomic bracket (inventory serialized INSIDE, after entry). Unique AOB → `SERIALIZE_FN` in
`re_signatures.hpp` (replaced the wrong `0x2573c0`). Only a live observer (fires-once-per-save + thread)
remains before wiring strip/reinject. Full write-up: `docs/re/windows_save_serialize_re_findings.md`.

## Two remaining paths (original — now superseded by the recommendation above)
- **Variant A (clean vanilla save) — more RE.** Find the SERIALIZE phase / the save-REQUEST processor
  (the fn that reads `GameMan+0xB42` and initiates serialize+write) via find-what-accesses on
  `GameMan+0xB42` (we have the HW-breakpoint infra, `goblin_field_probe`). Hook its entry (strip) +
  exit (reinject) — the true atomic bracket before serialization. Non-trivial: virtual dispatch +
  worker thread.
- **Variant B (reserved-id tolerate) — ships now.** Let the custom item live in the save under a
  reserved high goods id; loaded DLL-less it is a blank/unknown item (NOT corruption — a valid-shaped
  inventory entry the base game just doesn't define), which the plan already calls tolerable for ERR
  (the `.err` is mod-locked anyway). Uses only the shipped `give_item` + the sidecar item store (for
  the clean-uninstall record); NO save-fn RE. The `.sl2`-vanilla-clean payoff (Variant A) matters most
  for vanilla installs, less for ERR.

## Original plan — observer hook (superseded by the update above)

Hook `0x253e4b0` as a READ-ONLY observer (AOB `40 53 48 83 EC 30 44 8B D1 48 85 D2 75 ?? E8`, `SAVE_FN`
in `re_signatures.hpp`), logging entry/exit + thread id + call frequency. Confirm: (a) it fires on a
save (grace rest / warp / quit), (b) it is save-SPECIFIC (not also loads / unrelated ops), (c) it
brackets the whole save (one call per save, serialize happens inside). THEN wire strip-at-entry /
reinject-at-exit and re-run the cap-oracle clean-save test. If `0x253e4b0` turns out to be write-only,
escalate: find the serialize phase via find-what-accesses on `GameMan+0xB42` (the save-request flag)
or hook `0x1f374a2` (#08).
