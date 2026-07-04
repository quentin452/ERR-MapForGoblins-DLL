# Tool — offline minidump analysis on Linux (freeze / load-stall / crash dumps)

**Status:** active (2026-07-04). Tool: `tools/parse_minidump.py`.

The DLL watchdogs write minidumps to `logs/`:
- `MapForGoblins_freeze_<pid>.dmp` — present-thread stall (freeze watchdog).
- `MapForGoblins_load_stall_<pid>.dmp` — stuck world-load / warp infinite-loading (load watchdog).
- `MapForGoblins_crash_<pid>.dmp` — the crash handler (the `.txt` beside it is ALREADY symbolized
  for MapForGoblins.dll frames via dbghelp at capture time; the freeze/stall `.txt` is NOT — use this
  tool on their `.dmp`).

No Windows / dbghelp / minidump-stackwalk needed. `tools/parse_minidump.py <dump>` parses the raw
MINIDUMP structures (module list #4, thread list #3, x64 CONTEXT: RSP@0x98, RIP@0xF8) and, per thread,
scans the stack memory for qwords that land in a loaded module's range → probable return addresses,
printed as `ER+<rva>` (eldenring.exe) / `DLL+<rva>` (MapForGoblins.dll).

**Cross-ref:** ER imagebase in the Ghidra project is `0x140000000`, so `ER+0xb413c0` ⇔ Ghidra
`FUN_140b413c0`. DLL frames symbolize against the deployed `.pdb` (or match the crash `.txt`).

**Reading a STALL dump:** a genuine stall = ALL threads parked in `ntdll.dll` (wait states), no CPU
spin, no exception. The process main thread is the one with the lowest RSP (first/lowest stack VA).
To tell WHY a load hung, match the main/update thread's ER RVAs against the world-load class map in
`docs/re/windows_loading_screen_state_re_findings.md` (LocationStep `FUN_140b40c00`/`b413c0`, fade
`b3bfd0`, menu `CSMenuMan`) — menu-teardown frames ⇒ menu-context; streaming frames ⇒ bad/hung target.

**Faster discriminator than full symbolization:** for a warp freeze, re-run the SAME warp via the
debug RPC from gameplay (`python tools/mfg.py rpc warp <graceId>`). Works there but not from the vmap
⇒ menu-context (map open under the warp). Freezes BOTH ways ⇒ the destination itself (e.g. a DLC
area-61 grace with unmet entry state), not the UI path. See [rpc-commands](rpc-commands.md).

Related: [aob-scan-boot-race](aob-scan-boot-race.md), the load watchdog (`src/goblin_load_watchdog.cpp`).
