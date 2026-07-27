#!/usr/bin/env python3
"""Cross-check the markers our map DRAWS against an Elden Ring Randomizer spoiler log.

An external ground truth: the seed's own spoiler log says what was placed where, so a diff against
our live marker set catches coverage gaps, phantoms and duplication that no self-consistency check
can (re-deriving from the same regulation we already read would just confirm our own reader).

    # 1. in-game, with markers built (open the native map once so the disk-loot pass runs)
    python tools/mfg.py rpc vmap dump_markers C:/path/markers.csv
    # 2. offline
    python tools/audit_markers_vs_spoiler.py markers.csv <game_data_dir> [spoiler.txt]

<game_data_dir> = the randomizer/mod dir (regulation.bin + msg/ + map/). With no spoiler path the
NEWEST log in <game_data_dir>/spoiler_logs is used.

SPOILER HYGIENE: this prints ONLY counts, families and categories — never an item name or a
placement — so it can be run for a player who is mid-run and does not want to be spoiled. Keep any
new output aggregate; if you need per-item detail while debugging, write it to a file.

Method + the first run's findings: docs/memory/process/spoiler-log-marker-audit.md
"""
import csv
import json
import os
import re
import sys
import tempfile
from collections import Counter, defaultdict
from pathlib import Path

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE))
import extract_all_items as E  # noqa: E402  (.NET / SoulsFormats bootstrap)
from System import Array, Object  # noqa: E402
from System.IO import File as SysFile  # noqa: E402

# marker name_id -> FMG family, replicating goblin_messages.cpp decode_textid
BANDS = [
    (1600000000, 1700000000, ("NpcName",), 700000000),
    (950000000, 960000000, ("BloodMsg",), 950000000),
    (900000000, 950000000, ("TutorialTitle",), 900000000),
    (800000000, 900000000, ("ActionButtonText",), 800000000),
    (700000000, 800000000, ("NpcName",), 700000000),
    (600000000, 700000000, ("EventTextForMap",), 600000000),
    (500000000, 600000000, ("GoodsName",), 500000000),
    (400000000, 500000000, ("GemName", "ArtsName"), 400000000),
    (300000000, 400000000, ("AccessoryName",), 300000000),
    (200000000, 300000000, ("ProtectorName",), 200000000),
    (100000000, 200000000, ("WeaponName",), 100000000),
    (50000000, 100000000, ("WeaponName",), 0),
]
ITEM_FAMS = ("GoodsName", "WeaponName", "ProtectorName", "AccessoryName", "GemName", "ArtsName")
CATS = [c["enum"] for c in json.load(open(HERE.parent / "data" / "categories.json", encoding="utf-8"))]
_seq = [0]


def _read(method, data, suffix):
    """SoulsFormats keeps a mapped section open on the temp file it reads, so every read needs a
    FRESH path — reusing one silently fails the SECOND time (it burned a run of this audit)."""
    _seq[0] += 1
    tmp = os.path.join(tempfile.gettempdir(), f"{os.getpid()}_{_seq[0]}_mfgaudit{suffix}")
    SysFile.WriteAllBytes(tmp, data.ToArray() if hasattr(data, "ToArray") else data)
    return method.Invoke(None, Array[Object]([tmp]))


def load_fmgs(src, lang="engus"):
    fams = defaultdict(dict)
    for c in sorted((src / "msg" / lang).glob("*.msgbnd.dcx")):
        try:
            bnd = _read(E._bnd4_read, E.SoulsFormats.DCX.Decompress(str(c)), ".msgbnd")
        except Exception:
            continue
        for f in bnd.Files:
            fam = re.sub(r"_dlc\d+$", "", str(f.Name).split("\\")[-1].replace(".fmg", ""))
            for e in _read(E._fmg_read, f.Bytes, ".fmg").Entries:
                t = str(e.Text) if e.Text is not None else ""
                if t.strip():
                    fams[fam].setdefault(int(e.ID), t.strip())
    return fams


def resolve(nid, fams):
    for lo, hi, prefixes, sub in BANDS:
        if lo <= nid < hi:
            for p in prefixes:
                t = fams.get(p, {}).get(nid - sub)
                if t:
                    return t, p
            return None, prefixes[0]
    return fams.get("PlaceName", {}).get(nid), "PlaceName"


def parse_spoiler(path, names):
    """-> [(item, region, klass, detail)]  klass in {world, shop, drop}.

    Entry shape: "<Item>[ [qty]] in <Region>, <detail>: <prose>." with indented sub-lines
    "(cost: N)" for shop stock and "Drop chance for X: N%" for drop tables — those sub-lines are
    the ONLY reliable way to tell a real world placement from a shop/drop entry.
    """
    L = open(path, encoding="utf-8", errors="replace").read().splitlines()
    s = next(i for i, l in enumerate(L) if l.startswith("-- Spoilers:")) + 1
    e = next(i for i, l in enumerate(L) if l.startswith("-- End of item spoilers"))
    ents = []
    for l in L[s:e]:
        if not l.strip():
            continue
        if l.startswith(" "):
            if ents:
                ents[-1][1].append(l.strip())
        elif ": " in l:
            ents.append([l, []])

    def prefix(x):
        for i in range(len(x), 0, -1):
            if x[:i].strip() in names:
                return x[:i].strip()
        return None

    out, unparsed = [], 0
    for head, subs in ents:
        raw = head.split(": ", 1)[0].strip()
        # RAW first: some real item names END in "[N]" (Golden Rune [7]), so stripping a trailing
        # bracket as a quantity up-front would lose them and ~600 of our markers with them.
        item, lhs = prefix(raw), raw
        if not item:
            lhs = re.sub(r"\s*\[\d+\]$", "", raw).strip()
            item = prefix(lhs)
        if not item:
            unparsed += 1
            continue
        rest = re.sub(r"^\d+x\s*", "", lhs[len(item):].strip())
        m = re.match(r"^in\s+(.+)$", rest)
        loc = m.group(1) if m else ""
        k = ("shop" if any(x.startswith("(cost:") for x in subs)
             else "drop" if any(x.startswith("Drop chance") for x in subs) else "world")
        out.append((item, loc.split(",")[0].strip() if loc else "(none)", k,
                    loc.split(",", 1)[1].strip() if "," in loc else ""))
    return out, unparsed


def main():
    csv_path, src = Path(sys.argv[1]), Path(sys.argv[2])
    if len(sys.argv) > 3:
        spoiler = Path(sys.argv[3])
    else:
        logs = sorted((src / "spoiler_logs").glob("*.txt"), key=lambda p: p.stat().st_mtime)
        if not logs:
            sys.exit("no spoiler log found — pass one explicitly")
        spoiler = logs[-1]
    print(f"spoiler log: {spoiler.name}")

    fams = load_fmgs(src)
    names, fam_of = {}, {}
    for fam in ITEM_FAMS:
        for t in fams.get(fam, {}).values():
            names[t] = fam
            fam_of.setdefault(t, fam)
    rec, unparsed = parse_spoiler(spoiler, names)

    rows = list(csv.DictReader(open(csv_path, encoding="utf-8")))
    map_items, map_cat, per_cat, unres = Counter(), defaultdict(set), Counter(), Counter()
    for r in rows:
        nid = int(r["name_id"])
        ci = int(r["category"])
        cn = CATS[ci] if 0 <= ci < len(CATS) else str(ci)
        per_cat[cn] += 1
        t, fam = resolve(nid, fams)
        if t is None:
            band = next((f"{lo//1000000}M" for lo, hi, _, _ in BANDS if lo <= nid < hi), "<50M")
            unres[(band, cn)] += 1
            continue
        if fam in ITEM_FAMS:
            map_items[t] += 1
            map_cat[t].add(cn)

    print(f"\nMAP      {len(rows)} markers / {len(per_cat)} categories | "
          f"item-named {sum(map_items.values())} ({len(map_items)} distinct)")
    print(f"  name_id with NO text, by (band, category): {dict(unres.most_common(8))}")
    off = [r for r in rows if abs(float(r["worldX"])) > 40000 or abs(float(r["worldZ"])) > 40000]
    near0 = [r for r in rows if abs(float(r["worldX"])) < 200 and abs(float(r["worldZ"])) < 200]
    print(f"  out of bounds: {len(off)}   projection-failed near origin: {len(near0)}")
    print(f"SPOILER  {len(rec)} placements ({unparsed} unparsed), {len({r[0] for r in rec})} distinct")

    for k in ("world", "shop", "drop"):
        it = {r[0] for r in rec if r[2] == k}
        sh = {i for i in it if map_items.get(i)}
        print(f"  {k:5s}: {len(it):4d} items, on our map {len(sh):4d} ({len(sh)/max(1,len(it)):5.1%})")

    world = [(r[0], r[3]) for r in rec if r[2] == "world"]
    det = {i for i, d in world if d}
    nod = {i for i, d in world if not d} - det
    cov = lambda S: sum(1 for i in S if map_items.get(i)) / max(1, len(S))
    print(f"\nWORLD by how precise the log's location is (the detailed subset is the TRUSTED signal;"
          f"\n      region-only entries mix in drops/rewards that are not markers by design)")
    print(f"  with a location detail : {len(det):4d} items, coverage {cov(det):5.1%}")
    print(f"  region only            : {len(nod):4d} items, coverage {cov(nod):5.1%}")
    miss = {i for i in det | nod if not map_items.get(i)}
    both = miss & {r[0] for r in rec if r[2] in ("shop", "drop")}
    print(f"  absent from the map    : {len(miss)} ({len(both)} also listed as shop/drop elsewhere)"
          f"  families: {dict(Counter(fam_of.get(i, '?') for i in miss).most_common(6))}")

    allsp = {r[0] for r in rec}
    ph = {i for i in map_items if i not in allsp}
    print(f"\nPHANTOM  {len(ph)} items drawn, never in the log ({sum(map_items[i] for i in ph)} markers)")
    pc = Counter()
    for i in ph:
        for c in map_cat[i]:
            pc[c] += 1
    print(f"  by category: {dict(pc.most_common(8))}")
    print("  NB an item FAMILY with 0 log placements simply is not randomized in this seed, so its"
          "\n     'phantoms' are correct vanilla placements — check before filing a bug:")
    for fam in ITEM_FAMS:
        print(f"     {fam:15s} log placements: {sum(1 for r in rec if fam_of.get(r[0]) == fam)}")

    placed = Counter(i for i, _, k, _d in rec if k == "world")
    over = [(i, map_items[i], placed[i]) for i in map_items if placed[i] and map_items[i] > 2 * placed[i] + 2]
    oc = Counter()
    for i, _, _ in over:
        for c in map_cat[i]:
            oc[c] += 1
    print(f"\nDUPLIC   {len(over)} items drawn >2x+2 their placements — by category: "
          f"{dict(oc.most_common(8))}")
    print("  (harvestable categories are EXPECTED here: the log lists the lot once, the world holds"
          "\n   hundreds of nodes)")


if __name__ == "__main__":
    main()
