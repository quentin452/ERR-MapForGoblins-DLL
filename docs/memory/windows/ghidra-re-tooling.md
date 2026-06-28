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
