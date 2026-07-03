#!/usr/bin/env python3
"""E2E for World Editor slice 2 (repoint a loot asset to another lot) + the `help` verb.

Boots ER, loads a save, then:
  1. `help` returns the verb list (in-band discovery).
  2. Discovers two pickup assets whose markers resolve to DIFFERENT items (scan a small aegRow band
     via loot_at).
  3. Reads asset A's pickUpItemLotParamId (param_getf) and cross-checks it == the lot loot_at reports.
  4. Repoints A at asset B's lot (param_setf AssetEnvironmentGeometryParam A pickUpItemLotParamId lotB)
     — the exact write the panel's "Repoint asset to this lot" button makes.
  5. Re-reads: param_getf == lotB, and loot_at A now reports B's item name.
  6. Restores A to its original lot (edit is live-only, not persisted, but leave it clean anyway).

Run: python tools/mfg.py test world_editor_slice2   (or: python tools/rpc_tests/test_world_editor_slice2.py)
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from mfg_session import run_test  # noqa: E402


def parse_loot_at(reply):
    """`ok aeg=N lot=N item_textid=N name='...'` -> (lot, textid, name) or (0, -1, '')."""
    m = re.search(r"lot=(\d+) item_textid=(-?\d+) name='([^']*)'", reply)
    if not m:
        return 0, -1, ""
    return int(m.group(1)), int(m.group(2)), m.group(3)


def parse_getf(reply):
    """`ok Param[row].field=VALUE` (VALUE is a double string like 997230.000000) -> int or None."""
    m = re.search(r"\.[A-Za-z0-9_]+=(-?\d+)", reply)
    return int(m.group(1)) if m else None


def _test(g):
    g.load_save()

    # 1. help
    h = g.rpc("help")
    g.check("help lists loot_at+refresh_markers+param_setf",
            all(v in h for v in ("loot_at", "refresh_markers", "param_setf")), h)

    # 2. discover two pickup assets whose markers resolve to DIFFERENT items. Compare by textid
    #    (item identity), not name — many valid loot lots have an empty FMG name off this chain.
    found = []  # (aegRow, lot, textid, name)
    seen_tids = set()
    for aeg in range(99000, 99200):
        lot, tid, name = parse_loot_at(g.rpc(f"loot_at {aeg}"))
        if lot and tid >= 0 and tid not in seen_tids:
            found.append((aeg, lot, tid, name))
            seen_tids.add(tid)
        if len(found) >= 2:
            break
    if not g.check("found 2 distinct-item pickup assets", len(found) >= 2, detail=str(found)):
        return
    (aegA, lotA, tidA, nameA), (aegB, lotB, tidB, nameB) = found[0], found[1]
    g._log(f"[slice2] A: aeg={aegA} lot={lotA} tid={tidA} '{nameA}'  |  "
           f"B: aeg={aegB} lot={lotB} tid={tidB} '{nameB}'")

    # 3. param_getf pickUpItemLotParamId(A) == lotA (loot_at and the direct field agree)
    got_a = parse_getf(g.rpc(f"param_getf AssetEnvironmentGeometryParam {aegA} pickUpItemLotParamId"))
    g.check("param_getf(A) == loot_at lotA", got_a == lotA, f"getf={got_a} lotA={lotA}")

    # 4. repoint A -> lotB (the panel's "Repoint asset to this lot" write)
    setr = g.rpc(f"param_setf AssetEnvironmentGeometryParam {aegA} pickUpItemLotParamId {lotB}")
    g.check("param_setf repoint returned ok", setr.startswith("ok"), setr)

    # 5a. field now reads lotB
    got_b = parse_getf(g.rpc(f"param_getf AssetEnvironmentGeometryParam {aegA} pickUpItemLotParamId"))
    g.check("param_getf(A) == lotB after repoint", got_b == lotB, f"getf={got_b} lotB={lotB}")

    # 5b. loot_at A now resolves to B's item (the live map-build chain follows the repoint)
    lot_after, tid_after, _ = parse_loot_at(g.rpc(f"loot_at {aegA}"))
    g.check("loot_at(A) lot == lotB after repoint", lot_after == lotB, f"lot_after={lot_after}")
    g.check("loot_at(A) textid == tidB after repoint", tid_after == tidB,
            f"tid_after={tid_after} tidB={tidB}")

    # 6. restore
    g.rpc(f"param_setf AssetEnvironmentGeometryParam {aegA} pickUpItemLotParamId {lotA}")
    lot_restored, _, _ = parse_loot_at(g.rpc(f"loot_at {aegA}"))
    g.check("loot_at(A) restored to lotA", lot_restored == lotA, f"lot_restored={lot_restored}")


if __name__ == "__main__":
    run_test(_test)
