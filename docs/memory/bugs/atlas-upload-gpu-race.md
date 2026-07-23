# Atlas-upload GPU-init race at boot (DEFERRED — intermittent)

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

**Deferred fix ideas (when picked up):**
- Gate `try_upload_atlas` on a real "device + command queue + backbuffer ready" check (not just first
  present); retry next frame if not ready instead of uploading.
- Null-check `g_command_list` / `g_device` / `*out_tex` in `upload_rgba` before use; bail + retry.
- Confirm the atlas upload only runs AFTER the swapchain/device init hook completed (order-of-init).

**Repro:** cold-boot repeatedly; ~1 in several boots. Look for `MapForGoblins_crash_*.txt` with
`fault_base +0x0` and the `upload_rgba <- try_upload_atlas <- hk_present` stack.
