---
name: er-console-mod
description: "Elden Ring Console mod (Nexus 9365) as a coordinate-readout tool for the player-position RE — install, toggle key, coords vs tp frames"
metadata: 
  node_type: memory
  type: reference
---

"Elden Ring Console" mod (Nexus #9365, ModEngine3 native, build Feb 2026) is installed alongside ERR as a coordinate-readout tool for the [[ghidra-worldmap-re]] player-position work.

**Install (survives ERR launcher me3 regen):** the ERR `ReforgedLauncher.exe` REGENERATES `internals/modengine/err_offline.me3` on every launch (auto-scans `dll/offline/` and adds each DLL), so editing the me3 directly gets wiped. Correct install = drop `er_console_mod.dll` into `<ERRroot>/dll/offline/` (the "PLACE OFFLINE MODE DLL MODS HERE" folder, next to MapForGoblins.dll). Source dll lives at `<windows_downloads>\erconsole_extract\natives\er_console_mod.dll`.

**Toggle key:** the console uses `GetAsyncKeyState(VK_OEM_3)` (US grave/tilde VK). On the user's **French AZERTY** keyboard VK_OEM_3 = the **`ù` key** (NOT `²`, which is VK_OEM_7). Press `ù` to open. Fallback: Win+Space → English(US) → key left of `1`.

**Commands:** `coords`, `tp <x y z>`, `tgm` (god), `tcl` (noclip), `npc <id>`, `item add`, `search`, `runes`. Reads via WorldChrMan (AOB-based, patch-resilient → works on app 2.6.2.0; D3D12/kiero ImGui overlay).

**KEY FINDING — coords vs tp use DIFFERENT coordinate frames:**
- `coords` returns small ±-range values (e.g. X=7.5 Y=-0.9 Z=6.3) = **block-local physics/Havok frame** (recentred per chunk, same as MapForGoblins `+0x70/+0x78`). NOT global.
- `tp <x y z>` takes a **different (larger) frame** (e.g. `tp 166 60 6` worked) — likely the map/chunk 0–256 frame. The two differ by the chunk re-centring offset.
- ⇒ `coords` alone does not give global; the player **tile/chunk index** is still the missing piece for Target A. Next: Ghidra the coords-read offset (WorldChrMan→player→Vec3) and inspect adjacent struct fields for the block/tile index or chunk world origin.
