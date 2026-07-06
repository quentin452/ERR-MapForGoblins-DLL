#!/usr/bin/env python3
"""Boot ER (via me3, the RPC-enabled err_offline profile), load a save to in-world, then HOLD the game
alive so you can drive it with `mfg.py rpc <cmd>` one-shots — without an interactive repl.

Unlike `mfg.py repl --boot` (which owns an interactive shell + kills the game on exit), this:
  - RELEASES its RPC socket right after boot, so external `mfg.py rpc` one-shots can connect (the in-game
    RPC server is single-client — a held socket makes one-shots hang/time out).
  - Sleeps forever holding the game process alive. Ctrl-C (or stopping the VSCode task) kills the game.

Use it as the VSCode task "MFG: Boot ER + HOLD" — start it, leave it running for a whole session, and send
`mfg.py rpc ...` from another terminal / the RPC-one-shot task.

Gotchas (see docs/memory/tooling/mfg-rpc-driver-hardening.md):
  - Needs Steam already running.
  - The me3 profile MUST be err_offline.me3 (the default in mfg_session) — do NOT override MFG_PROFILE=err
    (that's a DATA-pipeline profile name; me3 rejects it and exits before the RPC comes up).
  - If a previous game left a STALE listener on the RPC port, a fresh boot connects to the dead socket and
    fails ("RPC up ~0s" then ConnectionError). Kill all eldenring.exe/me3/wineserver + confirm the port is
    free (`ss -tlnp | grep 38700`) before starting.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mfg_session import GameSession  # noqa: E402


def main():
    g = GameSession(boot_timeout=420, verbose=True)
    g.__enter__()
    try:
        g.load_save()
        print("STATUS:", g.rpc("status"), flush=True)
        print("BUILD:", g.rpc("mfg_build"), flush=True)
        # Release the RPC socket so external `mfg.py rpc` one-shots can connect (single-client server).
        try:
            g.rpc_conn.close()
            g.rpc_conn = None
        except Exception:
            pass
        print("HOLDING — game is up + in-world; drive it with `mfg.py rpc <cmd>`. Ctrl-C to quit.", flush=True)
        while True:
            time.sleep(10)
    finally:
        g.__exit__()


if __name__ == "__main__":
    main()
