#!/usr/bin/env python3
"""E2E for the in-game World Editor (slices 2+3) + the `help` verb.

Boots ER, loads a save, then:
  1. `help` returns the verb list (in-band discovery).
  2. Discovers two pickup assets whose markers resolve to DIFFERENT items (scan a small aegRow band
     via loot_at).
  3. Reads asset A's pickUpItemLotParamId (param_getf) and cross-checks it == the lot loot_at reports.
  4. (Slice 2) Repoints A at asset B's lot (param_setf AssetEnvironmentGeometryParam A
     pickUpItemLotParamId lotB) — the exact write the panel's "Repoint asset to this lot" button makes.
  5. Re-reads: param_getf == lotB, and loot_at A now reports B's item.
  6. Restores A to its original lot (edit is live-only, not persisted, but leave it clean anyway).
  7. (Slice 3) Round-trips ItemLotParam_map lotItemId02 on lotA (the panel's per-slot selector write:
     read original → write sentinel → read back → restore), proving the new lotItemId02..08 registry
     entries + the param_get_field bridge.

Run: python tools/mfg.py test world_editor   (or: python tools/rpc_tests/test_world_editor.py)
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

    # --- Slice 3: per-slot edit (the new lotItemId02..08 registry entries + param_get bridge) ---
    # Round-trip slot 2 on lotA: read original, write a sentinel, read it back, restore. Proves the
    # panel's slot selector write/read path (ItemLotParam_map.lotItemId0N by name).
    orig2 = parse_getf(g.rpc(f"param_getf ItemLotParam_map {lotA} lotItemId02"))
    g.check("param_getf slot 2 readable (new registry entry)", orig2 is not None,
            f"orig2={orig2}")
    if orig2 is not None:
        sentinel = 424242
        sr = g.rpc(f"param_setf ItemLotParam_map {lotA} lotItemId02 {sentinel}")
        g.check("param_setf slot 2 returned ok", sr.startswith("ok"), sr)
        got2 = parse_getf(g.rpc(f"param_getf ItemLotParam_map {lotA} lotItemId02"))
        g.check("slot 2 reads back the sentinel", got2 == sentinel, f"got2={got2}")
        g.rpc(f"param_setf ItemLotParam_map {lotA} lotItemId02 {orig2}")
        back2 = parse_getf(g.rpc(f"param_getf ItemLotParam_map {lotA} lotItemId02"))
        g.check("slot 2 restored to original", back2 == orig2, f"back2={back2} orig2={orig2}")

    # --- Slice 5: CLONE a lot + refresh_markers LotReader reset ---
    # Clone lotB (a known map lot) to a fresh id, repoint asset B at the copy, and prove:
    #   * BEFORE refresh_markers the cloned lot is INVISIBLE to the resolver (stale cached LotReader);
    #   * AFTER refresh_markers (which resets the LotReader) it resolves to B's item.
    # This is the whole point of the v2 reset — without it a cloned lot never resolves on the map.
    new_lot = 900900900
    cl = g.rpc(f"param_clone ItemLotParam_map {lotB} {new_lot}")
    g.check("param_clone lotB -> new_lot ok", cl.startswith("ok") and "new_row_present=1" in cl, cl)
    if cl.startswith("ok"):
        # Warm the resolver on the new lot's would-be marker via a repoint, BEFORE any reset.
        g.rpc(f"param_setf AssetEnvironmentGeometryParam {aegB} pickUpItemLotParamId {new_lot}")
        lot_pre, tid_pre, _ = parse_loot_at(g.rpc(f"loot_at {aegB}"))
        g.check("repoint took (loot_at lot == new_lot)", lot_pre == new_lot, f"lot_pre={lot_pre}")
        g.check("cloned lot INVISIBLE before refresh (stale reader)", tid_pre != tidB,
                f"tid_pre={tid_pre} tidB={tidB}")
        # Reset the LotReader (v2) and re-resolve — now the clone is visible.
        g.rpc("refresh_markers")
        lot_post, tid_post, _ = parse_loot_at(g.rpc(f"loot_at {aegB}"))
        g.check("cloned lot resolves B's item AFTER refresh (reader reset)", tid_post == tidB,
                f"tid_post={tid_post} tidB={tidB}")
        # restore asset B to its original lot
        g.rpc(f"param_setf AssetEnvironmentGeometryParam {aegB} pickUpItemLotParamId {lotB}")

    # --- Slice 6: picker enumeration (we_scan backs the F1 Browse list) ---
    sc = g.rpc("we_scan")
    m = re.search(r"assets=(\d+) goods=(\d+)", sc)
    na, ng = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
    g.check("we_scan found pickup assets", na > 0, sc)
    g.check("we_scan found named goods", ng > 0, sc)


if __name__ == "__main__":
    run_test(_test)
