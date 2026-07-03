#!/usr/bin/env python3
"""Regression: sidecar state store round-trips through disk (in one session).

Needs [Sidecar] sidecar_save = true + [Debug] debug_rpc_port set in the deployed ini.
Run: python3 tools/rpc_tests/test_sidecar.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from mfg_session import run_test  # noqa: E402


def test(g):
    g.load_save()
    g.assert_in("sidecar status", "path=", "sidecar bound to a save path")

    # kv round-trip through disk: set → save → dirty memory → load → read the DISK value.
    g.rpc("sidecar setkv rt_key hello")
    g.assert_in("sidecar save", "ok saved", "sidecar wrote .mfg")
    g.rpc("sidecar setkv rt_key OVERWRITTEN")          # dirty in-memory
    g.rpc("sidecar load")                              # re-read disk
    g.assert_in("sidecar getkv rt_key", "rt_key=hello", "load read the disk value (not the dirty one)")

    # custom flag persists to the store.
    g.rpc("sidecar addflag 2000000000")
    g.assert_in("sidecar save", "ok saved")
    g.rpc("sidecar load")
    g.assert_in("sidecar flags", "2000000000", "custom flag survived save+load")


if __name__ == "__main__":
    run_test(test)
