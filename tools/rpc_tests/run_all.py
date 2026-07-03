#!/usr/bin/env python3
"""Run EVERY RPC test (tools/rpc_tests/test_*.py) sequentially, then the regression scan.

Each test cold-boots ER under Proton (slow — ~1-3 min each) and can only run one at a time, so this is a
SERIAL sweep meant for a nightly LOCAL cron (the game runs on this box — a cloud cron cannot drive it).
Every test appends its PASS/FAIL to results.jsonl (via mfg_session), so after the sweep check_regress.py
compares each test's new run to its prior one and flags regressions.

Prerequisites (same as any single test): Steam already running (me3 needs it), a display for Proton.

Usage:
    python tools/rpc_tests/run_all.py                 # run all, then regression scan
    python tools/rpc_tests/run_all.py --only world    # only tests whose name contains "world"
    python tools/rpc_tests/run_all.py --list          # list the tests it would run
    python tools/rpc_tests/run_all.py --timeout 300   # per-test seconds (default 360)

Exit code: non-zero if ANY test failed/timed out OR check_regress found a regression.

Nightly local cron (edit `crontab -e`; adjust the repo path + hour, ensure Steam is up):
    30 4 * * *  cd /home/iamacat/Documents/GitHub/ERR-MapForGoblins-DLL && \
                /usr/bin/python tools/rpc_tests/run_all.py >> tools/rpc_tests/sweep.log 2>&1
"""
import argparse
import glob
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
# Tests known to need manual/interactive state or that are intentionally not part of the unattended
# sweep. Empty for now; add basenames here (not paths) to skip them.
SKIP = set()


def discover():
    names = sorted(os.path.basename(p) for p in glob.glob(os.path.join(HERE, "test_*.py")))
    return [n for n in names if n not in SKIP]


def kill_stragglers():
    """After a test (esp. a timeout that SIGKILLed the runner before GameSession.__exit__), make sure no
    ER/me3 lingers into the next boot. Safe BETWEEN tests: the test subprocess has already returned, so its
    driver shell is gone and only a real leftover game/me3 would match."""
    for pat in ("Game/eldenring.exe", "eldenring.exe", "me3"):
        subprocess.run(["pkill", "-9", "-f", pat], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    ap = argparse.ArgumentParser(description="Serial RPC-test sweep + regression scan")
    ap.add_argument("--timeout", type=int, default=360, help="per-test seconds (default 360)")
    ap.add_argument("--only", default=None, help="only tests whose name contains this substring")
    ap.add_argument("--list", action="store_true", help="list the tests and exit")
    args = ap.parse_args()

    tests = discover()
    if args.only:
        tests = [t for t in tests if args.only in t]
    if args.list:
        print("\n".join(tests) if tests else "(no tests)")
        return 0
    if not tests:
        print("[sweep] no tests found")
        return 0

    print(f"[sweep] {len(tests)} test(s), timeout {args.timeout}s each, serial (one game at a time)\n",
          flush=True)
    results = []
    for i, t in enumerate(tests, 1):
        print(f"[sweep] ({i}/{len(tests)}) {t} …", flush=True)
        t0 = time.time()
        try:
            r = subprocess.run([sys.executable, os.path.join(HERE, t)], cwd=REPO, timeout=args.timeout)
            rc = r.returncode
        except subprocess.TimeoutExpired:
            rc = 124
            print(f"[sweep]   TIMEOUT after {args.timeout}s", flush=True)
        except Exception as e:
            rc = 125
            print(f"[sweep]   launch error: {e!r}", flush=True)
        kill_stragglers()  # ensure a clean slate for the next boot
        dt = time.time() - t0
        results.append((t, rc))
        print(f"[sweep]   {t} -> rc={rc} ({dt:.0f}s)\n", flush=True)

    passed = [t for t, rc in results if rc == 0]
    print(f"[sweep] === {len(passed)}/{len(results)} tests passed ===", flush=True)
    for t, rc in results:
        print(f"   {'PASS' if rc == 0 else 'FAIL'} rc={rc:<4} {t}", flush=True)

    print("\n[sweep] regression scan:", flush=True)
    reg = subprocess.run([sys.executable, os.path.join(HERE, "check_regress.py")], cwd=REPO)
    regressed = reg.returncode != 0

    failed = len(results) - len(passed)
    if failed or regressed:
        print(f"\n[sweep] ⚠ {failed} test(s) failed"
              + (", regressions found" if regressed else "") + ".", flush=True)
        return 1
    print("\n[sweep] all tests passed, no regressions.", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
