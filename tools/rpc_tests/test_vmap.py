#!/usr/bin/env python3
"""Virtual-map panel regression guard (World Virtualization vision #1, slices A/B/D).

The virtual map is a mod-owned canvas drawn on its own per-frame entry (decoupled from F1). Its RPC
surface — `vmap 0|1|toggle | group <0-3> | fit` — drives open/close + the ER marker group it projects.
This asserts the open-state flag round-trips, the group setter sticks, and the game survives it. Pure
host state (no world load needed), so it is a single-boot SWEEP-safe test.

Run: python tools/rpc_tests/test_vmap.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from mfg_session import run_test  # noqa: E402


def _test(g):
    g.check("alive", g.alive())

    # open/close flag round-trips
    g.assert_in("vmap 0", "vmap=0", "vmap 0 -> closed")
    g.assert_in("vmap 1", "vmap=1", "vmap 1 -> open")
    g.assert_in("vmap toggle", "vmap=0", "vmap toggle -> closed")
    g.assert_in("vmap toggle", "vmap=1", "vmap toggle -> open")

    # group setter sticks (0..3 = the live ER marker groups)
    g.assert_in("vmap group 2", "group=2", "vmap group 2")
    g.assert_in("vmap group 0", "group=0", "vmap group 0 (reset)")

    # fit implies open (opens the map + frames the markers) — no crash is the assertion
    g.assert_in("vmap fit", "ok vmap fit", "vmap fit")

    # bad arg rejected, not silently accepted
    g.check("vmap junk -> err", g.rpc("vmap junk").startswith("err"))

    g.rpc("vmap 0")  # leave closed
    g.check("alive after vmap drive", g.alive())


SWEEP = _test  # run_all.py aggregation entry — single-boot, host-only state (safe to share a session)


if __name__ == "__main__":
    run_test(_test)
