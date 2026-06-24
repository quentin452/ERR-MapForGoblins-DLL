#!/usr/bin/env python3
"""#2 coverage probe — can the 748 position-less lots get a world position from
the active mod's EMEVD + MSB (no bake)?

For each position-less lot (x==y==z==0 in items_database.json), search the
decompiled ERR EMEVD JS for the lotId literal, collect co-occurring integers
that resolve to a placed MSB Part (enemy/asset) OR Region (POINT) position in
the same map, and report coverage by class. The ERR-custom m60_44_60 cluster
(~318, locationless grants) is reported separately.

Needs the DarkScript3 JS at D:\tools\emevd_js\err (see darkscript3-emevd-decompile
memory). MSB region/part index is built live via SoulsFormats over the maps the
position-less lots live in.

Run: PYTHONPATH="C:/Users/iamacat/AppData/Local/Temp/mfg_aux" MFG_PROFILE=err \
     py -3.14 tools/resolve_emevd_positions.py
"""
import sys, io, os, re, json, tempfile
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pathlib import Path
from collections import Counter, defaultdict
from pythonnet import load
load('coreclr')
import clr
clr.AddReference(str(config.SOULSFORMATS_DLL))
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
import SoulsFormats

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
_str = SysType.GetType('System.String')
_msbe_read = asm.GetType('SoulsFormats.MSBE').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str]), None)

JS_DIR = Path(r'D:\tools\emevd_js\err')

def read_msb(path):
    data = SoulsFormats.DCX.Decompress(str(path)).ToArray()
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_re.msb')
    SysFile.WriteAllBytes(tmp, data)
    msb = _msbe_read.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    return msb

def index_map(path):
    """entityID -> {kind,pos} for every placed Part and Region with an EntityID."""
    idx = {}
    msb = read_msb(path)
    for cat in ('MapPieces', 'Enemies', 'Players', 'Collisions', 'DummyAssets',
                'DummyEnemies', 'ConnectCollisions', 'Assets'):
        col = getattr(msb.Parts, cat, None) or []
        for p in col:
            eid = int(getattr(p, 'EntityID', 0) or 0)
            if eid > 0:
                idx.setdefault(eid, ('part', float(p.Position.X), float(p.Position.Y),
                                     float(p.Position.Z)))
            for g in (getattr(p, 'EntityGroupIDs', None) or []):
                gi = int(g)
                if gi > 0:
                    idx.setdefault(gi, ('part-grp', float(p.Position.X),
                                        float(p.Position.Y), float(p.Position.Z)))
    # Regions: msb.Regions has many subtype collections.
    for rn in dir(msb.Regions):
        if rn.startswith('_'):
            continue
        try:
            col = getattr(msb.Regions, rn)
        except Exception:
            continue
        if not hasattr(col, '__iter__'):
            continue
        try:
            for r in col:
                try:
                    eid = int(getattr(r, 'EntityID', 0) or 0)
                    if eid > 0:
                        idx.setdefault(eid, ('region', float(r.Position.X),
                                             float(r.Position.Y), float(r.Position.Z)))
                except Exception:
                    pass
        except Exception:
            pass
    return idx

def lot_map_prefix(map_name):
    # 'm11_00_00_00' -> 'm11_00'; 'm60_44_60_00' -> 'm60_44_60'
    return map_name

INT_RE = re.compile(r'\d{4,}')

def main():
    db = json.load(open(config.PROJECT_DIR / 'data' / 'items_database.json'))
    posless = [r for r in db if r['x'] == 0 and r['y'] == 0 and r['z'] == 0]
    custom = [r for r in posless if r['map'] == 'm60_44_60_00']
    targets = [r for r in posless if r['map'] != 'm60_44_60_00']
    print(f"position-less: {len(posless)} (ERR-custom m60_44_60: {len(custom)}, "
          f"to-resolve: {len(targets)})")

    # Build the MSB index for every map the targets live in (+ same supertile).
    maps = sorted(set(r['map'] for r in targets))
    msb_dir = config.require_err_mod_dir() / 'map' / 'MapStudio'
    index = {}  # map_name -> {eid: (kind,x,y,z)}
    for m in maps:
        p = msb_dir / (m + '.msb.dcx')
        if p.exists():
            try:
                index[m] = index_map(p)
            except Exception as ex:
                index[m] = {}
        else:
            index[m] = {}
    print(f"indexed {len(maps)} maps; entity ids per map (sample): " +
          ", ".join(f"{m}:{len(index[m])}" for m in maps[:6]))

    # Load the ENTIRE EMEVD JS corpus once (every map + common/common_func): a
    # lot can be awarded from common.emevd or a sibling map, so per-map search
    # under-counts. Keep all lines for literal co-arg extraction.
    all_lines = []
    for f in sorted(JS_DIR.glob('*.emevd.dcx.js')):
        all_lines.extend(f.read_text(encoding='utf-8', errors='replace').splitlines())
    print(f"loaded {len(all_lines)} EMEVD JS lines from {JS_DIR}")

    # Global entity index (union of all maps) for co-arg resolution; same-map is
    # preferred via the per-map idx first.
    gindex = {}
    for m, idx in index.items():
        for e, v in idx.items():
            gindex.setdefault(e, v)

    res = Counter()
    resolved_rows = []
    examples = defaultdict(list)
    for r in targets:
        lot = r['itemLotId']
        m = r['map']
        idx = index.get(m, {})
        # 1) lotId itself collides with a placed entity (e.g. a character) — drop
        #    at the enemy/boss position.
        if lot in idx:
            res['lot==entity'] += 1
            k, x, y, z = idx[lot]
            resolved_rows.append((lot, m, 'lot==entity', k, x, y, z))
            if len(examples['lot==entity']) < 5:
                examples['lot==entity'].append((lot, m, k))
            continue
        # 2) search the WHOLE EMEVD corpus for lines mentioning the lot literal
        lines = [ln for ln in all_lines if str(lot) in ln]
        coargs = set()
        for ln in lines:
            for tok in INT_RE.findall(ln):
                v = int(tok)
                if v != lot and (v in idx or v in gindex):
                    coargs.add(v)
        if coargs:
            def rank(e):
                k = (idx.get(e) or gindex.get(e))[0]
                samemap = 0 if e in idx else 1
                return (samemap, {'region': 0, 'part': 1, 'part-grp': 2}.get(k, 3))
            best = sorted(coargs, key=rank)[0]
            k, x, y, z = idx.get(best) or gindex[best]
            res['resolved-coarg'] += 1
            resolved_rows.append((lot, m, f'coarg {best}', k, x, y, z))
            if len(examples['resolved-coarg']) < 10:
                examples['resolved-coarg'].append((lot, m, best, k))
        elif lines:
            res['lot-in-emevd-no-coarg'] += 1
            if len(examples['lot-in-emevd-no-coarg']) < 10:
                examples['lot-in-emevd-no-coarg'].append((lot, m, lines[0].strip()[:90]))
        else:
            res['absent-from-emevd'] += 1
            if len(examples['absent-from-emevd']) < 8:
                examples['absent-from-emevd'].append((lot, m))

    print("\n=== RESULT (non-custom targets) ===")
    for k, v in res.most_common():
        print(f"  {v:5d}  {k}")
    print(f"  -> RESOLVABLE (lot==entity + resolved-coarg): "
          f"{res['lot==entity'] + res['resolved-coarg']} / {len(targets)}")
    print(f"\n  ERR-custom m60_44_60 (locationless by design): {len(custom)}")

    for cls in ('lot==entity', 'resolved-coarg', 'lot-in-emevd-no-coarg', 'absent-from-emevd'):
        if examples[cls]:
            print(f"\n  ex {cls}:")
            for e in examples[cls]:
                print("    ", e)

    out = config.DATA_DIR / 'emevd_posless_resolved.json'
    json.dump([{'lot': l, 'map': mm, 'via': via, 'kind': k, 'x': x, 'y': y, 'z': z}
               for (l, mm, via, k, x, y, z) in resolved_rows],
              open(out, 'w', encoding='utf-8'), indent=1)
    print(f"\nsaved {len(resolved_rows)} resolved -> {out}")

if __name__ == '__main__':
    main()
