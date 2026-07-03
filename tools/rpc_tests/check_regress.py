#!/usr/bin/env python3
"""Regression checker over the RPC-test ledger (tools/rpc_tests/results.jsonl).

mfg_session.summary() appends one record per test run:
    {"test","ts","passed","total","ok","checks":[{"name","ok"},...]}

This scans that ledger, groups runs by test, and — for each test — compares its two most recent runs to
report REGRESSIONS (a named check that was PASS in the prior run and FAILS in the latest, or an overall
pass->fail), plus RECOVERED checks and NEW failures. Exit code is non-zero if any regression is found, so
it can gate an agent/CI sweep.

Usage:
    python tools/rpc_tests/check_regress.py            # latest-vs-prior per test; exit 1 on any regression
    python tools/rpc_tests/check_regress.py --test X   # only test X (basename or substring)
    python tools/rpc_tests/check_regress.py --history  # print every run per test (audit)
    python tools/rpc_tests/check_regress.py --ledger P # use a ledger at path P
"""
import argparse
import json
import os
import sys

DEFAULT_LEDGER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "results.jsonl")


def load_runs(path):
    """Return {test_name: [run, ...]} in file order (oldest first)."""
    runs = {}
    if not os.path.exists(path):
        return runs
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                r = json.loads(line)
            except Exception:
                continue
            t = r.get("test", "unknown")
            runs.setdefault(t, []).append(r)
    return runs


def checks_map(run):
    return {c.get("name"): bool(c.get("ok")) for c in run.get("checks", [])}


def diff(prev, cur):
    """Return (regressed, recovered, new_fail) name lists between two runs."""
    pm, cm = checks_map(prev), checks_map(cur)
    regressed = sorted(n for n, ok in cm.items() if not ok and pm.get(n) is True)
    recovered = sorted(n for n, ok in cm.items() if ok and pm.get(n) is False)
    new_fail = sorted(n for n, ok in cm.items() if not ok and n not in pm)
    return regressed, recovered, new_fail


def main():
    ap = argparse.ArgumentParser(description="RPC-test regression checker")
    ap.add_argument("--ledger", default=DEFAULT_LEDGER, help="path to results.jsonl")
    ap.add_argument("--test", default=None, help="only tests whose name contains this substring")
    ap.add_argument("--history", action="store_true", help="print every run per test")
    args = ap.parse_args()

    runs = load_runs(args.ledger)
    if not runs:
        print(f"[check-regress] no ledger / no runs at {args.ledger}")
        return 0

    tests = sorted(runs)
    if args.test:
        tests = [t for t in tests if args.test in t]
        if not tests:
            print(f"[check-regress] no test matches '{args.test}'")
            return 0

    any_regress = False
    print(f"[check-regress] ledger {args.ledger} — {len(tests)} test(s)\n")
    for t in tests:
        rl = runs[t]
        latest = rl[-1]
        status = "PASS" if latest.get("ok") else "FAIL"
        line = f"  {status}  {t}  {latest.get('passed')}/{latest.get('total')}  @{latest.get('ts')}"
        if args.history:
            print(line)
            for r in rl:
                fails = [c["name"] for c in r.get("checks", []) if not c.get("ok")]
                tag = "ok " if r.get("ok") else "FAIL"
                print(f"       {tag} {r.get('ts')} {r.get('passed')}/{r.get('total')}"
                      + (("  fails: " + ", ".join(fails)) if fails else ""))
            continue

        if len(rl) < 2:
            print(line + "   (first run — no prior to compare)")
            continue
        regressed, recovered, new_fail = diff(rl[-2], latest)
        overall_regress = rl[-2].get("ok") and not latest.get("ok")
        if regressed or (overall_regress and not regressed and not new_fail):
            any_regress = True
            print("  ⚠ REGRESSION " + t)
            print(line)
            if regressed:
                print("       was PASS, now FAIL: " + ", ".join(regressed))
            elif overall_regress:
                print(f"       prior run all-pass, now {latest.get('passed')}/{latest.get('total')}")
        else:
            print(line + (("   recovered: " + ", ".join(recovered)) if recovered else ""))
        if new_fail:
            print("       new failing check(s): " + ", ".join(new_fail))

    if any_regress:
        print("\n[check-regress] ⚠ REGRESSIONS FOUND")
        return 1
    print("\n[check-regress] no regressions vs the prior run of each test")
    return 0


if __name__ == "__main__":
    sys.exit(main())
