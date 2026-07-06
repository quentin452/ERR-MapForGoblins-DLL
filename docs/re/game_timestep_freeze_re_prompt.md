# RE plan — game timestep / clean freeze (fix the pause resume-latency; add `set_timescale`)

**Status:** SCOPED 2026-07-06. Linux-runtime-doable (in-DLL FWA/RPM proven) + a short static
decompile on Windows (`D:\ghidra_proj2\ER`). The RE is ~70% done — the per-frame time driver is
already identified; this plan finds the exact dt lever and reworks the pause onto it.

## Why
`goblin_pause.cpp` flips `PAUSE_BRANCH` (je↔jne) — it gates the world STEP but **not the world CLOCK**,
so wall-time accrues while paused and the game drains the backlog on resume → **resume hitch ∝ pause
duration** (live-confirmed 2026-07-06, `windows_ingame_pause_re_prompt.md`). The clean fix = freeze the
CLOCK: drive the per-frame `dt` to 0 while paused (nothing accumulates → instant resume). Same lever
gives a free dev `set_timescale` (slow-mo / fast-forward).

## ★ The anchor we already have (RE ~70% done)
`windows_geom_spawn_thread_re_findings.md` + `windows_re_live_refresh_grace_lead.md`:
- **`FUN_140623410` (er+0x623410) = the main per-frame update task.** It **takes `float dt`, wraps it in
  an `FD4Time`, and drives dozens of subsystems with it** every frame while in-world (gated by a
  "world active" flag `cVar9==0`). Prologue ends `48 81 EC B8 00 00` (`sub rsp,0xB8`). Dispatched from
  the 0xaf6xxx task region.
- Its child **`FUN_1406d31f0(reqMgr, FD4Time*, …)`** (er+0x6d31f0) receives the built `FD4Time*` — proof
  the FD4Time (with its `deltaTime` float) is materialized inside `FUN_140623410` per frame.
- So `dt → FD4Time.deltaTime → every subsystem`. Zero `dt` (or `FD4Time.deltaTime`) = the whole world
  sees no time pass = frozen, clock included = instant resume. This is strictly better than the branch flip.

## RE questions (the remaining 30%)
1. **Where does `FUN_140623410` get its `dt`?** A float arg (xmm0/xmm1), or read from a global (a
   `CSFD4Time`/`FD4Time` singleton member) before it builds the FD4Time? Decompile the head of
   `FUN_140623410` → find the dt source and the store into `FD4Time.deltaTime`.
2. **Is there a global time-SCALE multiplier** (normally `1.0f`, used by cutscene slow-mo / the game-speed
   debug)? `dt_effective = dt_real * scale`. If it exists, writing it is the cleanest lever (no hook).
3. **`FD4Time` layout** — offset of the `deltaTime` float the subsystems read (from the `FUN_1406d31f0`
   call site: `param_2 = &FD4Time`, so decompile the first few field loads there).

## Implementation paths (ranked)
- **A — write a global (no hook), PREFERRED if it exists.** If `dt` comes from a global `CSFD4Time`
  member OR there's a time-scale multiplier global, `goblin::pause` just writes `0.0f` on pause / restores
  on resume (and `set_timescale <f>` writes any value). Find it via RTTI (`tools/ghidra/rtti_index.txt`
  grep `Time`/`FD4Time`/`CSFD4Time`) or FWA on the `FD4Time.deltaTime` write.
- **B — MinHook `FUN_140623410`, scale the incoming `dt`** by a global `g_timescale` before the
  trampoline (0 = paused). Works even if `dt` is a pure register arg. Watch the dt=0 edge: the engine
  already passes ~0 during loading screens, so subsystems tolerate it — but verify no divide-by-0 spike
  (audio/anim). Use a body-anchored AOB (the bare `sub rsp,0xB8` prologue is NOT unique — the sibling
  `FUN_1406d31f0` had the same problem, 3 matches; anchor deeper in the body or pin by RVA + `[SIG]` check).
- **C — reference lever (Option 2, orthogonal): ER's own native-map freeze.** Opening the map offline
  pauses the world with ZERO resume lag. DIFF a memory region **play vs native-map-open** to isolate the
  freeze flag ER itself sets; toggling that is the engine's own instant unfreeze. Good cross-check that A/B
  behave like native. Anchor: `CSSessionManager` Imp `er+0x2b9a0c8` / `windows_loading_screen_state_re_findings.md`.

## RE recipe
1. **Static (Ghidra `D:\ghidra_proj2\ER`, ~30 min):** `query.java 0x623410` → decompile head of
   `FUN_140623410`. Trace the `float dt` origin (arg vs global) + the `FD4Time.deltaTime` store + any
   `* scale`. Report: is there a writable global? its RVA? the FD4Time.deltaTime offset?
2. **Live (Linux in-DLL, the daily-build box):** if a global was found, `mem_dump` it (expect ~0.016 at
   60fps, or 1.0 for a scale) → write 0 via a scratch RPC → world should freeze; write back → instant
   resume. If dt is a pure arg, prototype hook B. NB `mem_fwa` needs the **`mem_fwa off` disarm verb**
   (queued from `windows_enemy_name_hud_feed_re_findings.md` — the single FWA slot wedges); land that first.
3. **Verify the fix:** pause 60 s → resume → measure the hitch. Target ≈ 0 (vs the branch flip's
   duration-proportional hitch). Compare side-by-side. Also confirm no audio glitch / animation pop.

## Deliverable
- The dt/time-scale address (house-style AOB into `src/re_signatures.hpp`) or the hook.
- `set_timescale <f>` RPC (0=pause, 1=normal, <1 slow-mo, >1 fast-fwd) — a dev lever + the pause primitive.
- Rework `goblin::pause::set_paused` to zero/restore the timestep (keep the `PAUSE_BRANCH` flip only if it
  turns out to be the actual clock gate — it isn't, per the live symptom). Update
  `windows_ingame_pause_re_prompt.md` Status + `docs/plans/dx_bugs_backlog_plan.md` PR D.

## Cross-refs
`windows_ingame_pause_re_prompt.md` (the pause feature + live-confirmed bug) · `windows_geom_spawn_thread_re_findings.md`
(FUN_140623410 = the dt driver) · `windows_re_live_refresh_grace_lead.md` (per-frame FD4Time driver) ·
`windows_loading_screen_state_re_findings.md` (native freeze / CSSessionManager, Option-2 reference).
