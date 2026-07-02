---
name: me3-cli-nonerr-launch
description: "How to launch vanilla / ERTE / Convergence via the ModEngine3 CLI (me3.exe) for non-ERR in-game testing — the mod-agnosticism acceptance loop"
metadata:
  node_type: memory
  type: tooling
---

To verify MapForGoblins is mod-agnostic you must run it on a **non-ERR** install in-game. This is fully
reproducible on this Windows box with the ModEngine3 CLI — no ERR launcher needed.

**me3 CLI:** `D:\DOWNLOAD\ERRv2.2.9.6-541-2-2-9-6-1780861369\ERRv2.2.9.6\internals\modengine\bin\win64\me3.exe`
(v0.11.0; the bare-vanilla profile does not need any ERR files). ERR itself wraps the same host via
`ReforgedLauncher.exe --offline`.

**Non-ERR test profiles** live under `D:\DOWNLOAD\mfg_test\{vanilla,erte,convergence}\` — each holds a
`<profile>.me3` + that dir's own `MapForGoblins.dll` + `MapForGoblins.ini` + `logs/`. The `.me3` profile
injects `MapForGoblins.dll` as a `[[natives]]` into the unmodified Steam game (vanilla = no `[[packages]]`;
game auto-detected at `D:\SteamLibrary\steamapps\common\ELDEN RING\Game`).

**Loop:**
1. Build the profile DLL: `cmake -B build-<p> ... -DGENERATED_SUBDIR=generated_<p> ...` then
   `ninja -C build-<p> MapForGoblins` (see [[build-toolchain-clang-xwin]]).
2. Copy `build-<p>/MapForGoblins.dll` → `D:\DOWNLOAD\mfg_test\<p>\MapForGoblins.dll`.
3. Launch: `me3.exe launch -g elden-ring -p D:\DOWNLOAD\mfg_test\vanilla\vanilla.me3`
   (run it backgrounded — it stays attached as the mod host; me3 host log under
   `%LOCALAPPDATA%\garyttierney\me3\data\logs\<profile>\`).
4. Read the session log at `D:\DOWNLOAD\mfg_test\<p>\logs\MapForGoblins.log`.

**Verified working on VANILLA (2026-07-01):** `ELDEN RING™ 1.16.2.0 Worldwide`, `[SIG] 29/29 clean`,
`GETMESSAGE PASS`, and the runtime English index built from vanilla's OWN engus FMGs —
`[NAMEEN] English index: 9877 names` (a different count than ERR → proves it reads the active install's
files, not ERR's baked table). No MapForGoblins errors.

**Benign exit-only crash — NOT a blocker, NOT our bug.** On GAME EXIT the crash dumper writes a
`MapForGoblins_crash_<pid>.txt` with `fault_module = me3_mod_host.dll +0xAEF4D` (zero MapForGoblins.dll
stack frames; all frames are `eldenring.exe`). It fires only when quitting, is byte-identical across runs
and to a 2026-06-24 dump (predates the runtime-English work), so it's a deterministic ModEngine3 × vanilla
teardown artifact — ignore it. Gameplay/testing during the session is unaffected. (Distinct from the
intermittent in-session `eldenring.exe +0x1EB9999` render race in [[mapforgoblins-map-open-freeze]].)

**Linux invocation (verified 2026-07-02, this repo's ERR install):**
`cd <ERR_ROOT>/internals/modengine && ./bin/me3 launch -g eldenring -e "<steam>/steamapps/common/ELDEN RING/Game/eldenring.exe" -p vanilla.me3`.
`--windows-binaries-dir` does NOT exist in me3 0.11.0 (`unexpected argument`) — the host DLL is
auto-resolved. `vanilla.me3` injects the SAME `dll/offline/MapForGoblins.dll` into the unmodified
game, so no separate DLL/ini copy is needed since the single-DLL migration (the `[PROFILE]` line
tells you which mode the DLL picked).

**GOTCHA — `me3 launch` BLOCKS until the game exits (`waitforexitandrun`).** Launching it
"headless" just to grep a boot log leaves the game window open forever and the command hung — a
human had to kill it manually (2026-07-02). If an agent launches the game only to read early
`MapForGoblins.log` lines: grab the log once the marker line appears, then KILL the game
(`pkill -f eldenring.exe`) — or better, tell the user the game window is theirs to close.
