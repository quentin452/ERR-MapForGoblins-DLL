#!/usr/bin/env python3
"""Probe: which flag does the engine ACTUALLY set when a boss dies?

Settles the one question the EMEVD could not answer statically. The defeat
registration (`2003[12]`) names an entity whose id normally doubles as the defeat
flag — except for night/roaming bosses, where the persistent flag is assigned from a
PARAMETER that a literal parser reads as a placeholder. See
docs/memory/features/run-tracker.md ("Probe 1 ran and FAILED").

Instead of parsing, ASK THE RUNNING GAME: scan the whole 1000-id band around each
suspect boss's tile and print every flag that is ON. On a save where that boss is
already dead, the persistent flag is in the list and the answer is direct.

Needs: game running, in-world (not the main menu), ini `[Debug] debug_rpc_port`.
Usage:  python tools/rpc_tests/probe_boss_defeat_flags.py [--port 38700]
"""
import argparse
import socket
import sys

DEFAULT_PORT = 38700

# The entities our EMEVD scan registered whose id sits in the game's reset group
# (common.emevd turns them OFF), i.e. the ones whose flag cannot be trusted.
SUSPECTS = [
    1036450340, 1036480340, 1037420340, 1038520340,
    1039430340, 1043370340, 1044320340, 1044320342,
]
# Controls: ordinary bosses whose entity id IS the flag. If these read consistently
# the probe itself is sound; if they do not, distrust everything below.
CONTROLS = [
    (10000800, "Godrick the Grafted"),
    (15000800, "Malenia"),
    (1042360800, "Tree Sentinel (Limgrave)"),
]


def rpc(port, cmd, timeout=30.0):
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        s.settimeout(timeout)
        s.sendall((cmd + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    return buf.decode(errors="replace").strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    args = ap.parse_args()

    if "pong" not in rpc(args.port, "ping"):
        sys.exit("no RPC answer — game not running, or debug_rpc_port not set")
    # A stale DLL answers ping too: make sure this build knows the verb.
    probe = rpc(args.port, "flag 6001")
    if probe.startswith("err") or "unknown" in probe:
        sys.exit(f"the running DLL has no `flag` verb ({probe!r}) — redeploy + restart")
    if probe.endswith("= 0"):
        sys.exit("flag 6001 (AlwaysOn) reads 0 — flag API cold or at the main menu; load a save")

    print("controls (entity id should equal the flag):")
    for ent, name in CONTROLS:
        print(f"   {ent:<11} {rpc(args.port, f'flag {ent}').split('=')[-1].strip():<3} {name}")

    print("\nsuspects — every flag ON in the boss's own 1000-id band:")
    for ent in SUSPECTS:
        base = (ent // 1000) * 1000
        own = rpc(args.port, f"flag {ent}").split("=")[-1].strip()
        rng = rpc(args.port, f"flag range {base} {base + 999}")
        on = rng.split(" ", 4)[-1] if " " in rng else rng
        print(f"   {ent} (own={own})  band {base}: {on or '<none ON>'}")

    print("\nReading: a band whose ONLY ON flag is the entity id means the boss is not dead on"
          "\nthis save (nothing to learn). A band with the entity OFF and an x800 sibling ON is"
          "\nthe proof that the persistent flag is that sibling.")


if __name__ == "__main__":
    main()
