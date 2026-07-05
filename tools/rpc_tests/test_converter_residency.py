#!/usr/bin/env python3
"""Converter RESIDENCY guard — the live WorldMapViewModel converter (worldmap_probe::project) must keep
working AFTER the native map closes. This is the property the vmap's underground/DLC projection (Fork 2)
AND the M5 native-draw cull both depend on: find_view_model() caches the VM (static s_vm) and the VM object
persists past map-close, so project() stays valid map-closed once the map has been opened once this session.

Rationale for the M5 cull: the recommended native-draw removal (D3D12 RSSetScissorRects empty-clip,
commit 2208332) hides PIXELS at rasterize but KEEPS the menu update/logic tick — strictly LESS teardown
than a full map close. So if the converter survives a close (this test), it survives the scissor cull a
fortiori. When the scissor toggle is RPC-exposed, extend this test to toggle it between the two projs.

Repro: open map + pan (resolve+cache VM) -> proj a point -> CLOSE map -> proj the same point -> assert
identical + valid. Run: python tools/rpc_tests/test_converter_residency.py
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from mfg_session import run_test  # noqa: E402


def _proj(g, area, gx, gz):
    r = g.rpc(f"proj {area} {gx} {gz}")
    m = re.search(r"u=(-?[\d.]+) v=(-?[\d.]+) page=(-?\d+)", r)
    return (float(m.group(1)), float(m.group(2)), int(m.group(3))) if m else None, r


def _test(g):
    g.load_save()
    g.check("alive in-world", g.alive())
    g.rpc("set rpc_auto_idle false")

    # open the native map + pan so the cursor publishes → find_view_model resolves + caches the VM.
    # Retry the open until the converter actually resolves (proj succeeds), since a single keypress can be
    # eaten (AZERTY/refocus). This is the ONE point where the map must be up.
    open_uv = None
    for _ in range(4):
        g.rpc("key m"); time.sleep(3)
        g.rpc("mouse_move 960 540"); time.sleep(0.3)
        g.rpc("mouse_drag 1000 560 820 460"); time.sleep(0.8)
        open_uv, open_r = _proj(g, 60, 100, 100)
        if open_uv is not None:
            break
    g.check("converter resolves once the map has been opened", open_uv is not None, open_r)

    # CLOSE the native map (Escape = layout-independent) and POLL until fully closed.
    closed = False
    for _ in range(6):
        g.rpc("key Escape"); time.sleep(1.5)
        st = g.status()
        if st.get("map_open") == 0 and st.get("menucover") == 0:
            closed = True
            break
    g.check("native map fully closed", closed, str(g.status()))

    # project the SAME point with the map CLOSED — must still work + return the SAME values (the VM cache
    # persists past close). This is the core residency guarantee the vmap projection + M5 cull rely on.
    closed_uv, closed_r = _proj(g, 60, 100, 100)
    g.check("proj STILL works with the map CLOSED (VM cached + persists)", closed_uv is not None, closed_r)
    if open_uv and closed_uv:
        du = abs(open_uv[0] - closed_uv[0]) + abs(open_uv[1] - closed_uv[1])
        g.check("projection IDENTICAL open vs closed (converter residency)", du < 0.01,
                f"open={open_uv} closed={closed_uv} du={du}")


SWEEP = _test  # single-boot, self-loads


if __name__ == "__main__":
    run_test(_test)
