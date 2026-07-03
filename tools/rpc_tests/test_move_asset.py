#!/usr/bin/env python3
"""Live test of the geom transform SETTER (vtable[0xd0], docs/re/windows_msb_placement_write_re_findings.md).

Boots ER, loads a save (in-world so geom tiles are loaded), then calls `move_asset <dx dy dz>` which
picks a live CSWorldGeomIns, moves it via the engine's own virtual setter, reads back the cached world
matrix (+0x220), and restores it. Asserts the setter moved the translation by exactly the delta and the
restore put it back — and that the game survived the vcalls.

Run: python tools/rpc_tests/test_move_asset.py  (or via mfg.py test move_asset)
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from mfg_session import run_test  # noqa: E402


def triple(reply, key):
    m = re.search(key + r"=\(([-\d.]+),([-\d.]+),([-\d.]+)\)", reply)
    return (float(m.group(1)), float(m.group(2)), float(m.group(3))) if m else None


def _test(g):
    g.load_save()
    g.check("alive in-world", g.alive())

    dy = 100.0
    reply = g.rpc(f"move_asset 0 {dy} 0", timeout=15)
    g.check("move_asset ok", reply.startswith("ok"), reply)
    before, moved, restored = triple(reply, "before"), triple(reply, "moved"), triple(reply, "restored")
    if not g.check("parsed before/moved/restored", all((before, moved, restored)), reply):
        return

    # Setter moved the cached-matrix Y translation by exactly +dy (x,z unchanged).
    g.check("setter moved Y by +delta", abs(moved[1] - (before[1] + dy)) < 0.5,
            f"before.y={before[1]:.2f} moved.y={moved[1]:.2f} (+{dy})")
    g.check("setter left X/Z unchanged", abs(moved[0] - before[0]) < 0.5 and abs(moved[2] - before[2]) < 0.5,
            f"before=({before[0]:.2f},{before[2]:.2f}) moved=({moved[0]:.2f},{moved[2]:.2f})")
    # Restore returned the translation to the original.
    g.check("restore returned to original",
            all(abs(restored[i] - before[i]) < 0.5 for i in range(3)),
            f"before={before} restored={restored}")
    # The vcalls did not crash the game.
    g.check("game alive after the vcalls", g.alive())

    # Targeted move: move a SPECIFIC asset's nearest placement (World Editor "Move this asset"). Find a
    # loaded aegRow via geom_dump, move it +40Y, assert it moved, restore.
    import re, os
    log = os.path.expanduser("~/Games/ERRv2.2.9.6/dll/offline/logs/MapForGoblins.log")
    g.rpc("geom_dump", timeout=15)
    dump_lines = [l for l in open(log) if "[GEOMDUMP] inst=" in l]
    m = re.search(r"aegRow=(\d+)", dump_lines[-1]) if dump_lines else None
    aeg = int(m.group(1)) if m else 0
    if g.check("geom_dump reported a loaded aegRow", aeg > 0, f"aeg={aeg}"):
        rr = g.rpc(f"move_aeg {aeg} 0 40 0", timeout=15)
        ab, an = triple(rr, "before"), triple(rr, "now")
        g.check("move_aeg targeted the asset + moved +40Y",
                bool(ab and an) and abs(an[1] - (ab[1] + 40)) < 0.5, rr)
        g.rpc("move_restore", timeout=10)
        g.check("game alive after targeted move", g.alive())


if __name__ == "__main__":
    run_test(_test)
