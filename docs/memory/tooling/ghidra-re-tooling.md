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
