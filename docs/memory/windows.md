# Windows Agent Notes

Windows does normal repo work too; its special role is **runtime RE** with the live game and Windows-only
tooling. Prefer Windows for anything that needs a running `eldenring.exe`.

## ⚠ Two standing Windows rules (user, 2026-07-06)

1. **The Windows deployed DLL is routinely STALE vs Linux.** Linux is the daily-built, daily-played dev
   box; the Windows install's `MapForGoblins.dll` can lag it by days/commits. So on Windows **never assume
   the running DLL has recent code** — check `python tools/mfg.py rpc mfg_build` (built= time) before
   RPC-verifying anything, and expect that any NEW verb you need is likely absent until you rebuild →
   redeploy → **restart** the game. (Observed 2026-07-06: exe up + in-world, but DLL was `Jul 5`.)
2. **On Windows, runtime RE = EXTERNAL RPM (`ReadProcessMemory`), not the in-DLL boot flow.** The
   `mfg.py --boot` / `GameSession` automation is **Linux/Proton-hardcoded** (me3 CLI paths, wine prefix,
   `pkill -x`) and does NOT boot/kill the game on Windows — the user launches ER here. Drive raw memory
   RE with the external RPM scanner path (`tooling/rpm-live-memory-tooling.md`; RTTI→heap→match, e.g.
   `scratchpad/GfxScan.cs`), attaching to the user-launched `eldenring.exe`.

## Live-verify on Windows (attach-mode RPC — the full loop works, incl. WRITE verbs)

The `--boot` automation is Linux-only, but the DLL's RPC listener is **TCP loopback → cross-platform**
(`tools/mfg_rpc.py` docstring: "from Windows alike"). So the attach path drives the LIVE game here for
ANY verb — `warp_local`/`param_set`/`give_item`/`move_asset`, not just read-only ones (the "read-only"
caveat was about not being able to auto-boot, NOT about verb capability). The only real Windows gaps:
auto-boot/kill (me3) and the cold-boot `mfg test` suite (each test reboots). Everything else verifies.

The loop (game must be CLOSED to swap the DLL — the loaded one is file-locked + a restart is needed to
load a new build anyway):
1. **Set `ERR_ROOT`** once in `.env.local` (gitignored) = the ERR install the user launches (the folder
   with `dll/offline/`, `internals/modengine/`, `ReforgedLauncher.exe`). `DLL_OFFLINE` resolves from it.
2. **Build**: `build.bat` (or `ninja -C build-err MapForGoblins`).
3. **Deploy**: `python tools/deploy.py` (picks `build-err` on Windows; refuses while ER is running).
4. **User launches ER** (ReforgedLauncher → in-world, load a save). The ini already has
   `[Debug] debug_rpc_port = 38700`.
5. **Freshness + drive**: `python tools/mfg.py rpc mfg_build` (confirm YOUR build stamp), `status`
   (in-world, not menu), then the verb under test, e.g. `mfg.py rpc coords` / `mfg.py rpc warp_local <x> <y> <z>`.
   `mfg.py repl` (NO `--boot`) gives an interactive shell against the running game.

Raw memory / find-what-accesses RE still prefers external RPM; but for verifying a shipped verb's
behavior (does the teleport move the player? does the param edit show?) attach-RPC is the tool.

## Windows-only / Windows-preferred

- Ghidra (project reuse + `query.java`/`rtti_index`). → `tooling/ghidra-re-tooling.md`, `tooling/ghidra-worldmap-re.md`
- Cheat Engine / runtime validation against the live game.
- External Python `ReadProcessMemory` probes vs a running `eldenring.exe`. → `tooling/rpm-live-memory-tooling.md`
- Oodle-only extraction: DarkScript3 EMEVD/ESD, FFDEC, the pythonnet/.NET data pipeline, packed-file work.
  → `tooling/darkscript3-emevd-decompile.md`, `tooling/mapforgoblins-pipeline-setup.md`
- ER Console mod as a coordinate-readout tool. → `tooling/er-console-mod.md`

## Dev-box quirks

- Invoke `.bat` via the PowerShell tool (Bash `cmd.exe` only prints the banner).
- Each tool call gets a copy-on-write FS snapshot — create dirs in the same call / via Write.
- Custom redirects go stale; read the background task's own output. Keep big artifacts on `D:\`.
- Pass env-var paths with forward slashes. → `tooling/windows-tooling-gotchas.md`

## How to answer an RE prompt

1. Read the prompt in `docs/re/windows_*_prompt.md`.
2. Check current code and existing findings first (offsets are resolved live at init via
   `re_signatures.hpp` + `resolve_field_offset` — see `tooling/param-offset-source-of-truth.md`).
3. Prefer the reusable Ghidra/RPM helpers over one-off scripts.
4. Validate offsets with the 4-check recipe (`tooling/re-offset-validation.md`) — never ship one
   hand-derived from paramdef packing.
5. Return a findings doc in `docs/re/*_findings.md` with concrete offsets, AOBs, confidence, runtime
   evidence, and implementation notes. Mark failed paths explicitly so they aren't retried.

Windows is not the only place to change code: pure C++/Python/docs work that needs no live runtime RE
can be done on either platform.
