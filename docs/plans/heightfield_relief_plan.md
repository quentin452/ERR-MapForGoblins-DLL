# Plan — raycast heightfield relief (Track D2 of the ImGui-only map)

Status: **DESIGN 2026-07-04, starting.** The mod-agnostic terrain backdrop for the vmap: sample the
LIVE 3D world with down-rays → ground height + normal → hillshade + sea/land. Correct for ANY mod that
reshapes the world (kills the Convergence trap). RE is DONE (static):
`docs/re/windows_terrain_raycast_heightfield_re_findings.md`.

## The primitive (from RE)
```
int FUN_140c70360(void* ctx, u32 filter, float start[3], float segDir[3],
                  float outPoint[3], float outNormal[3], u32* outDist);  // -> 1 hit / 0 miss
ctx    = *(DAT_143d76060 + 0x98)        // CS::PhysWorld singleton er+0x3d76060; ctx+8 = hknpWorld
filter = 0x5e                            // walkable ground / map (snap-to-ground query type)
```
Per cell (x,z): `start={x, Yhigh, z}`, `segDir={0, -(Yhigh-Ylow), 0}` → `outPoint[1]` = ground Y,
`outNormal` = slope. Same world frame as markers (`worldX=mapU+7040, worldZ=-mapV+16512`) — drops
straight onto the vmap projection, no transform.

## Hard constraints (RE §6)
- **Thread:** call on the GAME UPDATE THREAD, NOT present/RPC (hknp queries race the physics step; a
  present-thread deadlock is documented for the AEG streamer / geom_spawn). ← the key infra question.
- **Loaded-region only:** rays hit only STREAMED-IN collision. Sample around the player; misses = nodata;
  extend coverage by warping (warp is RE'd + now correct).
- **In-process only:** a pure RPM heightfield is impossible — the cast must execute in-process.

## Resolution (AOB vs RVA)
The on-disk `eldenring.exe` is Steam/VMProtect-wrapped → **cannot derive AOBs from disk**. Two paths:
- **Now (validate):** resolve `FUN_140c70360` + `DAT_143d76060` by **fixed RVA** (`er_base + 0xc70360`
  / `+0x3d76060`). Build-specific (ERR 2.2.9.6 = the dev box), like the WorldChrMan RVA fallback. Gate
  the whole feature behind a config flag; flag it needs-AOB-hardening.
- **Harden later:** a Windows Ghidra pass (or a runtime `mem_dump` at the live address + hand-craft) to
  produce byte signatures for `re_signatures.hpp`, so it survives a patch / works mod-agnostic.

## Slices
### ⚠ D2.2 BLOCKED (2026-07-04) — RVA cast/ctx resolution is NOT reliable
Building the grid sampler surfaced that the primitive only hit ONCE (abddc05) and never reproduced:
- **abddc05 hit was real** (frac 0.527 = the 2088→10.67 y-drop / 4000 — geometry-consistent, not garbage).
- **Every run since misses** — game-thread (hk_c32f0, map open: sample CONTROL + all 1600 cells) AND
  present-thread (gameplay, map closed). Same player spot each time.
- **ctx diagnostic (5 interpretations, `diag_ctx`):** `*slot`→inst=`0x6ff0800` (a real object: `*(inst)`
  is a vtable in the exe), but NONE of {`*(inst+0x98)`, `*(slot+0x98)`, `inst`, `*(inst)`, `*(inst+0x8)`}
  hit, and `*(inst+0x98)` is **VOLATILE** (changes every read: 0x1bd575600 ↔ 0xd725180) — a torn/transient
  value, not a stable world-holder pointer.
- Likely causes (compounding): the ctx field is updated by the game thread so an off-thread read is torn;
  AND/OR the RVA-resolved cast fn / singleton slot is subtly wrong (the on-disk exe is packed, so the
  RVAs come from a decrypted Ghidra dump whose runtime layout we haven't verified); AND/OR collision
  streaming state.

**ROOT CAUSE NAILED (FWA/disasm session 2026-07-04):**
- **ctx = `*(*(er+0x3d76060) + 0x98)` is CORRECT** — the game's own code (disassembled live) does exactly
  `mov rax,[singleton]; mov rcx,[rax+0x98]; call cast`. The RVAs map 1:1 (real prologues verified).
- The cast (`FUN_140c70360`) prologue does **`cmp qword[rcx+8],0; je <no-hit>`** → it early-exits with
  no hit if `*(ctx+8)==0` (ctx+8 = the hknpWorld).
- **`inst+0x98` (the ctx pointer) is WRITTEN every frame by the game thread** → an off-thread read is
  TORN (observed `0x1bd575600` = 0x1 high-dword stitched onto a low dword; clean read = `0xd6f5600`). A
  torn ctx → `ctx+8` garbage/null → early-exit → MISS. abddc05 caught a clean read by luck.
- **⇒ the cast MUST run on the GAME thread during GAMEPLAY** (physics active + collision loaded + ctx not
  mid-update). `hk_c32f0` is game-thread but only map-open (collision unloaded) — a dead end.

**Unblock = hook a gameplay game-thread fn and cast there (D2.2-real):**
- **`FUN_1403f13c0` scouted (2026-07-04)** — hookable prologue (game-thread, in-world), sig ≈
  `(rcx=obj, rdx=out-vec, r8d, xmm3)`, does `mov rax,[rcx]; call [rax+0x4a0]` + reads WorldChrMan
  (`er+0x3d65f88`). BUT: (a) **player-gated** (snap events, likely NOT per-frame → slow fill), (b)
  **arg count uncertain** (stack args beyond 4 possible → a mismatched detour CRASHES; x64 clang-cl has
  no `__declspec(naked)` for a register-perfect passthrough), (c) doesn't hand ctx in 640B (self-derive
  on the game thread instead — clean). ⇒ a **mediocre, risky** target.
- On the game thread `resolve_ctx()` (`*(*(singleton)+0x98)`) is CLEAN (no torn read) — so ANY game-thread
  gameplay hook works; the target just needs to be frequent + have a KNOWN signature to hook safely.
- **No clean per-frame gameplay game-thread hook is currently identified** (hk_c32f0 = map-open only;
  grace/event hooks = irregular). Implementing D2.2 = finding/validating such a hook (RE its full
  signature first) — real hook-engineering with crash risk.

**STATUS: D2 diagnosis COMPLETE (committed); implementation PARKED pending a safe game-thread gameplay
hook.** The sampler code + RPC harness are ready to reattach once such a hook exists. Recommendation:
resume D2 when there's appetite for the game-loop-hook RE; meanwhile the ImGui-only GATE work (Track A
parity) and the RVA cheap wins are higher-value + lower-risk.

**Hook-scout harness (resume here — `tools/hf_hook_scout.py`, 2026-07-04):** a live-RPM scout that
de-risks the safe hook from Python (it does NOT hook — that's the in-DLL detour). Three subcommands
attack the three blockers directly: `slot` (pure RPM — proves `inst+0x98` is written per-frame + catches
the torn reads, confirming the game-thread requirement), `fwa` (arms a write-watch on `inst+0x98` → the
game's own per-frame writer RIP + caller land as `[FWA]` lines → the ideal hook site, clean ctx by
construction), `disasm` (`mem_dump` + capstone of a candidate prologue → summarises the int/xmm arg regs
and flags INCOMING STACK args, killing the "arg count uncertain → detour crashes" risk). Needs the new
`er_base` RPC verb (turns `er+RVA` → absolute) or `--er-base`. Flow: `slot` → `fwa` → `disasm <writer RIP>`
→ only THEN write the detour against a fully-validated site.

**Old unblock notes (superseded by the root-cause above):**
1. **find-what-accesses** (debug RPC) on the cast site during a KNOWN-good in-game cast (walk into
   something / the AEG streamer) → capture the REAL ctx pointer + how it's derived + the exact fn. This
   is the tool AGENTS.md points to for exactly this.
2. Verify the runtime module layout: `mem_dump` at `er+0xc70360` and confirm it's the expected function
   prologue (rule out a packer RVA mismatch); if wrong, derive the fn + singleton by AOB from live memory.
3. Only then resume D2.2 (sampler code below is structurally ready — it just needs a cast that reliably
   hits, on the correct thread).

The D2.1/D2.2 code (`goblin_heightfield.*`, RPCs `hf_probe`/`hf_sample`/`hf_probe_present`, `diag_ctx`)
stays as the harness for that RE.

### D2.1 — validate the primitive — ⚠ HIT ONCE, NOT REPRODUCIBLE (see D2.2 BLOCKED above)
Cold-boot RPC test (`hf_probe` → open map → `[HEIGHTFIELD]` log) confirmed:
- Resolution OK (cast fn + PhysWorld singleton; ctx = *(*(er+0x3d76060)+0x98) — both plausible ptrs).
- **Cast HITS with a valid UP normal** `(0.075, 0.994, -0.085)` = walkable ground. Ran on the GAME
  thread via `hk_c32f0` — no crash/deadlock. `outDist` is FLOAT bits (hit fraction 0..1; 0.527 matched
  2077/4000 = the y=2088→10.67 drop).
- **KEY FINDING — the cast frame is Havok BLOCK-LOCAL, NOT the world frame.** The probe used the
  player's LocalPlayer+0x6C0 block-local coords (-80.4, 52.0) and hit ground directly under them. So the
  RE-doc's "same world frame as markers" is wrong for this path: D2.2 MUST convert world XZ → the
  current physics-chunk-local frame before casting (inverse of marker_world_pos: subtract the chunk/tile
  world origin), then convert the hit XZ back to world for drawing. This ties the sampler to the
  loaded chunks around the player (consistent with "loaded-region only").
- Δfoot=-77 in the probe = the save spot sits ~77u above base terrain (not a bug).

### D2.1 (original) — validate the primitive (de-risk) — FIRST
- New `src/goblin_heightfield.{cpp,hpp}`. Resolve fn + singleton by RVA (lazy, NOT at boot — the
  boot-scan gotcha). SEH-guarded noinline call wrapper (clang-cl pattern).
- One-shot test: cast a down-ray at the player XZ (`start={px, py+2000, pz}`, `segDir={0,-4000,0}`,
  filter 0x5e); log `outPoint.y`, `outNormal`, `outDist`. **Success = outPoint.y ≈ player foot Y.**
- Call point: on the game-update thread if one exists; else try a one-shot on the present thread
  (SEH-guarded, read-only — a single query may be safe enough to validate the ABI; escalate to a
  game-thread hook if it deadlocks/crashes). RPC verb `hf_probe` to trigger it.

### D2.2 — grid sampler
- Sample an N×N grid (config extent + resolution) around the player; a FEW cells/frame (rate-limited)
  on the safe thread → store `{groundY, normal, hit}` matrix in world XZ. Nodata for misses.

### D2.3 — render hillshade on the vmap
- `shade = dot(normalize(normal), lightDir)`; sea = `groundY < seaLevelConst` (config, per page) → blue;
  land = shaded grey/biome tint. Draw as a texture (or per-cell quads) UNDER markers/tiles on the vmap
  canvas (same AddImage slot as `s_tiles`). Toggle in the toolbar.

### D2.4 — coverage extension + persistence
- Accumulate the matrix as the player moves/warps; persist so a revisit doesn't re-sample. Log what's
  nodata (no silent gaps).

## Water / biome (follow-ups, RE §4/§5)
- Sea: heuristic `y < seaLevel` first. Escalate to `GXSR WaterHeightMap` sampling only if coastlines
  look wrong under a mod (separate RE).
- Biome tint: the cast's hit body carries a material id; mapping not decoded (follow-up).

## Anchors (this build, er-relative — RE §7)
```
CS::PhysWorld singleton   er+0x3d76060   ctx = *(singleton + 0x98)
ray cast (full)           er+0xc70360    (ctx, filter, start, segDir, &pt, &nrm, &dist) -> hit
terrain filter            0x5e
player pos (ray origin)   WorldChrMan[er+0x3d65f88]+0x1e508 LocalPlayer +0x6C0
```
