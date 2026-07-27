---
name: boot-crash-present-chain-d3d11
description: "Intermittent crash ~4s after launch, 0xC0000005 executing d3d11.dll, with our hk_present in the stack scan. We never touch D3D11 (link d3d12+dxgi only) — the faulting code is another overlay's. Disabling RTSS stopped it, on a still-small sample."
metadata:
  node_type: memory
  type: project
---

# Boot crash in the Present chain — fault inside d3d11.dll (RTSS suspected)

**Symptom (user-reported 2026-07-27).** With the mod installed the game *can* crash a few seconds after
launch. Intermittent: ~9 boots that day (log archive timestamps), **1** crash file.

## The one sample

`logs/MapForGoblins_crash_14400.txt`, boot 11:32:36 → crash 11:32:40:

```
exception_code = 0xC0000005            (access violation)
fault_module   = C:\Windows\SYSTEM32\d3d11.dll  (+0x80FC0)
[+0x00800] MapForGoblins.dll +0x199C05  ; hk_present (src/goblin_overlay.cpp:1338)
```

## What it is NOT

- **Not [[atlas-upload-gpu-race]]**, despite also being an intermittent boot crash with `hk_present` in
  the stack. That one is a **null deref** (`fault_address = 0x0`, `fault_base +0x0`); this faults while
  *executing valid code* inside d3d11. Different class — don't re-apply that note's conclusions here.
- **Not our code.** `src/` contains **no** reference to d3d11 and CMake links only `d3d12` + `dxgi`
  (CMakeLists ~369). The faulting instruction cannot be ours nor called directly by us. This is the
  single most useful fact for a future session: it rules out the whole "our renderer crashed" branch.
- **Probably not the grace-sprite GPU harvest** either, though it is the nearest suspicious thing in the
  log: `[GRACE-SRV] copied … -> slot 2` **succeeded** 840 ms before the crash, with a `[DVDBND]` line in
  between. (It does drive `g_frames[0].allocator` / `g_command_list` — worth re-checking if a future
  sample lands closer to it, but it is D3D12, not D3D11.)

## Reading

Our `hk_present` is in the stack because **we sit in the Present chain**, and that process had six
hooking/overlay DLLs: RTSSHooks64, EROverlay, PostureBarMod, ER-DeathCounter, Boss.dll, plus the Steam
overlay — several of which draw with D3D11. Most likely another overlay's D3D11 path faulted while the
chain ran through us. See [[multimod-hook-coexistence]] for the class.

**Status 2026-07-27: user disabled RTSS (RivaTuner) → no crash since.** Treat as a LEAD, not a verdict:
the bug is intermittent (1 in ~9 boots), so a handful of clean boots is not proof. Do not close it, and
do not "fix" anything on our side from this alone.

## If it recurs

1. Keep every `MapForGoblins_crash_<pid>.txt`. Two or three samples tell you whether the faulting module
   and offset are STABLE (a dead pointer) or wander (a race) — that distinction is worth more than any
   amount of static reading.
2. Bisect `external_dlls` down to MapForGoblins alone, then add the others back one at a time.
3. Symbolize with `python tools/resolve_crash.py <crash.txt> --dll build-err/MapForGoblins.dll`.

## Gotcha — a MISMATCHED pdb symbolizes silently

The deployed pair went out of sync (`DLLS/MapForGoblins.dll` 11:31 vs `DLLS/MapForGoblins.pdb` 11:13)
and llvm-symbolizer still returned a plausible symbol instead of failing. **Always symbolize against the
DLL+PDB pair of the build that actually ran** (`build-err/` right after a build, or `pdb-archive/<tag>/`
for a released one) and check the two mtimes match. A confident wrong frame is worse than no frame.

## Stack-scan caveat

The in-process handler does **not** unwind — it scans the stack for plausible return addresses. Order is
indicative, and any single frame (ours included) can be a stale leftover value. Never build a call-chain
argument on it alone.
