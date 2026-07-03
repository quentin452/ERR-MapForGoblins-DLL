---
name: mfg-rpc-driver-hardening
description: "RPC driver scripts MUST be freeze-proof: the game can freeze with the DLL (and the RPC listener) still alive, so liveness-gated waits + per-call timeouts are mandatory — otherwise background validation scripts spin forever on a gate that never comes (user-reported 2026-07-02)."
metadata:
  node_type: memory
  type: tooling
---

User-reported failure mode (2026-07-02): during a scripted validation run the game FROZE
completely (the known `eldenring.exe +0x1EB9999` in-session render race — crash dump, zero
MapForGoblins frames) while the DLL and its RPC listener stayed ALIVE. The driving background
script kept waiting on conditions that could never happen (`ping` still answered; `screenshot`
commands queue on the present thread, which was dead), and it kept spinning even after the user
exited the game. Anyone watching the task output was blocked indefinitely.

Mandatory pattern for every RPC driver / background validation script:

1. **Per-call timeout.** Wrap every `mfg_rpc.py` invocation in `timeout 15` — a frozen present
   thread means queued commands never pump and the socket read hangs. (`ping` is answered by
   the LISTENER thread, so it is NOT a liveness signal for the game.)
2. **Game-liveness gate in every wait loop.** Poll loops must also bail when the process dies:
   `ps -eo args | grep -q "Game/eldenring.exe" || break` (NB `pgrep -f eldenring.exe` matches
   the me3 command line AND the caller's own bash wrapper — filter `grep -v "bash -c"`, or
   match the full `Game/eldenring.exe` path via `ps`).
3. **Whole-script cap.** Run the entire recipe under `timeout 600 bash -c '…'` so a stuck run
   self-terminates instead of outliving the game.
4. **Present-thread liveness check** where it matters: a `screenshot` that doesn't return
   within its timeout = the game is frozen even though `ping` works → abort the recipe, report
   "game frozen (present thread dead)", don't keep sending inputs.

Related: the `+0x1EB9999` render race itself is pre-existing and documented in
[[er-shutdown-crash-noise]] / [[mapforgoblins-map-open-freeze]]; heavy scripted map open/close
cycling seems to tickle it (2 dumps in one evening of RPC loops, 18:27 + 18:43 on 2026-07-02).

## GOTCHA — you CANNOT launch/keep the game alive from a background Claude job (2026-07-03)

Verified while trying to auto-run a Slice-1 in-game RPC smoke test from a **background** Claude
session: the game will not stay alive when launched from the job's Bash tool.

- **The bg-job sandbox reaps the spawned process TREE.** Every game-launch command returns **exit
  144** (this env's "spawned tree got reaped" code) and the game dies WITH it — `setsid` / `nohup` /
  `disown` / `dangerouslyDisableSandbox` / `run_in_background` all still get reaped. Observed: ER
  booted far enough to load our DLL (`[PROFILE] ERR DETECTED` → `Waiting for params...`) then was
  torn down at param-wait — BEFORE `enable_hooks` starts the RPC listener, so `ping` never answers
  (port 38700 not listening) and the whole smoke test fails at the `ping` gate.
- **The interactive ERR launcher can't run headless either:** `4 - Launch … Offline … (Linux).sh`
  → `ReforgedLauncher --offline` uses a PromptPlus TUI → `PromptPlus requires a terminal/console
  environment!` and exits. The me3 CLI (`internals/modengine/bin/me3 launch -g eldenring -e
  <Game/eldenring.exe> -p err_offline.me3`, per [[me3-cli-nonerr-launch]]) IS the headless path and
  DOES start booting — but it's still reaped by the bg job as above.
- **Consequence / workflow:** in-game RPC verification must be driven against a **user-launched**
  game. The user starts ER their normal way (a real terminal), reaches a state where params are
  live (title screen is enough — see [[../linux]]), THEN the agent runs the RPC driver against the
  already-running process. The agent CAN do everything except keep the process alive: build, deploy
  (atomic), and drive `mfg_rpc.py` once something else owns the game process.
- Deploy note: redeploy AFTER the last rebuild — it's easy to `cp` the DLL before adding the RPC
  command you're about to test (the deployed binary then rejects `param_get` as "unknown command").
  Sanity-check with `grep -ac param_set <deployed dll>`.
