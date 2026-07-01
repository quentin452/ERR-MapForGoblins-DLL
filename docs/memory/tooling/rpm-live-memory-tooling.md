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

**EventFlag bit-array layout — read ANY live event flag via RPM (RE'd 2026-07-01, `D:\ghidra_scripts\read_event_flags.py`).** Resolve `EventFlagMan` via the `EVENT_FLAG_MAN_SLOT` AOB (`48 8B 3D ?? ?? ?? ?? 48 85 FF ?? ?? 32 C0 E9`, target = rip-disp @ match+3, insn len 7). Then: `mgr = *slot`; `mgr+0x1c` = divisor (1000); `mgr+0x20` = block size bytes (125 = 1000 bits); **`mgr+0x28` = FLAT BIT ARRAY base**; test flag `id`: `group=id/1000, sub=id%1000; byte = base + group*125 + sub/8; bit = 7-(sub%8)` (MSB-first, same order as the save file). GOTCHA that cost a false start: `mgr+0x38` is a `std::map<group,…>` that the MapForGoblins DLL walks for loot-flag PERSISTENCE (is the group node allocated?) — it is NOT the bit-value store, and its node value at +0x24/+0x28 is small metadata (`{0x1cb,1,0x1cb,group}`), so treating it as a bit-block pointer reads garbage that can coincidentally pass a 2-bit AlwaysOn/Off calibration. Always validate a flag read by MUTUAL EXCLUSIVITY of a known state register, not just 6001/6000. Validated: 6001=ON, 6000=OFF, and the quest state registers each show exactly one flag per sub-register (Boc `[3940,3945]`, Thops `[3800,3805]`, Alexander `[3660,3666]` — matches the EMEVD state machines in [[quest-browser]]). This is the game's OWN quest "resolver" output: ER has no quest-manager/journal object; the resolver = the EMEVD event VM writing these flags, and `IsEventFlag` reads this same array (which is why the mod's `read_event_flag` already consumes it correctly).

**RPM alone can settle struct/hook questions — no CE needed** (2026-06-30, native msg getter RE,
see `docs/re/windows_native_msg_getter_re_findings.md` + [[ghidra-re-tooling]]): read a live singleton
→ walk its fields to answer a "does the engine merge X internally?" question read-only (here:
`MsgRepositoryImp` `groupCount@+0x10 == 1`, base FMG slots already hold merged DLC, vanilla DLC slots
are 1-string stubs). Also: **detect hooks by RPM-reading a function's entry bytes** — a leading
`E9 <rel32>` (jmp to a trampoline just below the module base) = the function is MinHook'd (here ERR
hooks the message getter). Consequence: AOBs for hookable functions must anchor on the **interior**,
not the prologue (entry = interior_match − 5).
