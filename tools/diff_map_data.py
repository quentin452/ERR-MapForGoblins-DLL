"""Version-drift diff for the baked world-map marker set (MAP_ENTRIES).

When ERR ships an update, src/generated/goblin_map_data.cpp is re-baked from the
new regulation.bin. This tool snapshots that file and diffs two versions, so we
catch markers ADDED / REMOVED / MOVED / RECATEGORIZED / RE-ICONED across ERR
updates (coverage drift) before they silently break the map. Stdlib only.

Usage:
  # snapshot the current bake to a versioned JSON
  python tools/diff_map_data.py snapshot src/generated/goblin_map_data.cpp \
      data/map_snapshots/err-2.2.9.6.json

  # diff two bakes (each arg may be a .cpp OR a .json snapshot)
  python tools/diff_map_data.py diff data/map_snapshots/err-2.2.9.6.json \
      src/generated/goblin_map_data.cpp

  # vanilla vs ERR (just two .cpp files)
  python tools/diff_map_data.py diff src/generated_vanilla/goblin_map_data.cpp \
      src/generated/goblin_map_data.cpp
"""
import argparse
import io
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8")

# One MapEntry literal:
#   {2000000ull, { .iconId = 371, .areaNo = 10, .posX = -90.463f, ... },
#    Category::ReforgedRunePieces, 0, 9000, "AEG099_821_9000", 0u, 0},
ENTRY_RE = re.compile(
    r"\{(\d+)ull,\s*\{(.*?)\}\s*,\s*Category::(\w+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,"
    r'\s*("(?:[^"\\]*)"|nullptr)\s*,\s*(\d+)u?\s*,\s*(\d+)\s*\}',
    re.DOTALL,
)
FIELD_RE = re.compile(r"\.(\w+)\s*=\s*([^,]+?)\s*,")

# MapEntry.data fields we track for drift (others are render-only flags).
DATA_FIELDS = ("iconId", "areaNo", "gridXNo", "gridZNo", "posX", "posZ",
               "textId1", "textId2", "selectMinZoomStep")
POS_EPSILON = 0.5  # marker units; below this a pos change isn't a real "move"


def _coerce(v):
    v = v.strip()
    if v == "true":
        return 1
    if v == "false":
        return 0
    if v.endswith("f"):
        v = v[:-1]
    try:
        return int(v)
    except ValueError:
        try:
            return float(v)
        except ValueError:
            return v


def parse_cpp(path):
    """Parse goblin_map_data.cpp -> list of flat entry dicts."""
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    out = []
    for m in ENTRY_RE.finditer(text):
        row_id, inner, cat, geom, namesuf, objname, lot_id, lot_type = m.groups()
        fields = {k: _coerce(val) for k, val in FIELD_RE.findall(inner)}
        rec = {
            "row_id": int(row_id),
            "category": cat,
            "object_name": None if objname == "nullptr" else objname.strip('"'),
            "geom_slot": int(geom),
            "name_suffix": int(namesuf),
            "lotId": int(lot_id),
            "lotType": int(lot_type),
        }
        for f in DATA_FIELDS:
            rec[f] = fields.get(f, 0)  # designated-init: absent field == default 0
        out.append(rec)
    return out


def load(path):
    p = Path(path)
    if p.suffix == ".json":
        return json.loads(p.read_text(encoding="utf-8"))
    return parse_cpp(p)


def key_of(rec):
    """Stable identity across versions: prefer the MSB object name; else row_id."""
    return rec["object_name"] if rec["object_name"] else f"row:{rec['row_id']}"


def index(entries):
    idx = {}
    for r in entries:
        k = key_of(r)
        idx.setdefault(k, []).append(r)  # list: a name CAN repeat (rare)
    return idx


def moved(a, b):
    return (a["areaNo"] != b["areaNo"]
            or abs(a["posX"] - b["posX"]) > POS_EPSILON
            or abs(a["posZ"] - b["posZ"]) > POS_EPSILON
            or a["gridXNo"] != b["gridXNo"] or a["gridZNo"] != b["gridZNo"])


def cmd_snapshot(args):
    entries = parse_cpp(args.cpp)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(entries, ensure_ascii=False), encoding="utf-8")
    print(f"snapshot: {len(entries)} entries -> {out}")


def cmd_diff(args):
    old, new = load(args.old), load(args.new)
    oidx, nidx = index(old), index(new)
    okeys, nkeys = set(oidx), set(nidx)

    added = [r for k in (nkeys - okeys) for r in nidx[k]]
    removed = [r for k in (okeys - nkeys) for r in oidx[k]]
    moves, recat, reicon = [], [], []
    for k in okeys & nkeys:
        # pair positionally; if counts differ the surplus shows as added/removed
        a_list, b_list = oidx[k], nidx[k]
        for a, b in zip(a_list, b_list):
            if a["category"] != b["category"]:
                recat.append((a, b))
            if a["iconId"] != b["iconId"]:
                reicon.append((a, b))
            if moved(a, b):
                moves.append((a, b))
        added += b_list[len(a_list):]
        removed += a_list[len(b_list):]

    print(f"OLD {args.old}: {len(old)} entries")
    print(f"NEW {args.new}: {len(new)} entries   (net {len(new) - len(old):+d})")
    print(f"\n  + added         {len(added)}")
    print(f"  - removed       {len(removed)}")
    print(f"  ~ moved         {len(moves)}")
    print(f"  ~ recategorized {len(recat)}")
    print(f"  ~ re-iconed     {len(reicon)}")

    def by_cat_area(recs, get=lambda r: r):
        c = Counter((get(r)["category"], get(r)["areaNo"]) for r in recs)
        for (cat, area), n in sorted(c.items(), key=lambda x: -x[1]):
            print(f"      {n:5d}  {cat:28s} area {area}")

    if added:
        print("\n  ADDED by category/area:")
        by_cat_area(added)
    if removed:
        print("\n  REMOVED by category/area:")
        by_cat_area(removed)
    if recat:
        print("\n  RECATEGORIZED (sample 20):")
        for a, b in recat[:20]:
            print(f"      {key_of(a):24s} {a['category']} -> {b['category']}")
    if moves:
        print("\n  MOVED (sample 20):")
        for a, b in moves[:20]:
            print(f"      {key_of(a):24s} area {a['areaNo']}->{b['areaNo']}  "
                  f"({a['posX']:.1f},{a['posZ']:.1f})->({b['posX']:.1f},{b['posZ']:.1f})")

    if args.json:
        rep = {
            "old": str(args.old), "new": str(args.new),
            "counts": {"old": len(old), "new": len(new),
                       "added": len(added), "removed": len(removed),
                       "moved": len(moves), "recategorized": len(recat),
                       "reiconed": len(reicon)},
            "added": added, "removed": removed,
            "moved": [{"key": key_of(a), "old": a, "new": b} for a, b in moves],
            "recategorized": [{"key": key_of(a), "from": a["category"],
                               "to": b["category"]} for a, b in recat],
        }
        Path(args.json).write_text(json.dumps(rep, ensure_ascii=False, indent=1),
                                   encoding="utf-8")
        print(f"\nfull report -> {args.json}")

    # non-zero exit if anything drifted (CI-friendly)
    return 1 if (added or removed or moves or recat or reicon) else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("snapshot", help="parse a .cpp bake -> JSON snapshot")
    s.add_argument("cpp")
    s.add_argument("out")
    s.set_defaults(func=cmd_snapshot)

    d = sub.add_parser("diff", help="diff two bakes (.cpp or .json each)")
    d.add_argument("old")
    d.add_argument("new")
    d.add_argument("--json", help="write full JSON report here")
    d.set_defaults(func=cmd_diff)

    args = ap.parse_args()
    rc = args.func(args)
    sys.exit(rc or 0)


if __name__ == "__main__":
    main()
