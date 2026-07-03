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

**Two remaining paths (a DECISION):**
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
