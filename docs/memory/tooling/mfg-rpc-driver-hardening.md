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

## RPC auto-idle — scripted vs. human input arbitration (2026-07-03, `feat/rpc-auto-idle`)

The RPC input commands (`key`/`type`/`mouse_move`/`mouse_click`/`mouse_drag`/`mouse_wheel`) now
SUSPEND themselves while the human is actively driving, so scripted SendInput can't fight the user's
own kb/mouse. Gated on `[Debug] rpc_auto_idle` (default true; the 1500ms user-active window + 300ms
per-injection guard are hardcoded calibration). Driver implications:

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
