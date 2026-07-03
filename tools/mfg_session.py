#!/usr/bin/env python3
"""In-game RPC scripting for MapForGoblins — regression tests + fast repro.

Wraps the whole hand-rolled boot dance (launch me3 as a foreground child, poll `ping`, drive the
title→in-world nav, run RPC commands, kill before returning) behind a context manager, so a test /
repro is a few lines of Python instead of 40 lines of bash.

    from mfg_session import GameSession
    with GameSession() as g:
        g.load_save()                       # title → in-world
        g.rpc("sidecar setkv foo bar")
        g.rpc("sidecar save")
        g.assert_in("sidecar getkv foo", "foo=bar")
        g.warp(11102950)                    # Roundtable Hold

Constraints baked in (learned the hard way, see docs/memory/tooling/mfg-rpc-driver-hardening.md):
  * me3 MUST be an in-shell child killed before the process returns (a detached child is reaped) —
    so the whole script runs inside ONE `python3 mfg_session.py`/test invocation.
  * kill with `pkill -x eldenring.exe` (exact name) — `pkill -f .../eldenring.exe` also matches this
    script's own argv and kills it.
  * every rpc() has a per-call timeout (a frozen present thread hangs the socket).
  * the save file is NOT open at the title screen — load_save() waits for the sidecar path to bind.

Env overrides: MFG_ME3, MFG_EXE, MFG_PROFILE, MFG_ME3_CWD, MFG_RPC_PORT.
"""
import os
import re
import signal
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mfg_rpc import Rpc  # noqa: E402

HOME = os.path.expanduser("~")
ME3 = os.environ.get("MFG_ME3", f"{HOME}/Games/ERRv2.2.9.6/internals/modengine/bin/me3")
EXE = os.environ.get(
    "MFG_EXE", "/home/iamacat/.local/share/Steam/steamapps/common/ELDEN RING/Game/eldenring.exe")
PROFILE = os.environ.get("MFG_PROFILE", "err_offline.me3")
ME3_CWD = os.environ.get("MFG_ME3_CWD", f"{HOME}/Games/ERRv2.2.9.6/internals/modengine")
PORT = int(os.environ.get("MFG_RPC_PORT", "38700"))


class AssertionFail(Exception):
    pass


class GameSession:
    """Launch ER via me3, expose the RPC, kill on exit. Context manager."""

    def __init__(self, port=PORT, boot_timeout=260, launch=True, verbose=True):
        self.port = port
        self.boot_timeout = boot_timeout
        self.launch = launch
        self.verbose = verbose
        self.proc = None
        self.rpc_conn = None
        self.checks = []  # (name, ok, detail) for the test summary

    # ---- lifecycle ----
    def _log(self, *a):
        if self.verbose:
            print(*a, flush=True)

    def __enter__(self):
        if self.launch:
            self._log(f"[session] launching me3 ({PROFILE}) …")
            self.proc = subprocess.Popen(
                [ME3, "launch", "-g", "eldenring", "-e", EXE, "-p", PROFILE],
                cwd=ME3_CWD, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self._wait_rpc()
        return self

    def __exit__(self, *exc):
        try:
            if self.rpc_conn:
                self.rpc_conn.close()
        except Exception:
            pass
        if self.launch:
            self._log("[session] killing game …")
            if self.proc and self.proc.poll() is None:
                self.proc.send_signal(signal.SIGTERM)
            subprocess.run(["pkill", "-x", "eldenring.exe"], check=False)
            time.sleep(2)
        return False

    def _wait_rpc(self):
        t0 = time.monotonic()
        while time.monotonic() - t0 < self.boot_timeout:
            if self.launch and self.proc and self.proc.poll() is not None:
                raise RuntimeError("me3 exited before the RPC came up")
            try:
                c = Rpc(self.port, timeout=6.0)
                if "pong" in c.cmd("ping"):
                    self.rpc_conn = c
                    self._log(f"[session] RPC up (~{int(time.monotonic()-t0)}s)")
                    return
                c.close()
            except (OSError, ConnectionError):
                pass
            time.sleep(3)
        raise RuntimeError(f"RPC never came up within {self.boot_timeout}s")

    # ---- commands ----
    def rpc(self, line, timeout=10.0):
        """Send one RPC command, return the reply. Reconnects once on a dropped socket."""
        try:
            self.rpc_conn.sock.settimeout(timeout)
            r = self.rpc_conn.cmd(line)
        except (OSError, ConnectionError):
            self.rpc_conn = Rpc(self.port, timeout=timeout)
            self.rpc_conn.sock.settimeout(timeout)
            r = self.rpc_conn.cmd(line)
        self._log(f"  > {line}\n  < {r}")
        return r

    def status(self):
        r = self.rpc("status")
        return {k: int(v) for k, v in re.findall(r"(\w+)=(-?\d+)", r)}

    # ---- nav ----
    def load_save(self, timeout=90):
        """Title → in-world. Dismiss the splash, confirm Continue, wait for the sidecar to bind
        (the save file opens only in-world) + settle past the load tips."""
        time.sleep(6)
        self.rpc("key Return"); time.sleep(3)
        self.rpc("key e"); time.sleep(3)      # confirm Continue (this install's bind)
        self.rpc("key Return")
        t0 = time.monotonic()
        while time.monotonic() - t0 < timeout:
            s = self.rpc("sidecar status")
            if "path=" in s and "path=(none)" not in s:
                break
            time.sleep(4)
        else:
            raise RuntimeError("never reached in-world (sidecar path did not bind)")
        time.sleep(10)           # settle past load tips
        self.rpc("key e"); time.sleep(2)
        self._log("[session] in-world")

    def warp(self, grace_id, wait=12):
        r = self.rpc(f"warp {grace_id}")
        time.sleep(wait)
        return r

    def screenshot(self, path):
        """path is the GAME's view (Z:\\...). Returns the reply."""
        return self.rpc(f"screenshot {path}")

    def alive(self):
        try:
            return "pong" in self.rpc("ping", timeout=6.0)
        except Exception:
            return False

    # ---- assertions (for regression tests) ----
    def check(self, name, ok, detail=""):
        self.checks.append((name, bool(ok), detail))
        self._log(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""))
        return ok

    def assert_in(self, cmd_or_reply, needle, name=None):
        """Run cmd (or use a given reply string) and check it contains needle."""
        reply = cmd_or_reply if cmd_or_reply.startswith(("ok", "err")) else self.rpc(cmd_or_reply)
        return self.check(name or f"'{needle}' in `{cmd_or_reply}`", needle in reply, reply)

    def summary(self):
        passed = sum(1 for _, ok, _ in self.checks if ok)
        total = len(self.checks)
        print(f"\n=== {passed}/{total} checks passed ===", flush=True)
        for n, ok, d in self.checks:
            if not ok:
                print(f"  FAIL: {n} — {d}", flush=True)
        self._persist_results(passed, total)
        return passed == total

    def finish_test(self, test_name):
        """Aggregate mode (run_all --aggregate): the tests share ONE GameSession, so this records the
        checks accumulated for `test_name` as its own ledger run, prints a per-test summary, then RESETS
        self.checks for the next test. Returns True iff all passed."""
        passed = sum(1 for _, ok, _ in self.checks if ok)
        total = len(self.checks)
        print(f"\n=== [{test_name}] {passed}/{total} checks passed ===", flush=True)
        for n, ok, d in self.checks:
            if not ok:
                print(f"  FAIL: {n} — {d}", flush=True)
        self._persist_results(passed, total, test_name)
        self.checks = []
        return passed == total

    def _persist_results(self, passed, total, test_name=None):
        """Append this run's PASS/FAIL to tools/rpc_tests/results.jsonl so a regression is RECORDED,
        not a phantom, and loudly flag any check that PASSED in the prior run of this same test but
        FAILS now (or an overall pass->fail). Best-effort: never let bookkeeping fail the test."""
        try:
            import json
            from datetime import datetime
            test = test_name or (os.path.basename(sys.argv[0]) if sys.argv and sys.argv[0] else "unknown")
            path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "rpc_tests", "results.jsonl")
            prior = None
            if os.path.exists(path):
                with open(path) as f:
                    for line in f:
                        try:
                            r = json.loads(line)
                        except Exception:
                            continue
                        if r.get("test") == test:
                            prior = r  # keep the LAST prior run of this test
            rec = {
                "test": test,
                "ts": datetime.now().isoformat(timespec="seconds"),
                "passed": passed, "total": total, "ok": passed == total,
                "checks": [{"name": n, "ok": ok} for n, ok, _ in self.checks],
            }
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "a") as f:
                f.write(json.dumps(rec) + "\n")
            # Regression detection vs the prior run of the SAME test.
            if prior:
                prev_ok = {c["name"]: c["ok"] for c in prior.get("checks", [])}
                regressed = [n for n, ok, _ in self.checks if not ok and prev_ok.get(n) is True]
                if regressed:
                    print(f"\n  ⚠ REGRESSION in {test} (was PASS, now FAIL): " + ", ".join(regressed),
                          flush=True)
                elif prior.get("ok") and passed != total:
                    print(f"\n  ⚠ REGRESSION in {test}: prior run was all-pass, this run is {passed}/{total}",
                          flush=True)
            print(f"  [results] appended to {path}", flush=True)
        except Exception as e:
            print(f"  [results] persist skipped: {e!r}", flush=True)


def run_test(fn):
    """Run a test function fn(session) inside a GameSession; exit code = pass/fail."""
    with GameSession() as g:
        try:
            fn(g)
        except Exception as e:
            g.check("test raised", False, repr(e))
        ok = g.summary()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    # Smoke test: boot, load, ping.
    def _smoke(g):
        g.load_save()
        g.check("alive after load", g.alive())
    run_test(_smoke)
