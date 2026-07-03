#!/usr/bin/env python3
"""Run the MapForGoblins in-game RPC regression tests.

Each test in tools/rpc_tests/test_*.py is a standalone script (its own game boot, via
mfg_session.run_test) so a crash in one can't poison the others. This runner execs them one at a
time and reports pass/fail by exit code.

    python3 tools/mfg_test.py                 # run all
    python3 tools/mfg_test.py sidecar warp    # run tests whose name contains these

Prereqs: the DLL deployed with [Debug] debug_rpc_port + (for sidecar tests) [Sidecar] sidecar_save
set in the deployed ini, and ER NOT already running. Slow: ~1 boot (~40-60s) per test.
"""
import glob
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS_DIR = os.path.join(HERE, "rpc_tests")


def main():
    filters = sys.argv[1:]
    tests = sorted(glob.glob(os.path.join(TESTS_DIR, "test_*.py")))
    if filters:
        tests = [t for t in tests if any(f in os.path.basename(t) for f in filters)]
    if not tests:
        print("no matching tests")
        return 1
    results = []
    for t in tests:
        name = os.path.basename(t)
        print(f"\n{'='*70}\n RUN {name}\n{'='*70}", flush=True)
        t0 = time.monotonic()
        rc = subprocess.run([sys.executable, t]).returncode
        results.append((name, rc == 0, int(time.monotonic() - t0)))
        # give the OS a moment to fully release the game process between boots
        time.sleep(3)
    print(f"\n{'='*70}\n SUMMARY\n{'='*70}", flush=True)
    ok = 0
    for name, passed, secs in results:
        print(f"  {'PASS' if passed else 'FAIL'}  {name}  ({secs}s)", flush=True)
        ok += passed
    print(f"\n{ok}/{len(results)} tests passed", flush=True)
    return 0 if ok == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
