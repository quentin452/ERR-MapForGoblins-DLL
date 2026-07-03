#!/usr/bin/env python3
"""mfg — one front door for the MapForGoblins dev-RPC tooling.

Wraps the scattered scripts (mfg_rpc, mfg_session, mfg_test) behind subcommands:

    mfg rpc <cmd> [args...]      one-shot RPC against a RUNNING game (goods_count 0x.. etc.)
    mfg repl [--boot]            interactive RPC shell (history, tab on blank line lists verbs)
    mfg run <script> [--boot]    replay a saved command script (one RPC line per row; # = comment)
    mfg test [name...]           run the in-game regression suite (each test cold-boots ER)

Attach vs boot:
  - `rpc` always attaches to an already-running game (the fast path while you iterate).
  - `repl`/`run` attach by default; pass --boot to cold-boot ER for the session (loads a save,
    kills the game on exit). Booting needs Steam already running (see mfg-rpc-driver-hardening.md).

Port: --port, else $MFG_RPC_PORT, else 38700 (matches MapForGoblins.ini [Debug] debug_rpc_port).
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from mfg_rpc import Rpc  # noqa: E402

DEFAULT_PORT = int(os.environ.get("MFG_RPC_PORT", "38700"))


def _connect(port):
    try:
        return Rpc(port)
    except (OSError, ConnectionError):
        sys.exit(f"mfg: no RPC listener on 127.0.0.1:{port} — is ER running with the DLL deployed "
                 f"(and Steam up)? Use `mfg repl --boot` to launch it.")


# ---- rpc ----
def cmd_rpc(a):
    line = " ".join(a.words).strip()
    if not line:
        sys.exit("mfg rpc: needs a command, e.g. `mfg rpc goods_count 0x40003bed`")
    rpc = _connect(a.port)
    try:
        print(rpc.cmd(line))
    finally:
        rpc.close()


# ---- repl ----
def _repl_loop(send):
    try:
        import readline  # noqa: F401  (enables history + line editing if available)
    except Exception:
        pass
    print("mfg repl — type an RPC command, `quit`/Ctrl-D to exit.")
    while True:
        try:
            line = input("mfg> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return
        if not line:
            continue
        if line in ("quit", "exit"):
            return
        try:
            print(send(line))
        except (OSError, ConnectionError) as e:
            print(f"  (connection lost: {e})")
            return


def cmd_repl(a):
    if a.boot:
        from mfg_session import GameSession
        with GameSession() as g:
            g.load_save()
            _repl_loop(g.rpc)
    else:
        rpc = _connect(a.port)
        try:
            _repl_loop(rpc.cmd)
        finally:
            rpc.close()


# ---- run ----
def _read_script(path):
    with open(path, "r", encoding="utf-8") as f:
        for raw in f:
            line = raw.split("#", 1)[0].strip()
            if line:
                yield line


def _run_lines(send, lines):
    bad = 0
    for line in lines:
        reply = send(line)
        mark = "ok " if reply.startswith("ok") else ("ERR" if reply.startswith("err") else "?  ")
        if reply.startswith("err"):
            bad += 1
        print(f"[{mark}] {line}\n        {reply}")
    return bad


def cmd_run(a):
    if not os.path.exists(a.script):
        sys.exit(f"mfg run: no such script {a.script}")
    lines = list(_read_script(a.script))
    if a.boot:
        from mfg_session import GameSession
        with GameSession() as g:
            g.load_save()
            bad = _run_lines(g.rpc, lines)
    else:
        rpc = _connect(a.port)
        try:
            bad = _run_lines(rpc.cmd, lines)
        finally:
            rpc.close()
    print(f"\n{len(lines) - bad}/{len(lines)} ok")
    sys.exit(1 if bad else 0)


# ---- test ----
def cmd_test(a):
    import mfg_test
    sys.argv = ["mfg_test"] + a.names
    sys.exit(mfg_test.main())


def build_parser():
    p = argparse.ArgumentParser(prog="mfg", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"RPC port (default {DEFAULT_PORT})")
    sub = p.add_subparsers(dest="sub", required=True)

    pr = sub.add_parser("rpc", help="one-shot RPC against a running game")
    pr.add_argument("words", nargs=argparse.REMAINDER, help="the RPC command + args")
    pr.set_defaults(fn=cmd_rpc)

    pp = sub.add_parser("repl", help="interactive RPC shell")
    pp.add_argument("--boot", action="store_true", help="cold-boot ER + load a save for the session")
    pp.set_defaults(fn=cmd_repl)

    pn = sub.add_parser("run", help="replay a command script")
    pn.add_argument("script", help="file of RPC command lines (# comments allowed)")
    pn.add_argument("--boot", action="store_true", help="cold-boot ER + load a save for the run")
    pn.set_defaults(fn=cmd_run)

    pt = sub.add_parser("test", help="run the in-game regression suite")
    pt.add_argument("names", nargs="*", help="substring filters (e.g. custom_item)")
    pt.set_defaults(fn=cmd_test)
    return p


def main():
    a = build_parser().parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
