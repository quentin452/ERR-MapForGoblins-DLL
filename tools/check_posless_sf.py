#!/usr/bin/env python3
"""Definitive: scan EVERY *.msb.dcx (DFLT + KRAK, via SoulsFormats) for Treasure
events placing a position-less lot. Reports the LOD/file + part for any hit, so
we know if the 430 posless lots are really un-placed or just in maps #1 skips."""
import sys, io, os, json, tempfile
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from collections import Counter
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

def read_msb(p):
    data = SoulsFormats.DCX.Decompress(str(p)).ToArray()
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_c.msb')
    SysFile.WriteAllBytes(tmp, data)
    m = _msbe.Invoke(None, Array[Object]([tmp])); os.unlink(tmp)
    return m

def main():
    db = json.load(open(config.PROJECT_DIR / 'data' / 'items_database.json'))
    posless = set(r['itemLotId'] for r in db
                  if r['x'] == 0 and r['y'] == 0 and r['z'] == 0 and r['map'] != 'm60_44_60_00')
    md = config.require_err_mod_dir() / 'map' / 'MapStudio'
    found = {}
    files = sorted(md.glob('*.msb.dcx'))
    for i, p in enumerate(files):
        try:
            msb = read_msb(p)
        except Exception:
            continue
        for t in msb.Events.Treasures:
            try:
                lot = int(getattr(t, 'ItemLotID', 0) or 0)
            except Exception:
                lot = 0
            if lot in posless and lot not in found:
                pn = getattr(t, 'TreasurePartName', None)
                found[lot] = (p.name, str(pn) if pn else '', p.name[:-8].split('_')[-1])
    by_lod = Counter(v[2] for v in found.values())
    print(f"scanned {len(files)} MSBs (DFLT+KRAK)")
    print(f"position-less non-custom lots: {len(posless)}")
    print(f"  placed as Treasure in SOME msb: {len(found)}  by LOD: {dict(by_lod)}")
    non00 = [(l, v) for l, v in found.items() if v[2] != '00']
    print(f"  in non-_00 maps (missed by #1): {len(non00)}")
    for l, v in sorted(non00)[:40]:
        print(f"    lot {l}: {v[0]} part={v[1]!r}")

if __name__ == '__main__':
    main()
