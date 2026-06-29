---
name: er-shutdown-crash-noise
description: "MapForGoblins_crash_*.txt at eldenring.exe +0x1EB9999 = Elden Ring's own shutdown crash on Exit/Alt+F4, not our bug — ignore"
metadata: 
  node_type: memory
  type: reference
---

When triaging `…/dll/offline/logs/MapForGoblins_crash_<pid>.txt`: a `0xC0000005` with
`fault_address = eldenring.exe +0x1EB9999` (stack ~all `eldenring.exe` frames, e.g. `+0x1EB97B8`,
`+0x4842C58`, `+0x29C8E58`; **zero MapForGoblins.dll frames**) is **Elden Ring's KNOWN
teardown crash on Exit / Alt+F4** — ER frees its own resources badly on shutdown. <user>
confirmed 2026-06-25: "ER crash tout le temps quand tu appuies sur Exit/Alt+F4".

Tells it apart from a real bug:
- It fires at SESSION END — `MapForGoblins.log` runs normally (census/flag refresh every ~100ms)
  right up to exit; the crash file's mtime is the shutdown moment.
- It is **identical across sessions and across DLL builds** (same fault RVA even though the
  MapForGoblins.dll ASLR base differs) → not tied to any code change.

**Action: ignore these crash files.** Only investigate a `MapForGoblins_crash_*.txt` whose
`fault_module` is MapForGoblins.dll or whose stack shows our DLL base. See
[[resolve-loot-flag-dlc-bug]] (where this first came up while runtime-validating the loot fix).
