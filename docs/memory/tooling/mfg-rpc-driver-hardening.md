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
   - **★ `status` now carries `frame=<N>` (present-thread heartbeat, 2026-07-06).** The cheap,
     non-blocking freeze test: read `status`, wait ~2 s, read again — if `frame=` is UNCHANGED while
     the process is still alive, the render/main thread is deadlocked (the RPC listener is a separate
     thread, so `ping`/`status` still answer). Prefer this over the screenshot probe (which itself
     hangs on a freeze). The in-DLL `freeze_watchdog` (default 20 s, ini `[Debug] freeze_watchdog_secs`)
     ALSO auto-writes `MapForGoblins_freeze_<pid>.{txt,dmp}` on a present stall — but only that class
     (present frozen); a freeze where Present keeps running (game-update/input stuck) advances `frame=`
     and is NOT caught by either. NB a D-state (uninterruptible) frozen game can also hang `pgrep`/`ps`
     reading its `/proc` — force-kill with `pkill -9` in the background so cleanup doesn't block you.
5. **DLL-FRESHNESS gate BEFORE verifying new code (user-reported 2026-07-04).** `ping`/`status`
   answer from a **STALE DLL** too — the listener lives, but the running DLL predates your source
   changes, so your new verbs return `err unknown command` and old behaviour silently persists
   (wasted a round-trip talking to an old build sitting in the main menu). **Before RPC-testing
   anything you just added:**
   - `timeout 15 python tools/mfg.py rpc mfg_build` → the running DLL's compile time. If it isn't
     from your latest build, the DLL is stale. (Any new verb edits `goblin_debug_rpc.cpp`, so its
     `__TIME__` advances every time the RPC surface changes.) Fallback check: probe your **newest
     verb** — `err unknown command` ⇒ stale.
   - A stale DLL means: **rebuild → redeploy → RESTART the game** (or hot-reload). A DLL loads at
     game boot; a redeploy WITHOUT a restart keeps the old DLL resident. Reconnecting the RPC does
     NOT reload it.
   - Also confirm you're actually **in-world** (`status map_open=`, real player state), not the main
     menu — `fg=1` only means the window is focused, not that a save is loaded.

Related: the `+0x1EB9999` render race itself is pre-existing and documented in
[[er-shutdown-crash-noise]] / [[mapforgoblins-map-open-freeze]]; heavy scripted map open/close
cycling seems to tickle it (2 dumps in one evening of RPC loops, 18:27 + 18:43 on 2026-07-02).

## Driving the F1 panel in tests — prefer STATE-RPCs over pixel-clicks (2026-07-04)

Pixel `mouse_click <x> <y>` on F1 widgets (esp. the TAB BAR) is flaky — clicks land on the wrong tab
(the AZERTY/refocus/warp-race quirks below), so a scripted verify that clicks tabs is unreliable. Drive
the panel by STATE instead — deterministic, no coordinate guessing, no focus race:

- **`f1_tab <Markers|Search|Quests|Display|Dev|0..4>`** — force-selects a tab (opens the panel first;
  ImGui `SetSelected`, one-shot). Use this to reach a tab before screenshotting, NOT a pixel-click.
- **`open_f1 0|1|toggle`**, **`vmap 0|1|toggle`** (virtual world map), **`pause 0|1|toggle`** — panel /
  window / pause state directly.
- **`set <ini_key> <val>`** for any config bool/float; **`param_setf`/`param_getf`**, the world-editor
  verbs, `goods_count`, etc. for feature state.
- Rule of thumb: if you need to verify a UI action, add a small state-RPC for it (the pattern above)
  rather than pixel-clicking. Only fall back to `mouse_click`/`mouse_drag`/`mouse_wheel` for things with
  NO state backing (e.g. exercising the virtual-map canvas pan/zoom itself).

## RPC auto-idle — scripted vs. human input arbitration (2026-07-03, `feat/rpc-auto-idle`)

The RPC input commands (`key`/`type`/`mouse_move`/`mouse_click`/`mouse_drag`/`mouse_wheel`) now
SUSPEND themselves while the human is actively driving, so scripted SendInput can't fight the user's
own kb/mouse. Gated on `[Debug] rpc_auto_idle` (default true; the 1500ms user-active window + 700ms
per-injection guard are hardcoded calibration). Driver implications:

- **⚠ FALSE-POSITIVE FIX (2026-07-05):** headless/bg scripted input used to false-suspend right after
  `load_save` — two causes, both fixed. (1) The per-injection guard was 300ms but our own key's WM_INPUT
  raw echo lands ~390ms later under Proton → escaped the guard → counted as user activity → the next
  scripted key within 1500ms was suspended (a burst stalled; `load_save` only survived via its ~3s
  inter-key sleeps). Guard bumped 300→700ms. (2) **Mouse MOVEMENT no longer moves the idle clock** —
  ~151 phantom WM_MOUSEMOVE per headless boot (our recenter + Proton/Wine) were the real test-breaker;
  only genuine key/click now suspends. Diagnose with the new **`idlediag`** verb (per-source clock-mover
  tally). Verified: a 6× rapid `key` burst right after `load_save` = 0/6 suspended (was 6/6).

- **`status` fields:** `user_idle_ms=` (age of the last real user input; 99999 = none/idle) and
  `rpc_input_idle=` (1 while input injection is suspended). Non-input RPC — status, screenshot, set,
  reload, pause, open_f1 — is NEVER gated, so a driver can always read state.
- **A suspended input command returns `idle user active (rpc input suspended; poll status
  rpc_input_idle=)`** and does nothing (not even a focus-steal). Treat that reply as "retry later",
  not an error. `mfg_rpc.py` has `wait_idle()` / the `wait-idle` subcommand to block until
  `rpc_input_idle=0` before an input burst.
- **Our own injection does NOT self-idle:** each RPC SendInput arms a guard window so its WM echo
  through hk_wndproc isn't miscounted as user activity — a scripted `type`/`key`/drag runs to
  completion. The mechanism (`mark_rpc_injection` / `note_user_input`, `g_last_user_input_tick`)
  lives in `src/input/input_wndproc.cpp`; the gate + status in `src/goblin_debug_rpc.cpp`.
- **Interaction with the `g_has_focus` background blocker (Phase 4):** these are complementary —
  focus-loss already kills the game's own input path when the window is backgrounded; auto-idle adds
  the same-window case (user grabs kb/mouse while the game is FOCUSED). Set `rpc_auto_idle=false` if a
  script must drive input regardless of user activity (accepts the fight).

## PREREQ — Steam must already be running (2026-07-03, corrects the header below)

me3's launcher aborts with `ERROR require_steam: Steam is required to run this game` when the Steam
client isn't up — the game never boots and `GameSession` fails at `me3 exited before the RPC came up`.
Fix: start Steam headless ONCE (`steam -silent >…/steam.log 2>&1 &`); with `AllowAutoLogin=1` +
`RememberPassword=1` it auto-logs-in online (watch `~/.local/share/Steam/logs/connection_log.txt` for
`RecvMsgClientLogOnResponse() : 'OK'`), daemonizes, and **survives across Bash tool calls** (it
reparents itself — not reaped like a detached me3). After that a **background Claude job CAN** run the
foreground one-shot below to boot ER, drive RPC, and kill the game. So the "CANNOT launch" header is
only true when Steam is down; with Steam up it's a solved, repeatable flow (goods_count offsets were
live-verified this way 2026-07-03, `tools/rpc_tests/test_goods_count.py` 6/6).

## GOTCHA — game won't stay alive if launched DETACHED from a background Claude job (2026-07-03)

Verified while trying to auto-run a Slice-1 in-game RPC smoke test from a **background** Claude
session: the game will not stay alive when launched DETACHED (setsid/disown/run_in_background) from
the job's Bash tool — but the foreground in-shell-child workaround below works (and needs Steam up,
per the PREREQ above).

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
- **THE WORKAROUND — one FOREGROUND blocking command (VERIFIED 2026-07-03).** The reaping only hits
  a DETACHED child (setsid/disown/`run_in_background` reparent me3 to init → the job's cleanup kills
  it). If instead you launch me3 as an in-shell background child (`me3 launch … &`) inside a SINGLE
  **foreground** Bash tool call (a generous `timeout`, e.g. 300000 ms) and do the WHOLE recipe —
  poll `ping`, run the RPC test, then `kill $ME3` — before the command returns, the parent shell
  stays alive the whole time so me3 is NOT reaped. Proven: ER booted, RPC up at ~28s, full param
  round-trip ran, then clean kill. Pattern:
  ```bash
  cd <ERR>/internals/modengine
  ./bin/me3 launch -g eldenring -e "<Game/eldenring.exe>" -p err_offline.me3 >me3.log 2>&1 &
  ME3=$!                          # in-shell child of the still-running foreground shell
  for i in $(seq 1 55); do kill -0 $ME3 || break; r=$(timeout 8 python tools/mfg_rpc.py --port 38700 ping); [[ $r == *pong* ]] && break; sleep 4; done
  # ... run the RPC commands (params live at title — no save needed) ...
  kill $ME3; pkill -f "Game/eldenring.exe"     # MUST kill before returning, else the game outlives the run
  ```
  **GOTCHA (2026-07-03): `pkill -f "Game/eldenring.exe"` matches the DRIVER SHELL'S OWN args** (the
  launch command line contains that path) → pkill kills your own script mid-run (exit 144, any output
  after it is lost). Either `pkill -f "eldenring.exe" | grep -v` won't help (pkill has no such filter) —
  use `kill $ME3` alone, or `pgrep -f Game/eldenring.exe | grep -v $$` then kill those PIDs, or just
  accept it and read the PERSISTED log afterward (the game's log survives the shell death). Also: a
  **two-boot** recipe blows the default **120 s** Bash-tool timeout — split into separate tool calls or
  pass an explicit longer `timeout`.
  **PREFER the scripting framework over hand-rolled bash (added 2026-07-03):**
  `tools/mfg_session.py` wraps this whole dance (launch me3 child → poll `ping` → title→in-world nav →
  rpc + assertions → `pkill -x eldenring.exe` on exit) behind a `GameSession` context manager, with all
  the gotchas baked in (per-call timeouts, exact-name kill, sidecar-path bind wait). A repro/regression
  is now a few lines of Python instead of 40 lines of bash:
  ```python
  from mfg_session import GameSession
  with GameSession() as g:
      g.load_save(); g.warp(11102950)
      g.assert_in("sidecar getkv foo", "foo=bar")
  ```
  Regression tests live in `tools/rpc_tests/test_*.py` (each = one standalone boot via `run_test`);
  `python3 tools/mfg_test.py [filter]` runs them all + reports pass/fail. Proven 2026-07-03: `test_warp`
  4/4, `test_sidecar` 5/5, `test_goods_count` 6/6, `test_custom_item` 4/4 (Variant A clean-save). Runs
  inside ONE foreground call (me3 is the script's child, killed at `__exit__` before return — the
  reaping constraint still holds). Hand bash only for one-off pokes.

  **Front door: `tools/mfg.py` (or `tools/mfg`) — one CLI over all of the above (added 2026-07-03):**
  - `mfg rpc <cmd> [args]` — one-shot RPC against a RUNNING game (`mfg rpc goods_count 0x40003bed`).
  - `mfg repl [--boot]` — interactive RPC shell (readline history). Attach to a running game, or
    `--boot` to cold-boot ER + load a save for the session.
  - `mfg run <script> [--boot]` — replay a saved command script (one RPC line/row, `#` comments);
    sample `tools/rpc_scripts/smoke.mfgs`. Prints per-line ok/err + an `N/N ok` tally.
  - `mfg test [name...]` — the regression suite (delegates to `mfg_test`; each test cold-boots).
  Port = `--port` / `$MFG_RPC_PORT` / 38700. `--boot` needs Steam already running (PREREQ above).
  Attach mode (no `--boot`) is the fast iterate path once a game is up; it gives a clear "no listener"
  hint instead of a stack trace when nothing's running.

  This machine's exact pieces (verified 2026-07-03): me3 = `~/Games/ERRv2.2.9.6/internals/modengine/bin/me3`,
  exe = `~/.local/share/Steam/steamapps/common/ELDEN RING/Game/eldenring.exe`, profile `err_offline.me3`.
  RPC comes up at ~24s. Screenshots: pass a `Z:\...` path (game's view; `Z:` = `/`).
- **LOAD A SAVE from a cold boot (needed for inventory/world/sidecar tests; VERIFIED 2026-07-03).**
  Some state only exists in-world — e.g. the **save FILE is not opened at the title screen** (a
  `sidecar status` there returns `path=(none)`; ER opens `ER0000.err` only on actual character load).
  Nav that works blind from the title: RPC `key Return` (dismiss the "press any button" splash) →
  `key e` (confirm the default-highlighted **Continue**) → `key Return` → in-world in ~4s. Poll for the
  world (e.g. `sidecar status` path flips off `(none)`, or `status map_open` after `key M`). `key e` is
  this install's confirm bind (AZERTY); VK-only send (the driver already handles the AZERTY scancode
  gotcha). This closed the full sidecar Phase-1 round-trip in one automated foreground run.
  Trade-off: the command BLOCKS for the whole boot+test (~30-120s) and the game is KILLED at the end
  (a foreground tool call can't leave a child running — returning reaps it). So this is for
  self-contained verify runs, NOT for leaving a game up for interactive iteration. For a long-lived
  session the user still launches ER themselves.
- **Consequence / workflow:** in-game RPC verification either uses the foreground one-shot above, or
  is driven against a **user-launched** game. Params are live at the title screen (no save — see
  [[../linux]]). The agent can build, deploy (atomic), and drive `mfg_rpc.py`; it just can't keep a
  detached game process alive across tool calls.
- Deploy note: redeploy AFTER the last rebuild — it's easy to `cp` the DLL before adding the RPC
  command you're about to test (the deployed binary then rejects `param_get` as "unknown command").
  Sanity-check with `grep -ac param_set <deployed dll>`.
