# Geom pose-descriptor BUILDER (`thunk_FUN_144cbdae7`) — Ghidra findings

Answers `windows_geom_spawn_builder_re_prompt.md`. Ghidra `query.java` on `D:\ghidra_proj2\ER`,
imagebase `0x140000000`, 2026-07-03. **Verdict: don't call the builder standalone — it is EH-wrapped and
streaming-context-welded; the hang is contextual, not an arg bug.**

## What the builder resolves to
The 4-arg `thunk_FUN_144cbdae7` (er+0x6c3910) does **not** reach a clean linear function — it lands in a
chain of **MSVC C++ exception-handling funclets** that the decompiler cannot linearize:
```
thunk_FUN_144cbdae7 (er+0x6c3910, jmp)  →  FUN_144cbdae7 (er+0x144cbdae7, EH funclet, garbled)
   → FUN_1450358f6 (er+0x50358f6) / FUN_1401e3126 (er+0x1e3126)   [catch/cleanup funclets]
      → FUN_1406c3956 (er+0x6c3956, 5-byte trampoline → unrecovered jumptable @ 0x145532db3)
```
Every hop is a 5–47-byte fragment in the relocated/high EH region (`0x144…`, `0x145…`), with
"Could not recover jumptable / Too many branches" — the classic signature of a function with **`try/catch`
scopes around throwing operations** (heap alloc + resource/streaming access). The real body is chopped into
funclets; there is no clean decompile to read the loop/lock from. That EH instrumentation is itself the
finding: **the builder touches resources and can throw — it is not a pure math/transform builder.**

## Why it HANGS (resolved without the body)
- **`arg4=0` is NOT the cause.** The driver passes `arg4 = *(resource+0x58)`, which live-recon measured
  **= 0** — i.e. `arg4=0` is exactly what the *working* driver call passes. So the standalone hang is not a
  bad argument value.
- ⇒ **The hang is CONTEXTUAL** (prompt Q3/Q4 = yes): called off the streaming thread / outside the driver's
  established state, the resource-touching, EH-wrapped builder **waits or loops on streaming state that only
  the tile-stream driver (`FUN_1406a7930`) sets up** before this call (the phase/index machinery at
  `BlockData+0x490/0x494`, the resource mid-load). A hang (not an AV, not a C++ throw caught by its own EH)
  is consistent with a resource-resident spin / fence wait, not a wild pointer. Blind live calls each hang →
  can't be brute-forced.

## Consequence for `spawn_clone` — the ctor-driven route is the wrong shape
Combined with the earlier findings (the descriptor `param_4` is a ~0x188-byte object that **embeds owned,
resource-backed sub-objects** — a heap pose module stolen into `self+0x20` and a move-constructed
`CSMsbPartsGeom` at `self+0x30`, both pointer-rich), there is **no cheap, safe way to produce an independent
`param_4`**:
- **Call the builder** → hangs (streaming-welded, above).
- **Alias the source** → gutted source (move-init steals the pose ptr + move-outs the CSMsbPartsGeom).
- **Shallow-copy the source's descriptor** → shares the two owned sub-objects → double-free / corruption.
- **Deep-copy** → the sub-objects are exactly the pointer-rich resource graph deep-copy can't safely walk.

So **hand-driving the Dynamic ctor from a standalone RPC is the wrong shape for ADD.** The engine only ever
builds these descriptors on the streaming thread with the resource mid-load, inside `try/catch`.

## Recommended pivots (for the Linux agent — pick one)
1. **Spawn on the streaming path, not standalone.** Hook the tile-stream spawn driver `FUN_1406a7930`
   (or `FUN_1406adc80`) and inject one extra part into its per-part loop, so the builder + ctor run in their
   native context (thread + resource state) and the descriptor is engine-built + owned correctly. Heaviest
   but correct. (This is also closer to "MSB-inject + re-stream", the fidelity route from the placement
   findings §add-routes.)
2. **Asset streaming-REQUEST path** (`FUN_1406a5080` → `FUN_1406c7000`, the one I earlier downgraded as "not
   a shortcut"): it *requests* the streamer to load/place an asset by name+transform — i.e. it hands the
   work to the stream thread instead of hand-building an instance. Re-evaluate it as the *primary* ADD
   mechanism now that the direct-ctor route is blocked. Decompile `FUN_1406a5080`'s full flow + what
   consumes the request it registers (does the streamer then spawn a geom for it?).
3. **Throwaway visual-only probe (accept the leak/risk).** For a one-shot "does a duplicated asset render"
   demo: alias the source's descriptor, fire the ctor, **never destroy the clone and never let the source
   unload** (stay in the tile). Confirms render/collision follow a spawned instance, but is NOT a usable
   primitive — do not ship. Even this risks gutting the source via the move-init, so it needs a copied (not
   aliased) descriptor with the two owned ptrs pointed at throwaway allocations — fragile.

**Bottom line:** MOVE stays fully solved; **ADD-via-standalone-ctor is a dead end** — the real ADD path is
streaming-thread spawn (pivot 1) or the streaming request (pivot 2), both bigger than the `spawn_clone`
route hoped. Recommend the Linux agent stop blind builder calls and evaluate pivot 2 (request path) first
(cheapest of the three), then pivot 1.

## Anchors
- builder chain: `thunk_FUN_144cbdae7` er+0x6c3910 → EH funclets er+0x144cbdae7 / er+0x50358f6 /
  er+0x1e3126 → `FUN_1406c3956` er+0x6c3956 → jumptable er+0x145532db3 (unrecovered).
- driver call site `FUN_1406a7930` er+0x6a7930; move-ctor `FUN_1406c3180` er+0x6c3180; CSMsbPartsGeom
  move-ctor `FUN_140cef4a0` er+0xcef4a0.
- streaming-request path lead: `FUN_1406a5080` er+0x6a5080 → `FUN_1406c7000` er+0x6c7000.
