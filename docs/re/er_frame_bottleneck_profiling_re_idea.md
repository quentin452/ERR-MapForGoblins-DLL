# RE idea — name ER's hot main-thread functions (the mono-thread frame bottleneck)

Status: **IDEA / followup, not started (2026-07-05).** Feasible with existing tooling. Scoped + caveated below.

## Why / what
Live `perf record` on the running game (2026-07-05, 12s/49k samples) showed ER is **mono-thread CPU-bound**:
**~69% of samples on ONE thread** (tid == PID = the main thread) inside `[JIT]` game code, GPU ~48% idle,
vkd3d/winevulkan translation only ~4%, the mod negligible. So the frame ceiling is ER's own main-loop logic
on a single thread. The symbols were **unresolved** (`0x6ffff6805f87…`) because the on-disk exe is VMProtect'd.
This idea = **resolve those hot addresses to named ER functions** so we know WHICH subsystem dominates the frame
(render-cmd build? animation? AI/behaviour? Havok step? EMEVD? streaming?).

## ⚠ Scope caveat (read first — don't aim this at the wrong problem)
- This names the **ENGINE** bottleneck (same on Windows — the engine is mono-thread there too). Useful for
  **engine understanding** + the runtime-modding / greybox / hook-placement work, NOT for "fix the fps".
- It is **NOT** the right lever for the **Linux < Windows fps deficit on the same box**. That delta is
  **distributed wine per-call overhead** (syscalls / D3D12 call thunks / ntdll), not one hot function — it
  won't show as a single nameable RVA. Fix that with config: **gamescope** (`-f`, Wayland exclusive-fullscreen),
  a recent **Proton** (Experimental / GE — vkd3d-proton improves per version), and the CPU **governor →
  performance**. See the perf/MangoHud session notes.
- VMProtect: functions ER virtualizes won't symbolize (the sample lands in the VM interpreter, not the real
  fn). Most of the hot game loop is NOT virtualized, but expect some un-nameable buckets.

## Method A — external perf → RVA → Ghidra (cheapest, half-done)
1. `perf record -p $(pgrep -x eldenring.exe) -F 999 -- sleep 12` (already proven; paranoid=1, no sudo).
2. Map each hot sample address → module-relative RVA: read `/proc/$PID/maps`, find the `eldenring.exe`
   mapping base, `RVA = sample_addr - base`. (The `er_base` RPC also returns the live base for cross-check.)
3. Look the RVA up in **Ghidra on the DECRYPTED dump** (never the on-disk VMProtect'd exe — the decrypt/dump
   recipe is in `patch_diff_maintenance.md`). Name the function / its subsystem.
4. Cross-reference against the **RE coverage map** (`docs/re/README.md`) — many hot candidates are already named
   (`GameRend`/`GameRendCameraSet` er+0x680460, `CSPhysWorld`/Havok step, `WorldChrMan`, the menu update tick
   `FUN_140766980`, EMEVD, streaming). A hit on an already-RE'd fn is instant attribution.

## Method B — in-DLL statistical profiler (Proton-independent, RVA-native)
A sampler that attributes directly to ER RVAs (no /proc/maps step), hostable in the mod:
- Identify the hot thread = the one that calls Present (we already hook it) / highest-CPU-time thread.
- A low-prio sampler thread loops: `SuspendThread(main)` → `GetThreadContext` → RIP → `ResumeThread` →
  if RIP ∈ eldenring.exe module range, `hist[(RIP-base) >> bucket]++` → sleep ~1 ms.
- After N s, dump the top RVA buckets to the log (already module-relative → paste straight into Ghidra).
- Behind a dev flag / RPC verb (`frameprof start|stop|dump`), SEH-guarded, like the other probes. Riskier than
  perf (suspending the main thread) → keep sample windows short; perf (Method A) is safer, prefer it first.

## Deliverable / acceptance
A ranked list "RVA → ER function/subsystem → % of frame", so the frame's dominant cost is named (e.g.
"55% in the Havok character step" vs "in the render-command build" vs "in EzState/EMEVD"). Feeds: engine
understanding, hook placement, and an honest answer to "what actually limits ER's frame" — separate from the
Linux-config fps deficit (fix that with gamescope/Proton/governor).
