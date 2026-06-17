#!/usr/bin/env python3
"""Map-coverage profiler for MapForGoblins.

Answers "what does the game have vs what did we put a marker on?" per map, so
missing-icon gaps (and dungeons that produce no markers at all) are visible
and quantified instead of guessed.

Inputs (all committed under data/):
  - generated/goblin_map_data.cpp : the baked marker set (areaNo/grid/category)
  - data/msb_entity_index.json    : every placed entity + its map (positions)
  - data/dungeon_to_world... via src LEGACY_CONV : which sub-maps project to overworld

Output: per-map table [map | placed-entities | markers | projects? | status],
plus a global summary. Pass a map prefix (e.g. m32_07) to dump that map's detail.

Usage:  tools/coverage_profiler.py [map_prefix]
"""
import json
import os
import re
import sys
from collections import Counter, defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "data")
GEN = os.path.join(ROOT, "src", "generated")


def load_markers():
    """Parse goblin_map_data.cpp into per-entry dicts (areaNo, gridXNo, category)."""
    path = os.path.join(GEN, "goblin_map_data.cpp")
    txt = open(path, encoding="utf-8", errors="replace").read()
    entries = []
    # Each entry block: .areaNo = N, .gridXNo = N, ... }, Category::X, ...
    for blk in txt.split("// Row ID ")[1:]:
        a = re.search(r"\.areaNo\s*=\s*(\d+)", blk)
        gx = re.search(r"\.gridXNo\s*=\s*(\d+)", blk)
        cat = re.search(r"Category::(\w+)", blk)
        if not a:
            continue
        entries.append({
            "areaNo": int(a.group(1)),
            "gridXNo": int(gx.group(1)) if gx else 0,
            "category": cat.group(1) if cat else "?",
        })
    return entries


def map_key(area, gx):
    """(areaNo, gridXNo) -> 'mAA_GX' prefix matching msb_entity_index map ids."""
    return f"m{area:02d}_{gx:02d}"


def load_legacy_conv_keys():
    """Sub-maps (mAA_GX) the game projects onto the overworld (from LEGACY_CONV)."""
    path = os.path.join(GEN, "goblin_legacy_conv.hpp")
    txt = open(path).read()
    keys = set()
    for m in re.finditer(r"\{\s*(\d+),\s*(\d+),", txt):
        keys.add(map_key(int(m.group(1)), int(m.group(2))))
    return keys


def main():
    focus = sys.argv[1] if len(sys.argv) > 1 else None

    markers = load_markers()
    idx = json.load(open(os.path.join(DATA, "msb_entity_index.json")))
    conv_keys = load_legacy_conv_keys()

    # markers per map prefix
    mk_by_map = Counter(map_key(m["areaNo"], m["gridXNo"]) for m in markers)
    # placed entities per map prefix, split by kind
    ent_by_map = defaultdict(Counter)
    for v in idx.values():
        pref = "_".join(v["map"].split("_")[:2])
        ent_by_map[pref][v["kind"]] += 1

    OVERWORLD = {"m60", "m61", "m12"}  # have their own in-game map page/layer

    if focus:
        print(f"=== detail: maps matching {focus} ===")
        for pref in sorted(set(list(mk_by_map) + list(ent_by_map))):
            if not pref.startswith(focus):
                continue
            ents = ent_by_map[pref]
            print(f"{pref}: markers={mk_by_map[pref]}  "
                  f"entities(asset={ents['asset']},enemy={ents['enemy']})  "
                  f"projects={pref in conv_keys}")
        return

    # global per-map table, dungeon maps sorted by entity count
    print(f"{'map':10} {'entities':>8} {'markers':>7} {'proj':>5}  status")
    print("-" * 60)
    rows = []
    for pref in sorted(set(list(mk_by_map) + list(ent_by_map))):
        area = pref[:3]
        if area in OVERWORLD:
            continue  # overworld/underground — has its own page
        ents = ent_by_map[pref]
        nent = ents["asset"] + ents["enemy"]
        nmk = mk_by_map[pref]
        proj = pref in conv_keys
        if nmk == 0 and nent > 0:
            status = "NO MARKERS"
        elif not proj and nmk > 0:
            status = "markers but NOT PROJECTED (invisible)"
        elif proj and nmk > 0:
            status = "ok (projected)"
        else:
            status = "-"
        rows.append((nent, pref, nmk, proj, status))
    for nent, pref, nmk, proj, status in sorted(rows, reverse=True):
        print(f"{pref:10} {nent:8} {nmk:7} {str(proj):>5}  {status}")

    # summary
    total_mk = sum(mk_by_map.values())
    dungeon_no_marker = sum(1 for nent, p, nmk, pr, s in rows if s == "NO MARKERS")
    dungeon_not_proj = sum(1 for *_, s in rows if "NOT PROJECTED" in s)
    print("\n=== summary ===")
    print(f"total markers (all maps): {total_mk}")
    print(f"dungeon sub-maps with placed entities but ZERO markers: {dungeon_no_marker}")
    print(f"dungeon sub-maps with markers but no projection (invisible): {dungeon_not_proj}")
    print("Run with a map prefix (e.g. m32_07) for per-map detail.")


if __name__ == "__main__":
    main()
