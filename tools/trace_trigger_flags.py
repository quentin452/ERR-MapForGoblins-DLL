#!/usr/bin/env python3
"""#2 deep trace — 2-hop: position-less lot -> trigger flag -> flag setter event
-> positioned region/entity. Measures how many posless lots have a real world
anchor reachable via the EMEVD award->trigger->setter chain (the only honest
route, per the 90005774 finding).

Generic (no per-template arg map): for each posless lot, gather candidate
trigger flags = the other integers appearing on any award line for that lot
(InitializeCommonEvent of an award template, or a direct AwardItemLot/
AwardItemsIncludingClients literal). For each candidate flag that some event
SETS (SetEventFlag*/BatchSetEventFlags ON), scan that setter event for an int
resolving to a placed MSB Region (POINT) or Part with a position. Wide net =>
UPPER BOUND on recoverable.

Run: PYTHONPATH=... MFG_PROFILE=err py -3.14 tools/trace_trigger_flags.py
"""
import sys, io, os, re, json, tempfile
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pathlib import Path
from collections import defaultdict, Counter
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
_msbe = asm.GetType('SoulsFormats.MSBE').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str]), None)
JS_DIR = Path(r'D:\tools\emevd_js\err')

# Award common templates (from common_func): lot is one of the params; we don't
# need which one — we take ALL other ints on the call line as candidate flags.
AWARD_TPLS = {90005300, 90005301, 90005360, 90005390, 90005555, 90005632,
              90005724, 90005750, 90005753, 90005768, 90005774, 90005776,
              90005792, 90005860, 90005861, 90005880, 90005110, 90005959,
              90006400, 90006900, 90007100}
INT = re.compile(r'\d{4,}')

def read_msb(p):
    data = SoulsFormats.DCX.Decompress(str(p)).ToArray()
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_t.msb')
    SysFile.WriteAllBytes(tmp, data)
    m = _msbe.Invoke(None, Array[Object]([tmp])); os.unlink(tmp)
    return m

def index_map(msb):
    idx = {}
    for cat in ('MapPieces','Enemies','Players','Collisions','DummyAssets',
                'DummyEnemies','ConnectCollisions','Assets'):
        for p in (getattr(msb.Parts, cat, None) or []):
            eid = int(getattr(p, 'EntityID', 0) or 0)
            if eid > 0: idx.setdefault(eid, ('part', p.Position.X, p.Position.Y, p.Position.Z))
            for g in (getattr(p, 'EntityGroupIDs', None) or []):
                gi = int(g)
                if gi > 0: idx.setdefault(gi, ('part-grp', p.Position.X, p.Position.Y, p.Position.Z))
    for rn in dir(msb.Regions):
        if rn.startswith('_'): continue
        try: col = getattr(msb.Regions, rn)
        except Exception: continue
        if not hasattr(col, '__iter__'): continue
        try:
            for r in col:
                try:
                    eid = int(getattr(r, 'EntityID', 0) or 0)
                    if eid > 0: idx.setdefault(eid, ('region', r.Position.X, r.Position.Y, r.Position.Z))
                except Exception: pass
        except Exception: pass
    return idx

def main():
    db = json.load(open(config.PROJECT_DIR / 'data' / 'items_database.json'))
    posless = [r for r in db if r['x']==0 and r['y']==0 and r['z']==0 and r['map']!='m60_44_60_00']
    lots = set(r['itemLotId'] for r in posless)

    maps = sorted(set(r['map'] for r in posless))
    md = config.require_err_mod_dir() / 'map' / 'MapStudio'
    gindex = {}
    for m in maps:
        p = md / (m + '.msb.dcx')
        if p.exists():
            try:
                for e, v in index_map(read_msb(p)).items():
                    gindex.setdefault(e, (v[0], float(v[1]), float(v[2]), float(v[3]), m))
            except Exception: pass
    print(f"indexed {len([1 for _ in gindex])} entity/region ids over {len(maps)} maps")

    # Parse JS corpus into events; record flags each event SETS, and all ints.
    setters = defaultdict(list)   # flag -> [event_idx]
    events = []                   # (ints:set, raw)
    award_lines_by_lot = defaultdict(list)
    set_re = re.compile(r'(?:SetEventFlag\w*|SetNetworkconnectedEventFlag\w*|EnableEventFlag)\(\s*(\d+)\s*,\s*ON')
    batch_re = re.compile(r'BatchSetEventFlags\(\s*(\d+)\s*,\s*(\d+)\s*,\s*ON')
    for f in sorted(JS_DIR.glob('*.emevd.dcx.js')):
        txt = f.read_text(encoding='utf-8', errors='replace')
        for chunk in re.split(r'(?=\$Event\()', txt):
            if not chunk.strip(): continue
            ints = set(int(t) for t in INT.findall(chunk))
            ei = len(events)
            events.append(ints)
            for fl in set_re.findall(chunk):
                setters[int(fl)].append(ei)
            for a, b in batch_re.findall(chunk):
                a, b = int(a), int(b)
                if 0 <= b - a <= 64:
                    for fl in range(a, b + 1): setters[fl].append(ei)
            # award lines for posless lots
            for line in chunk.splitlines():
                if 'AwardItemLot(' in line or 'AwardItemsIncludingClients(' in line:
                    for t in INT.findall(line):
                        v = int(t)
                        if v in lots: award_lines_by_lot[v].append(line.strip())
                elif 'InitializeCommonEvent(' in line or 'InitializeEvent(' in line:
                    toks = [int(t) for t in INT.findall(line)]
                    if any(t in AWARD_TPLS for t in toks):
                        for v in toks:
                            if v in lots: award_lines_by_lot[v].append(line.strip())

    # Trace.
    res = Counter(); resolved = []; examples = defaultdict(list)
    for lot in lots:
        lines = award_lines_by_lot.get(lot, [])
        if not lines:
            res['no-award-line'] += 1; continue
        cand_flags = set()
        for ln in lines:
            for t in INT.findall(ln):
                v = int(t)
                if v != lot and v in setters: cand_flags.add(v)
        if not cand_flags:
            res['award-but-no-settable-flag'] += 1; continue
        pos = None
        for fl in cand_flags:
            for ei in setters[fl]:
                hit = [e for e in events[ei] if e in gindex]
                if hit:
                    best = sorted(hit, key=lambda e: {'region':0,'part':1,'part-grp':2}.get(gindex[e][0],3))[0]
                    pos = (fl, best, gindex[best]); break
            if pos: break
        if pos:
            res['TRACED-to-position'] += 1
            fl, ent, g = pos
            resolved.append({'lot': lot, 'flag': fl, 'anchor': ent, 'kind': g[0],
                             'x': g[1], 'y': g[2], 'z': g[3], 'map': g[4]})
            if len(examples['TRACED-to-position']) < 15:
                examples['TRACED-to-position'].append((lot, f"flag {fl} -> {g[0]} {ent} @ {g[4]}"))
        else:
            res['flag-set-but-no-position'] += 1
            if len(examples['flag-set-but-no-position']) < 8:
                examples['flag-set-but-no-position'].append((lot, list(cand_flags)[:3]))

    print(f"\nposless non-custom lots: {len(lots)}")
    for k, v in res.most_common(): print(f"  {v:5d}  {k}")
    for cls in ('TRACED-to-position', 'flag-set-but-no-position'):
        if examples[cls]:
            print(f"\n ex {cls}:")
            for e in examples[cls]: print("   ", e)
    out = config.DATA_DIR / 'emevd_trigger_traced.json'
    json.dump(resolved, open(out, 'w', encoding='utf-8'), indent=1)
    print(f"\nsaved {len(resolved)} traced -> {out}")

if __name__ == '__main__':
    main()
