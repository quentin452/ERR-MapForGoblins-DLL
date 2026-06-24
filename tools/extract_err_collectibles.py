#!/usr/bin/env python3
"""Extract ALL ERR collectible asset positions (AEG099_8xx family — 100% ERR-new,
0 in vanilla). Generalises extract_rune_positions (which only does 821/822) to
every dedicated-model collectible. These ARE placed in the world with real MSB
positions, so this is WHERE the positions are for the ERR collectibles.

Output: data/err_collectibles.json  [{model, map, name, x,y,z, entityId, partsType}]
plus a per-model summary."""
import sys, io, json
sys.path.insert(0, 'tools')
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from collections import Counter, defaultdict
from pythonnet import load
load('coreclr')
import clr
clr.AddReference(str(config.SOULSFORMATS_DLL))
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
import SoulsFormats
asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
_str = SysType.GetType('System.String')
_rd = asm.GetType('SoulsFormats.MSBE').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str]), None)

def main():
    md = config.require_err_mod_dir() / 'map' / 'MapStudio'
    out = []
    per_model = Counter()
    for p in sorted(md.glob('*.msb.dcx')):
        try:
            m = _rd.Invoke(None, Array[Object]([str(p)]))
        except Exception:
            continue
        mapn = p.name[:-8]
        for cat in ('Assets', 'DummyAssets'):
            col = getattr(m.Parts, cat, None) or []
            for a in col:
                mdl = str(a.ModelName or '')
                if not mdl.startswith('AEG099_8'):
                    continue
                pos = a.Position
                eid = int(getattr(a, 'EntityID', 0) or 0)
                grp = [int(g) for g in (getattr(a, 'EntityGroupIDs', None) or []) if int(g) > 0]
                out.append({'model': mdl, 'map': mapn, 'name': str(a.Name or ''),
                            'x': float(pos.X), 'y': float(pos.Y), 'z': float(pos.Z),
                            'entityId': eid, 'groups': grp, 'partsType': cat})
                per_model[mdl] += 1
    outp = config.DATA_DIR / 'err_collectibles.json'
    json.dump(out, open(outp, 'w', encoding='utf-8'), indent=1)
    print(f"TOTAL ERR collectible assets placed (positions found): {len(out)}")
    print(f"distinct models: {len(per_model)}")
    print(f"saved -> {outp}\n")
    print(f"{'model':14} {'count':>6}  {'entityId set?':>13}")
    for mdl, c in per_model.most_common():
        n_eid = sum(1 for r in out if r['model'] == mdl and r['entityId'] > 0)
        print(f"{mdl:14} {c:>6}  {n_eid:>13}")

if __name__ == '__main__':
    main()
