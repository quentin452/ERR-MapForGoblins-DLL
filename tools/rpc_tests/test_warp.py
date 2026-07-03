#!/usr/bin/env python3
"""Regression: grace warp moves the player + the game survives.

Run: python3 tools/rpc_tests/test_warp.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from mfg_session import run_test  # noqa: E402

ROUNDTABLE = 11102950
FIRST_STEP = 1042362951


def test(g):
    g.load_save()
    g.assert_in(f"warp {ROUNDTABLE}", "ok warp", "warp to Roundtable Hold accepted")
    g.check("alive after warp", g.alive())
    # a second warp (different destination) — the game must stay alive through repeated warps.
    g.assert_in(f"warp {FIRST_STEP}", "ok warp", "warp to The First Step accepted")
    g.check("alive after 2nd warp", g.alive())


if __name__ == "__main__":
    run_test(test)
