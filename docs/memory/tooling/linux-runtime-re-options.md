---
name: linux-runtime-re-options
description: Candidate paths for doing LIVE runtime RE on the Linux box (game runs here via Proton) instead of switching to the Windows PC — options, trade-offs, next trial step
metadata:
  type: project
---

**Problem (user, 2026-07-01):** runtime RE (Cheat Engine, live Python RPM, debugger-on-game) is
currently Windows-only by convention, forcing a two-PC workflow. But the game itself RUNS on this
Linux box (ERR under Proton at `~/Games/ERRv2.2.9.6`, DLL deployed + played daily) — the
"no live game on Linux" line in [[linux.md]]/`docs/memory/linux.md` conflates "no Windows RE
tooling comfort" with "no live target". The live target is here; the tooling gap is the question.

## Candidate paths (untested unless noted, ranked by expected fit)

1. **In-DLL instrumentation (proven, zero new tooling)** — we already do runtime RE from inside
   the process: `goblin_field_probe`, `goblin_worldmap_probe`, bind-test/FORCELOAD dev panels,
   `[SIG]` AOB framework, F9 marker dump. In-process derefs are ~free (no wineserver IPC — see
   [[linux-rpm-walk-danger]]). Extending a probe + rebuild (~1 min cross-build) is often FASTER
   than a Cheat Engine session, and the hot-reload + debug-RPC plan
   (`overlay_hot_reload_playwright_plan.md` Route B) shortens that loop further. Best default.
2. **ceserver + Cheat Engine GUI** — CE's `ceserver` runs natively on Linux (ptrace) and the CE
   GUI connects to it over TCP; GUI can run under wine on the same box, or from the Windows PC
   pointed at this box (no game on Windows needed). Gives real CE scans/watches on the Proton
   process. Needs `ptrace_scope` ≤ 1 (same-user attach) or CAP_SYS_PTRACE.
3. **Python RPM equivalent, native Linux** — `process_vm_readv`/`/proc/<pid>/mem` on the Proton
   pid replaces the Windows Python-RPM scripts; module base (eldenring.exe) from
   `/proc/<pid>/maps` (PE mappings visible under wine). Same ptrace_scope caveat.
4. **PINCE / scanmem+GameConqueror** — native CE-like scanners (GDB/scanmem based). Value scans
   fine; weaker than CE for structure dissection.
5. **winedbg / gdb attach** — breakpoints + stepping on the Proton process (EAC is already off for
   modded ER). Clunky vs x64dbg but functional; Proton's wine has gdb proxy support.

## Stays Windows-anyway

- Ghidra static RE actually runs fine natively on Linux (Java) — the "Windows preferred" habit is
  about the existing project/scripts on that box, not a real constraint.
- Oodle-compressed asset extraction stays blocked on Linux ([[linux.md]]).

## Next step (not started)

Pick ONE real RE task and trial path 2 (ceserver) against the live Proton game; if attach/scan
works, write the recipe here and relax the platform rule in `AGENTS.md`/`docs/memory/linux.md`.
Until then, default to path 1 (in-DLL probes) before reaching for the Windows PC.
