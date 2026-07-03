# Ghidra prompt — decomp the geom pose-descriptor BUILDER (`thunk_FUN_144cbdae7`)

> **ANSWERED → `windows_geom_spawn_builder_re_findings.md` (2026-07-03).** The builder resolves to MSVC EH
> funclets around a `try/catch` resource-touching body (`FUN_1406c3956` + jumptable) — no clean decomp.
> `arg4=0` is what the working DRIVER passes, so the hang is **contextual** (streaming-thread/lock-welded),
> not an arg bug. Combined with the descriptor's owned resource sub-objects, **hand-driving the ctor
> standalone is a dead end** → pivot to streaming-thread spawn (hook the driver) or the asset-request path
> (`FUN_1406a5080`). Stop blind builder calls.

> **Why:** the ADD-a-geom `spawn_clone` primitive is fully coded and every other arg is live-verified, but
> the one call that builds `param_4` (the ~0x188-byte pose descriptor the Dynamic ctor `FUN_1406b9880`
> move-inits) **HANGS the game** when called standalone with the args we resolved. A hang (not an AV) =
> unbounded loop or a lock. We need the builder's exact semantics before the live call can work. See
> `windows_geom_spawn_re_findings.md` (LIVE spawn_clone attempt) + `windows_msb_placement_write_re_findings.md`.

Imagebase `0x140000000`; project `D:\ghidra_proj2\ER`; tool `query.java`.

## The function
- **`thunk_FUN_144cbdae7`** at **er+0x6c3910** → target **er+0x144cbdae7** (Ghidra decompiled the target as
  a garbled fragment last time — it sits in a relocated/hot region; try harder: follow the thunk, set the
  calling convention, and decompile the *target*, not the thunk).
- Called by the tile-stream spawn driver `FUN_1406a7930` (er+0x6a7930) — and identically at a second driver
  site — as:
  ```c
  thunk_FUN_144cbdae7(local_1e8 /*out, ~0x188 bytes*/, param_1 /*BlockData*/,
                      local_278 /*partsList = *(*(BlockData+8) + 0x48)*/, *(*(BlockData+8) + 0x58));
  ```
  (4-arg signature, identical at both driver call sites per 9081c7c8.)

## What we already know (live, this ERR build)
- `out` is a ~0x188-byte descriptor. `FUN_1406c3180` later move-inits it into the new instance
  (`self+0x20` pose module + move-constructs the embedded `CSMsbPartsGeom` at `self+0x30` from `out+0x18`).
- Live arg values that HANG: `BlockData` valid; `resource = *(BlockData+8)` = valid (vtable er+0xa7d4b0);
  `partsList = *(resource+0x48)` = a valid heap ptr; **`arg4 = *(resource+0x58)` = 0**.

## Questions (in priority order)
1. **What is `arg4`?** Index into `partsList`? A flags word? A count/limit? The hang with `arg4=0` suggests
   0 is degenerate for whatever it means (empty loop bound, or "part 0" that then loops). **Where does the
   builder select WHICH part** of `partsList` to build the transform for — from `arg4`, from a field on
   `BlockData` (e.g. the stream index `BlockData+0x494`), or from `partsList` internal state?
2. **Does it LOOP?** Over `partsList` (what's the element stride + terminator/count)? An unbounded or
   count-driven loop with a wrong count = our hang. Give the loop bound source.
3. **Does it take a LOCK / wait on a fence / spin on a streaming flag?** (CriticalSection, `FUN_140b32880`-
   style manager lock, an event, a "resource resident" spin.) That would deadlock when called off the
   stream thread. Name the sync primitive + what sets it.
4. **Can it run standalone off the streaming thread**, or is it welded to the streaming state machine
   (reads/writes `BlockData+0x490/0x494` phase/index, expects the resource mid-load)? If welded, what's the
   minimal precondition to call it in isolation.
5. **What is `partsList` exactly** (`resource+0x48`) — an array of part descriptors (stride?), a FromSoft
   list header `{begin,end,cap}`, or a single part? And `resource+0x58` (=0 live) — its real meaning.

## Deliverable
The builder's real 4-arg signature + arg4 semantics + the part-selection source + whether it loops/locks and
is standalone-safe. If it's NOT safely callable in isolation, propose the alternative to get a fresh OWNED
`param_4` (e.g. a smaller sub-builder, or the minimal fields to construct the ~0x188 descriptor by hand from
a source part + a 4x4). Write findings to `windows_geom_spawn_builder_re_findings.md`.
