#!/usr/bin/env python3
"""add_collision Route D — NON-MUTATING stages (docs/re/add_collision_linux_impl_brief.md).

Boots ER, loads a save (in-world so the Havok world + bodies are loaded, map CLOSED), then walks the
staged RPC WITHOUT ever calling addBody:
  1. `add_collision`                 -> hknpWorld/bodyMgr resolve (plausible bodies ptr + count)
  2. `add_collision recon`           -> phase-1 layout dumps still run clean
  3. `add_collision 100 20 100`      -> phase-2 cinfo build + dump (borrowed live shape), NO mutation
The mutating `go` stage is deliberately NOT here (it leaves a leaked probe body in the broadphase and is
verified manually with the hf_probe oracle — brief §6); this test keeps the nightly sweep side-effect-free.

Run: python tools/rpc_tests/test_add_collision.py
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from mfg_session import run_test, LOG  # noqa: E402


def _test(g):
    g.load_save()
    g.check("alive in-world", g.alive())

    # 1. resolve-only
    reply = g.rpc("add_collision", timeout=15)
    g.check("resolve ok", reply.startswith("ok"), reply)
    m = re.search(r"count=(\d+)", reply)
    g.check("body count plausible (1..0x100000)",
            m is not None and 0 < int(m.group(1)) < 0x100000, reply)
    g.check("alive after resolve", g.alive())

    # 2. phase-1 recon (read-only dumps)
    reply = g.rpc("add_collision recon", timeout=15)
    g.check("recon ok", reply.startswith("ok"), reply)
    g.check("alive after recon", g.alive())

    # 3. phase-2 cinfo build + dump — borrowed shape resolved, NO addBody
    reply = g.rpc("add_collision 100 20 100", timeout=15)
    g.check("cinfo dump ok", reply.startswith("ok"), reply)
    m = re.search(r"shape=(0x[0-9a-f]+)", reply)
    g.check("borrowed shape resolved", m is not None and int(m.group(1), 16) > 0x10000, reply)
    g.check("game alive after cinfo build (no mutation)", g.alive())

    # Show the last [ADDCOL] block for eyeballing.
    if os.path.exists(LOG):
        block = [l.rstrip() for l in open(LOG) if "[ADDCOL]" in l]
        print("\n--- last [ADDCOL] block ---")
        for l in block[-20:]:
            print(l.split("] [info] ")[-1] if "] [info] " in l else l)


SWEEP = _test  # run_all.py aggregation entry — single-boot, self-loads (safe to share a session)


if __name__ == "__main__":
    run_test(_test)
