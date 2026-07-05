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

## Results so far (2026-07-05, Method A run — Linux/Proton, RTX 3060 + i5-10400F)
Ran the perf→RVA pipeline live (`tmp/perf_rva.py`: boot + 16s render activity + `perf record -F 999` +
`/proc/PID/maps`). **The RVA-resolution method WORKS** — eldenring.exe maps at base `0x6ffff4c00000`
(maps region, file-offset 0); `RVA = sample - base`, Ghidra addr `= 0x140000000 + RVA`.

**Hot ER functions (Ghidra addrs — NOT yet named; not in `re_signatures.hpp`):**
| Ghidra addr | RVA | % | note |
|---|---|---|---|
| `0x141C05F87` (+f47/f8d/`0x141C060C7`) | `0x1C05F47` | **~5.5% cumulated** | the dominant frame region (~0x180 bytes) |
| `0x14251BEE7` | `0x251BEE7` | 1.9% | 2nd |
| `0x1404F9950` | `0x4F9950` | 0.4% | |

### RVAs NAMED (Ghidra decompile, 2026-07-05) — confirms "flat / generic plumbing", NOT a fixable subsystem
- **`0x141C05F87` (5.5%, the dominant "region") = a FAMILY of identical 53-byte intrusive-list `find-by-id`
  lookups**, not one function. `0x1c05f30 / f70 / fb0 / ff0 …` are byte-identical (same AOB), spaced 0x40 —
  template instantiations of one generic finder, one per registry type. Each:
  `node=*(mgr+0x30); while(node){ if(*(int*)(node+0x18)==id) break; node=*(node+0x10);} return (node+0x1c flag)?node:0;`
  i.e. an **O(n) linear walk of an intrusive linked list keyed by an int id**, gated by a per-node enabled byte.
  The perf samples (+f47/+f87/+fc7/0x60c7) spread ACROSS the family → the "hot region" is the same lookup
  pattern called constantly for many different managers, not a discrete subsystem.
- **`0x14251BEE7` (1.9%) = the CRT `memmove`/`memcpy`** (size-0..16 switch + SSE-unrolled bulk copy with
  overlap handling; ERMS bit `DAT_1448574d0`, non-temporal threshold `DAT_143c5add8`; 30+ callers:
  `write_string`, `fp_format_f_internal`, `memcpy_s`, `operator=`, `__wcsrtombs_utf8`, …). Generic data
  movement from everywhere.
- **`0x1404F9950` (0.4%) = another intrusive-list membership check** (`node=*(base+8); while(node){ if(*(int*)(node+8)==id) break; node=*(node+0x30);} return node!=0;`), 29 callers across unrelated TUs.

**⇒ Verdict for "is the fps easily fixable": NO.** The top cost is **generic engine plumbing** — id-keyed
intrusive-list lookups + `memmove` — spread across hundreds of call sites, not a single subsystem you could
disable or a mod-patchable hotspot (converting FromSoft's intrusive lists to hashmaps is not moddable). Top 3
sum to only ~8%; a flat profile means meaningful fps needs speeding up hundreds of functions = impossible from
a mod. And these run on Windows too, so they are NOT the Linux<Windows delta (that stays the distributed wine
syscall/thunk tax: `clear_bhb_loop`, `rwsem_spin_on_owner`, the wine dispatchers). The only lever remains
config (`mitigations=off`, security tradeoff) — no easy code fix exists.

**⚠ Key result — the profile is FLAT.** Top single fn = only 3.67%, then a long tail. **There is NO single
dominant/pathological function.** The "69% [JIT]" of the first (unsymbolized) run is that same 69% spread
across hundreds of ER functions once the file-backed mapping resolves. So the 60(Win)→44+stutter(Linux)
deficit is **distributed** — the whole game loop a bit slower under wine — not one hot loop to fix. Naming
`0x141C05F87` via Ghidra is engine-curiosity, NOT a perf fix.

**Kernel/wine tax visible in the same profile (the real Linux<Windows gap):**
- `clear_bhb_loop` (0.47%) = Spectre/BHI mitigation on every syscall/context-switch — taxes wine (syscall-heavy).
  → **`mitigations=off`** (kernel cmdline) is the one actionable lever this surfaced (security tradeoff).
- `rwsem_spin_on_owner` (0.92%) = kernel lock contention (wine memory-mgmt / mmap_lock).
- `__wine_syscall_dispatcher` + `__wine_unix_call_dispatcher` = the per-syscall wine transition cost.

**Other perf investigation results (same session, all ruled out as the fix):** PROTON_LOG refuted CONSTANT
shader/pipeline compilation (pipeline activity only in the first ~6s = load; foz cache active; only signal was
a benign flood of vkd3d `GetResourceAllocationInfo3: Invalid resource desc` / `Invalid alignment 4096 for
buffer` warns = ER's D3D12 buffer pattern, a light query, not the killer). ntsync active. gamescope `-f`
(compositor) no help. GE-Proton10-34 already (Proton version fine). governor perf (powerprofilesctl) no help.
`nvidia_drm.modeset=1`, real NVIDIA Vulkan, full clocks — all fine. **SetCPUAffinity.dll (Nexus 2859 ER Stutter
Fix)** installed as a me3 native + VERIFIED active under Proton (pins ~8 game threads to cores 1-6, avoiding
core 0) → **no help either.** ⇒ Working conclusion: **inherent Proton mono-thread/syscall overhead on ER**
(a known FromSoft-under-Proton reality), no single fixable cause; `mitigations=off` is the last config lever.

## Deliverable / acceptance
A ranked list "RVA → ER function/subsystem → % of frame", so the frame's dominant cost is named (e.g.
"55% in the Havok character step" vs "in the render-command build" vs "in EzState/EMEVD"). Feeds: engine
understanding, hook placement, and an honest answer to "what actually limits ER's frame" — separate from the
Linux-config fps deficit (fix that with gamescope/Proton/governor).
