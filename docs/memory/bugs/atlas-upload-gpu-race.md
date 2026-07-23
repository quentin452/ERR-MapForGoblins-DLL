# Atlas-upload GPU-init race at boot (HARDENED 2026-07-23 — was intermittent)

**Symptom:** intermittent CRASH at cold boot (not every boot). Access violation, `fault_address = 0x0`
(null deref). Crash triage: `MapForGoblins_crash_<pid>.txt` with `fault_base +0x0`.

**Stack (symbolized via `tools/resolve_crash.py`):**
```
upload_rgba (src/goblin_overlay.cpp:343)
  <- try_upload_atlas (src/goblin_overlay.cpp:1132)
  <- hk_present (src/goblin_overlay.cpp:1702)
```
So it's the D3D12 icon-atlas upload running inside the **present hook** (`hk_present`) — the RGBA upload
(`upload_rgba`: command list + resource barrier + fence @ ~line 340-345) fires before the device / command
list / atlas resource is safely ready → null deref. A GPU-init timing race on the render thread.

**NOT related to marker/prune/build-thread changes** (those run on the disk-build worker; this is the
present hook). Hit once during the 2026-07-23 phantom-tile work (boot12); the very next boot loaded fine —
confirms it's intermittent, not a regression from that session's edits.

**Root cause (confirmed by static analysis 2026-07-23).** `upload_rgba` derefs FIVE D3D12 globals
(`g_device`, `g_command_queue`, `g_command_list`, `g_srv_heap`, `g_frames[0].allocator`), but the
`try_upload_atlas` gate only checked THREE (`g_command_queue`/`g_device`/`g_srv_heap`) + `g_imgui_init`.
`g_command_list` and `g_frames[0].allocator` were **unguarded** → null deref inside the driver (fault
attributed to line 343 `ExecuteCommandLists`, imprecise on the optimized build). Compounding it,
`cleanup_imgui_device()` nulled `g_command_list` / cleared `g_frames` / released the heaps but did NOT
reset `g_imgui_init`, so the `g_imgui_init` guard went STALE after any teardown.

**Fix applied (`goblin_overlay.cpp`, defensive — cannot regress, only adds guards + retry):**
1. `try_upload_atlas` gate now requires the FULL set (`g_command_list && !g_frames.empty() &&
   g_frames[0].allocator` added). If not ready it RETURNS WITHOUT marking `g_atlas_ready` → retries next
   frame instead of giving up to circles forever on a transient boot-timing miss.
2. `upload_rgba` bails `false` at entry if any global is null (belt-and-suspenders vs a teardown between
   the caller's check and the derefs).
3. `cleanup_imgui_device()` now sets `g_imgui_init = false` so every init-dependent path (upload + the
   per-frame draw path, which all Reset `g_frames[0].allocator`/`g_command_list`) is consistently gated;
   the next `hk_present` re-runs `init_imgui` from scratch.

**Residual (unproven) hypothesis — NOT covered by the null guards.** The command queue is captured once at
boot (`hk_execute_command_lists`, the first DIRECT queue). If the game DESTROYS that queue and makes a new
one during a boot device-reset (fullscreen transition), `g_command_queue` would DANGLE (freed, non-null →
passes the guards) → a use-after-free crash with a similar stack. If the crash ever recurs AFTER this
hardening, this is the next suspect: re-validate / re-capture the queue on device change instead of caching
it for the process lifetime.

**Repro:** cold-boot repeatedly; ~1 in several boots. Look for `MapForGoblins_crash_*.txt` with
`fault_base +0x0` and the `upload_rgba <- try_upload_atlas <- hk_present` stack.
