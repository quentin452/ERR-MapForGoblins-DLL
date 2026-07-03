#!/usr/bin/env python3
"""Run EVERY RPC test (tools/rpc_tests/test_*.py) sequentially, then the regression scan.

The ER cold boot (~45s-2min) is the real cost, so single-boot tests that declare a module-level
`SWEEP = <_test|test>` entry run TOGETHER in ONE shared GameSession (each self-loads via g.load_save(), so
they stay isolated); inherently multi-boot / boot-config tests (no SWEEP) run per-boot. This cuts the 9-test
suite from 9 boots to ~4. `--no-aggregate` forces everything per-boot. Every test appends its PASS/FAIL to
results.jsonl (via mfg_session), so after the sweep check_regress.py compares each test's new run to its
prior one and flags regressions. Meant for a nightly LOCAL cron (the game is on this box — no cloud cron).

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
import importlib.util
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


def sweep_entry(test_file):
    """Import a test module (its `if __name__=='__main__'` guard means importing does NOT boot) and return
    its `SWEEP` entry callable if it declared one (single-boot, aggregation-safe), else None. A multi-boot
    or boot-config test (custom_item/gapc/author_items) declares no SWEEP → runs per-boot."""
    path = os.path.join(HERE, test_file)
    try:
        spec = importlib.util.spec_from_file_location("sweep_" + test_file[:-3], path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        fn = getattr(mod, "SWEEP", None)
        return fn if callable(fn) else None
    except Exception as e:
        print(f"[sweep] import {test_file} failed ({e!r}) — will run per-boot", flush=True)
        return None


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
    ap.add_argument("--no-aggregate", action="store_true",
                    help="force EVERY test into its own boot (no shared session)")
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

    # Split into AGGREGATABLE (declare a `SWEEP` entry — single-boot, safe to share a session) and the
    # rest (multi-boot / boot-config → their own subprocess boot). Aggregation is a big speed win: N
    # single-boot tests in ONE game boot instead of N.
    results = []
    agg = []
    standalone = list(tests)
    if not args.no_aggregate:
        agg = [(t, e) for t in tests if (e := sweep_entry(t)) is not None]
        agg_names = {t for t, _ in agg}
        standalone = [t for t in tests if t not in agg_names]

    if agg:
        print(f"[sweep] aggregate phase: {len(agg)} test(s) in ONE boot "
              f"({', '.join(t for t, _ in agg)})\n", flush=True)
        sys.path.insert(0, os.path.dirname(HERE))  # tools/ so `from mfg_session import ...` resolves
        from mfg_session import GameSession
        try:
            with GameSession() as g:
                for name, entry in agg:
                    print(f"[sweep]   (agg) {name} …", flush=True)
                    t0 = time.time()
                    try:
                        entry(g)
                    except Exception as e:
                        g.check("test raised", False, repr(e))
                    ok = g.finish_test(name)   # records this test's checks to the ledger + resets
                    results.append((name, 0 if ok else 1))
                    print(f"[sweep]   (agg) {name} -> {'PASS' if ok else 'FAIL'} ({time.time()-t0:.0f}s)\n",
                          flush=True)
        except Exception as e:
            print(f"[sweep] aggregate boot failed ({e!r}) — those tests recorded as failed", flush=True)
            done = {n for n, _ in results}
            for name, _ in agg:
                if name not in done:
                    results.append((name, 1))
        kill_stragglers()

    if standalone:
        print(f"[sweep] per-boot phase: {len(standalone)} test(s), timeout {args.timeout}s each\n",
              flush=True)
    for i, t in enumerate(standalone, 1):
        print(f"[sweep] ({i}/{len(standalone)}) {t} …", flush=True)
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
