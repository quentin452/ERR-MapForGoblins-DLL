---
name: clang-cl-seh-noinline
description: clang-cl ELIDES __try around a raw LOAD (even noinline) → 0xC0000005. Two safe fixes — ReadProcessMemory (kernel, can't elide, but Wine-IPC slow) OR __try around a noinline CALL (raw_copy/raw_store8) which clang-cl PRESERVES → lets you drop RPM for free in-process reads
metadata: 
  node_type: memory
  type: project
---

**UPDATE 2026-06-27 — the "RPM is the only fix" conclusion is SUPERSEDED.** clang-cl
elides `__try` only when it wraps the **raw load/store directly** (it "proves" a plain
deref can't throw). It does NOT elide `__try` around a **CALL** — it can't prove an
opaque call won't fault, so it keeps the SEH frame. So the working pattern that lets you
DROP ReadProcessMemory entirely (we're injected in-process → a raw deref is free, no
wineserver IPC):
```c
__declspec(noinline) static void raw_copy(void *d, const void *s, size_t n){ memcpy(d,s,n); }
__declspec(noinline) static void raw_store8(uint8_t *d, uint8_t v){ *d = v; }
static bool safe_read(void *a, void *o, size_t n){ __try{ raw_copy(o,a,n); return true; } __except(EXCEPTION_EXECUTE_HANDLER){ return false; } }
```
The faulting access lives in the noinline helper; `__try` wraps the CALL, not the load.
SHIPPED in `goblin_collected.cpp` safe_read/safe_write_byte (commit 85cece4, merge
83a723f) → killed the Proton RPM-IPC flood = the constant ~20fps collected-refresh
stutter (see [[collected-geof-bruteforce-scan]]; read_wgm 581→4ms). WHY this beats RPM:
RPM is a wineserver round-trip (~10µs) on Wine; the in-process deref is ~ns. RPM stays
the right tool ONLY where you can't be sure you're in the target process. Residual risk
to watch: prove the SEH actually catches by forcing a faulting read (cross unloaded
tiles, eviction churn) — a crash on a freed-block deref = the pattern regressed.

**clang-cl SEH gotcha (hit 2026-06-20).** The cross build (clang-cl + lld-link, [[mapforgoblins-linux-build]]) handles `__try/__except` LESS robustly than MSVC: when a tiny `seh_readN(ptr)` helper is INLINED into a caller, clang merges/hoists the raw load OUT of the `__try` region and the SEH guard is silently lost — a bad pointer then faults UNHANDLED.

Observed: a `0xC0000005` crash in goblin_worldmap_probe at the probe's WorldMapArea read (`movl 0x378(%rbx)` = view+0x378 pan) on DLC map entry, where `cursor+0xF0` (view) was mid-transition garbage. Disasm showed the seh_read calls inlined to direct `movl`s with no SEH frame → the `__try` never caught it. (Decoded the stripped DLL by disassembling at imagebase 0x180000000 + crash RVA from MapForGoblins_crash_320.txt.)

**FIX: mark every `seh_read*` helper `__declspec(noinline)`** so each `__try` stays a self-contained real call. Belt-and-suspenders: also a `plausible_ptr(p)` gate (p>=0x10000 && p<0x7fffffffffff && 8-aligned) before dereferencing any read-from-memory pointer (e.g. cursor+0xF0). Applied in goblin_worldmap_probe.cpp (seh_read4/seh_read8/seh_read_i32).

**Project-wide caution:** other SEH read-helpers (goblin_inject probe_map_pos_seh, goblin_collected, marker dump) may have the same latent risk — if any starts crashing on a guarded read, suspect inlining first → add noinline. Especially risky for code that runs FREQUENTLY during object transitions (the probe at 100ms tick hit it; once-per-open code rarely catches the transition window).

**RECURRENCE (2026-06-20, 912a83c):** same trap bit `scan_region` / `scan_region_all` (the cursor vtable memory scanner) — they hold `__try` but were NOT noinline. Latent while the scan was gated on map-open (stable memory); when a refactor made the scan run gate-INDEPENDENT at startup/loading, it faulted on volatile regions UNCAUGHT → 0xC0000005 at DLL+0x5A920 on the first probe tick.

**ROOT CAUSE FOUND — noinline is NOT enough (2026-06-20, 1fc4b01).** Decoded DLL+0x5A920 (imagebase 0x180000000 → VMA 0x18005a920): the faulting code is a 3-insn leaf `movq (%rcx),%rax; movq %rax,(%rdx); ret` with 12 probe call sites — i.e. clang-cl compiled `seh_read8` (which IS `__declspec(noinline)`) to a bare `*out=*src` leaf with NO __try and inlined the `return true` into callers. **clang-cl ELIDES the __try/__except even inside a noinline function** when it "proves" the plain (even `volatile`) load can't throw. So the SEH guard was a lie; every "guarded" read could fault. **THE FIX: don't rely on __try at all — read via `ReadProcessMemory(GetCurrentProcess(), src, out, n, &got)` (returns false on bad src, never faults; opaque kernel call the optimizer can't elide).** seh_read8/4/i32 switched to RPM (1fc4b01). Slower (a syscall/read) but bulletproof; fine for the O(KB) CSMenuMan walk (the O(GB) scan was removed). RULE: for reads of game pointers that may be mid-transition/garbage, use ReadProcessMemory, not __try — clang-cl's SEH elision makes __try unreliable regardless of noinline.
