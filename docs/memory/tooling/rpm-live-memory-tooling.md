---
name: rpm-live-memory-tooling
description: I CAN read live eldenring.exe memory from this box via Python ctypes RPM (pairs with CE for static→runtime RE)
metadata: 
  node_type: memory
  type: reference
---

**Live process-memory reading of `eldenring.exe` works from this Windows box** via Python ctypes
(`py -3.14`) + `kernel32` ReadProcessMemory — no driver, while the game runs (ERSC/offline, EAC
bypassed). This unblocks the static→runtime RE loop that pure Ghidra kept getting wrong (see
[[player-pos-static-unreliable]]).

Reusable scripts in `<ghidra_scripts>\*.py` (all self-contained):
- **module base via Toolhelp** (`CreateToolhelp32Snapshot` MODULE) → `eldenring.exe` base is **ASLR'd**
  (e.g. `0x7FF61AB80000`, varies per launch) — resolve at runtime, NEVER assume `0x140000000`.
  RVA→VA = `base + RVA`.
- `read_frame.py` — deref a `[base+RVA]+offsets` chain and print floats (validate a pointer chain).
- `read_addrs.py` — read absolute addrs as float/int + dump a window (identify a struct's vec layout).
- `find_chain.py` / `find_chain2.py` — **reverse pointer scan**: scan all committed readable regions
  (~8–9 GB, ~1–2 min) with `bytes.find` for qwords pointing into a target struct window; classify
  static (in-module) vs heap → find the parent chain without CE's restart-filter dance.
- `read_pinmap.py` — walk an MSVC `std::map` `_Tree` (RB-tree) in-process.

**The winning RE loop this session:** static Ghidra lead → <user> does CE **"find what accesses"**
/ value-scan (gives the writer instr + struct base `RBX`) → I **RPM-verify** the chain + reverse-scan
to a static. CE find-what-accesses output (RIP + register dump) is gold: subtract module base for the
RVA, and `RBX`/`this` = the struct base. Caveat: heap addrs are session-specific (need the chain);
I can read memory but **can't see the screen / drive gameplay** — <user> still does the visual
in-game validation, and the game must be running for any RPM read.

**RPM alone can settle struct/hook questions — no CE needed** (2026-06-30, native msg getter RE,
see `docs/re/windows_native_msg_getter_re_findings.md` + [[ghidra-re-tooling]]): read a live singleton
→ walk its fields to answer a "does the engine merge X internally?" question read-only (here:
`MsgRepositoryImp` `groupCount@+0x10 == 1`, base FMG slots already hold merged DLC, vanilla DLC slots
are 1-string stubs). Also: **detect hooks by RPM-reading a function's entry bytes** — a leading
`E9 <rel32>` (jmp to a trampoline just below the module base) = the function is MinHook'd (here ERR
hooks the message getter). Consequence: AOBs for hookable functions must anchor on the **interior**,
not the prologue (entry = interior_match − 5).
