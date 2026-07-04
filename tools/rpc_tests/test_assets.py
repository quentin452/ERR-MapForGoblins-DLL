#!/usr/bin/env python3
"""Path-loading regression guard (design: virtual_world_multi_world_design.md).

The mod reads menu assets off the ACTIVE install, and the load path differs by install SHAPE — vanilla
PACKED (dvdbnd Data*.bhd), vanilla UXM-UNPACKED (loose menu/…), or ERR-modded (me3 loose overlay + base
dvdbnd). A subtle path break can silently regress one shape while another still works. `assets_probe`
resolves the key menu files through the mod's own loader and reports per-file shape+size; this test asserts
they resolve (nothing MISSING) and records the shape into the ledger, so a break shows as a regression
(a file that resolved before → MISSING now) instead of a phantom.

Run: python tools/rpc_tests/test_assets.py
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from mfg_session import run_test  # noqa: E402


def _test(g):
    g.load_save()
    g.check("alive in-world", g.alive())

    reply = g.rpc("assets_probe", timeout=30)  # loose decompress of an icon sheet can take a few s
    print("assets_probe ->", reply)
    g.check("assets_probe ok", reply.startswith("ok"), reply)

    m = re.search(r"missing=(\d+)", reply)
    missing = int(m.group(1)) if m else -1
    g.check("no menu asset MISSING (load path resolves)", missing == 0, reply)

    # Per-file: each listed asset must NOT be MISSING (catches a break on THIS install shape).
    for rel in ("menu/71_MapTile.tpfbhd", "menu/hi/01_common.tpf.dcx"):
        mm = re.search(re.escape(rel) + r"=(\w+)\((\d+)\)", reply)
        shape = mm.group(1) if mm else "?"
        size = int(mm.group(2)) if mm else 0
        g.check(f"{rel} resolves ({shape})", mm is not None and shape != "MISSING" and size > 0, reply)

    sm = re.search(r"shape=(\S+)", reply)
    print(f"[info] install shape = {sm.group(1) if sm else '?'} (recorded for the ledger)")
    g.check("game alive after probe", g.alive())


SWEEP = _test  # run_all.py aggregation entry — single-boot, self-loads (safe to share a session)


if __name__ == "__main__":
    run_test(_test)
