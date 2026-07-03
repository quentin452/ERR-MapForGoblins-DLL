#!/usr/bin/env python3
"""MapForGoblins debug-RPC driver (Phase 3 of docs/plans/overlay_hot_reload_playwright_plan.md).

Talks to the DLL's dev-only TCP loopback listener (ini [Debug] debug_rpc_port, empty = disabled).
Works from Linux against the game running under Proton (Wine loopback == host loopback) and from
Windows alike. Line protocol: send one command line, read one "ok ..."/"err ..." reply line.

CLI:
    ./tools/mfg_rpc.py --port 38700 ping
    ./tools/mfg_rpc.py --port 38700 status
    ./tools/mfg_rpc.py --port 38700 open_f1 toggle
    ./tools/mfg_rpc.py --port 38700 set map_symbol_scale 3.0
    ./tools/mfg_rpc.py --port 38700 screenshot 'Z:\\tmp\\shot.bmp'
    ./tools/mfg_rpc.py --port 38700 reload_overlay
    ./tools/mfg_rpc.py --port 38700 wait-reload      # reload_overlay + poll status until gen bumps
    ./tools/mfg_rpc.py --port 38700 wait-idle        # block until the user stops driving (rpc_input_idle=0)

NB screenshot paths are interpreted by the GAME process: under Proton use a Wine path
(Z:\\ maps to the Linux root, e.g. Z:\\home\\user\\shot.bmp or Z:\\tmp\\shot.bmp).

Scriptable:
    from mfg_rpc import Rpc
    with Rpc(38700) as rpc:
        print(rpc.cmd("status"))
"""

import argparse
import re
import socket
import sys
import time


class Rpc:
    def __init__(self, port, host="127.0.0.1", timeout=20.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buf = b""

    def cmd(self, line):
        """Send one command line, return the reply line (str, no newline)."""
        self.sock.sendall(line.encode("utf-8") + b"\n")
        while b"\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("RPC connection closed")
            self.buf += chunk
            if len(self.buf) > 1 << 20:
                raise RuntimeError("reply exceeds 1 MB with no newline — protocol desync?")
        reply, self.buf = self.buf.split(b"\n", 1)
        return reply.decode("utf-8", "replace").rstrip("\r")

    def status(self):
        """Parse `status` into a dict of ints, e.g. {'panel': 0, 'hotreload': 1, 'gen': 2, ...}."""
        reply = self.cmd("status")
        if not reply.startswith("ok"):
            raise RuntimeError(reply)
        return {k: int(v) for k, v in re.findall(r"(\w+)=(-?\d+)", reply)}

    def wait_reload(self, timeout=60.0, poll=0.5):
        """reload_overlay, then poll until render_generation bumps. Returns the new gen."""
        before = self.status()["gen"]
        reply = self.cmd("reload_overlay")
        if not reply.startswith("ok"):
            raise RuntimeError(reply)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            gen = self.status()["gen"]
            if gen > before:
                return gen
            time.sleep(poll)
        raise TimeoutError(f"render generation stuck at {before} after {timeout}s")

    def wait_idle(self, timeout=30.0, poll=0.3):
        """Block until rpc_input_idle=0 (the user is no longer actively driving kb/mouse), so a
        scripted input run won't fight the human. Returns the final status dict. If rpc_auto_idle
        is off in the ini, rpc_input_idle is always 0 and this returns immediately."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            st = self.status()
            if st.get("rpc_input_idle", 0) == 0:
                return st
            time.sleep(poll)
        raise TimeoutError(f"user still active after {timeout}s (rpc_input_idle=1)")

    def close(self):
        self.sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, required=True, help="debug_rpc_port from MapForGoblins.ini")
    ap.add_argument("command", nargs="+", help="command + args (or wait-reload)")
    args = ap.parse_args()
    with Rpc(args.port) as rpc:
        if args.command[0] == "wait-reload":
            print(f"ok gen={rpc.wait_reload()}")
            return 0
        if args.command[0] == "wait-idle":
            st = rpc.wait_idle()
            print(f"ok idle user_idle_ms={st.get('user_idle_ms', '?')}")
            return 0
        reply = rpc.cmd(" ".join(args.command))
        print(reply)
        return 0 if reply.startswith("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
