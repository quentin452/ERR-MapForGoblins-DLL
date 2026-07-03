---
name: ghidra-re-tooling
description: Reusable Ghidra RE tools (query.java + rtti_index) — use these before writing a one-off find_xxx.java
metadata: 
  node_type: memory
  type: reference
---

For ERR `eldenring.exe` RE, use the reusable tools FIRST — don't write a new `find_xxx.java` per task.
The full exe is already decompiled once (persisted `<ghidra_project>\ER`, reused via `-noanalysis`, ~2 min
load). See [[ghidra-worldmap-re]] for the headless setup. Committed in repo `tools/ghidra/` (commit a0187c8).

**Project map (don't confuse them):** `D:\ghidra_proj2\ER.rep` = the analyzed **game**
(program name `eldenring.exe`, ~2.7 GB DB — open this for game RE). `D:\ghidra_proj_ergg\ergg.rep`
= the small **ERR loader DLL** only (~59 MB, program `ergg.dll`) — NOT the game; useless for FMG/param RE.

**Ghidra here is 12.1.2 → Jython is GONE.** `analyzeHeadless … -postScript foo.py` dies with
"Ghidra was not started with PyGhidra. Python is not available". Two valid paths now:
- **`.java` GhidraScripts** (`query.java`, `rtti_index.java`) still run via `analyzeHeadless` — unchanged.
- **Python = pyghidra** (`pip install pyghidra capstone`; set `GHIDRA_INSTALL_DIR=D:\ghidra\ghidra_12.1.2_PUBLIC`).
  Open the existing analyzed project **read-only, no re-analysis**, drive the full Ghidra API in CPython:
  ```python
  import os, pyghidra; os.environ["GHIDRA_INSTALL_DIR"]=r"D:\ghidra\ghidra_12.1.2_PUBLIC"; pyghidra.start()
  from ghidra.base.project import GhidraProject
  gp   = GhidraProject.openProject(r"D:\ghidra_proj2", "ER", True)     # readOnly
  prog = gp.openProgram("/", "eldenring.exe", True)                    # existing program, no import
  # Java byte[] via jpype (no `jarray` module in CPython): jpype.JArray(jpype.JByte)(n)
  ```
  Good for bespoke scans (AOB byte-search, ref/call tallies, batch decompile) when `query.java` isn't enough.
  Symbols can be in the EXTERNAL space → guard `addr.subtract(imageBase)` in try/except.

- **`<ghidra_scripts>\rtti_index.txt`** (live copy; a snapshot was committed to `tools/ghidra/` @ a0187c8
  but is NOT in the current working tree — grep the D:\ path) — TSV `vtable_rva  td_rva  ctor_rvas  mangled_name` for all
  9760 classes / 10202 vtables. Grep instead of re-deriving RTTI by hand: `grep 'MapIns@CS@@$'`,
  `grep -i worldgeom`, `grep $'\t0x6c5900'` (which class owns ctor @ that RVA). RVAs are er_base-relative
  for one build → regenerate after a patch (`rtti_index.java`; header records imagebase).
- **`<ghidra_scripts>\query.java`** — parametrized decompile, no per-RE script:
  `analyzeHeadless ... -postScript query.java <0xADDR|name:SUBSTR> ...` → fn decomp + entry AOB +
  rip-relative static globals + callers, OR RTTI-walk a class → vtable/vmethods/ctors. Output →
  `<ghidra_scripts>\out_query.txt` (UTF-8 file; headless mangles multi-line stdout).
- **`rtti_index.java`** builds the index (COL self-RVA detection + one byte-pass vtable match — no
  per-class memory scan; ~minutes).

Fast path for a new RE: grep `rtti_index.txt` → class → vtable/ctor RVAs; `query.java` those; only write
a bespoke script for custom scans/iteration. (Decompiling ALL 100k+ funcs to text = GBs + VMP noise +
all `FUN_` names + stale-on-patch → not worth it; the index + on-demand query is the sweet spot.)

## Operational playbook — driving `query.java` headless from THIS harness (2026-07-03, cost real cycles)
- **The invocation that works** — the **PowerShell tool** (NOT `cmd.exe`; see [[windows-tooling-gotchas]]),
  `run_in_background: true`, then wait for the completion notification:
  ```
  $env:GHIDRA_INSTALL_DIR="D:\ghidra\ghidra_12.1.2_PUBLIC";
  & "D:\ghidra\ghidra_12.1.2_PUBLIC\support\analyzeHeadless.bat" D:\ghidra_proj2 ER `
    -process eldenring.exe -noanalysis -scriptPath D:\ghidra_scripts -postScript query.java 0xADDR 0xADDR2
  ```
  `cmd.exe /c "...\analyzeHeadless.bat ... > /d/foo.log"` **fails twice over**: cmd only prints its banner
  (nested-quote bridge), AND a Git-Bash `/d/...` redirect path is invalid for cmd (needs `D:\`).
- **~2 min DB load per run; do NOT kill `java` mid-run** (I killed it twice chasing "stuck" runs and just
  restarted the 2-min load). Two headless runs on the **same `.rep`** contend/lock — run them **sequentially**.
- **`| Select-Object -Last N` buffers** → the background task's `.output` file stays **empty until the run
  ends**, so polling it looks "stuck". Instead poll the real artifact: `head -1 /d/ghidra_scripts/out_query.txt`
  and check its **`== query … args=[…] ==`** header shows YOUR targets (query.java overwrites this file each run).
- **Verify the args echo** — a typo like `0x56090 1a` (a space split) becomes two junk targets; a bare token
  is treated as `name:<token>` → a **huge RTTI name-walk** that balloons the run. The header catches it.
- **`name:<substr>` matches too broadly**: `name:CSWorldGeomIns@CS@@` also hits every `_Func_/lambda` vtable
  whose mangled name contains it. To get THE class vtable, **grep `rtti_index.txt` for the exact vtable RVA**
  and read that block, not the whole name-walk.
- **The vtable walk prints only the first ~6 slots.** A virtual at a high offset is NOT in that dump — find
  it by decompiling **callers** and reading the `(**(code**)(*self + 0xNN))(...)` indirect call. (This is how
  the geom transform **setter `vtable[0xd0]`** was found — `windows_msb_placement_write_re_findings.md`.)
- **EH-funclet trap:** a function that decompiles as a 1-line garbled fragment whose call target lands in the
  **high `0x144xxxxx`/`0x145xxxxx` region**, with phantom `in_stack_…`/`in_RAX` args and "Could not recover
  jumptable / Treating indirect jump as call", is an **MSVC C++ exception-handling funclet**, not the real
  body. Follow the thunk→funclet chain back to a normal-region `FUN_` (usually right next to the thunk), but
  the decompiler often **cannot linearize a `try/catch` body** — recognize the signature and stop chasing
  (worked example: the geom pose-descriptor builder `thunk_FUN_144cbdae7`,
  `windows_geom_spawn_builder_re_findings.md`).

## Live-probe coordination (Windows Ghidra ↔ Linux/Proton) — the platform split
- **This Windows box runs the game (`eldenring.exe` + `me3`), but its loaded mod DLL is usually STALE**
  (older than HEAD). So driving the mod's **dev RPC** here (`geom_dump`, `spawn_probe`, `move_asset`, …)
  hits **old code** — don't trust it for verifying new findings; hand live RPC/deploy confirmation to the
  **Linux/Proton agent** (it owns deploy+RPC and the E2E tests). Pure **`ReadProcessMemory` of GAME-static
  globals/structs** is DLL-version-independent, so RPM recon here is fine when you can locate the target.
- **RPC transport** (if you do use it): loopback **TCP `127.0.0.1:<port>`**, port from the deployed ini
  `[Debug] debug_rpc_port` (empty = disabled); client **`tools/mfg_rpc.py`** (`Rpc(port).cmd("…")`), works
  from Windows and Linux alike.
- **Division of labor:** Windows = Ghidra static (this doc) + Cheat Engine; Linux = deploy + RPC + live
  probe + E2E. The `docs/re/*_re_findings.md` files ARE the handoff medium between the two — write the
  static answer + a live-verify checklist, don't try to close the loop on the wrong box.
