# Ghidra RE tooling — reusable, no more one-off scripts per RE

The full `eldenring.exe` is **already decompiled once** and persisted at `D:\ghidra_proj2\ER`
(~1.4 GB, the ~2 h auto-analysis is saved). Every query reuses it via `-noanalysis` (≈2 min DB load,
no re-analysis). These two tools replace writing a new `find_xxx.java` for each investigation.

> ⚠️ All RVAs are `er_base`-relative for **one specific ERR build**. After a game/ERR patch, regenerate
> `rtti_index.txt` (RVAs shift). Header of the file records the imagebase it was built against.

## 1. `rtti_index.txt` — grep-able class map (regenerate with `rtti_index.java`)
TSV: `vtable_rva  td_rva  ctor_rvas  mangled_name`. 9760 classes / 10202 vtables. Grep it instead of
re-deriving RTTI by hand:
```
grep 'MapIns@CS@@$'            tools/ghidra/rtti_index.txt   # exact class -> vtable + ctors
grep -i 'worldgeom'           tools/ghidra/rtti_index.txt   # fuzzy class search
grep $'\t0x6c5900'            tools/ghidra/rtti_index.txt   # which class has ctor @ RVA 0x6c5900
```
Built by `rtti_index.java` (finds COLs via the x64 self-RVA trick, then one byte-pass matches vtables —
no per-class memory scan). Regenerate:
```
analyzeHeadless.bat D:\ghidra_proj2 ER -process eldenring.exe -noanalysis \
  -scriptPath D:\ghidra_scripts -postScript rtti_index.java
```

## 2. `query.java` — parametrized on-demand decompile (no per-RE script)
```
analyzeHeadless.bat D:\ghidra_proj2 ER -process eldenring.exe -noanalysis \
  -scriptPath D:\ghidra_scripts -postScript query.java <target> [<target> ...]
```
`<target>`:
- `0x<hex>` — address (`>= imagebase` = VA, else RVA): decompiles the function + entry AOB +
  rip-relative static-global accesses + callers.
- `name:<substr>` (or a bare token) — RTTI mangled-name substring: walks every matching TypeDescriptor
  → vtable → vmethods + ctors (decompiled). Use `rtti_index.txt` to get the exact name first.

Output → `D:\ghidra_scripts\out_query.txt` (UTF-8; headless logging mangles multi-line, so it writes a
file). Examples:
```
... query.java name:CSWorldGeomStaticIns@CS@@        # class: vtable + ctors decompiled
... query.java 0x71a0f0 0x6c5900                     # decompile two functions by RVA
```

## Workflow (the fast path for a new RE)
1. `grep` `rtti_index.txt` for the class(es) you care about → vtable RVA + ctor RVAs.
2. `query.java name:<exact>` or `query.java 0x<ctorRVA>` → read the decompiled ctor/methods.
3. Only write a bespoke script when you need a custom scan/iteration the two tools don't cover.

Sources live in `D:\ghidra_scripts\` (run location) and are mirrored here for versioning.
See `docs/re/` for the findings these tools produced.
