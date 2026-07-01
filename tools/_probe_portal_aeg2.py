#!/usr/bin/env python3
"""RE probe 2 (Portal): characterise AEG099_510 placements — the sending-gate model.

180 total placements is > MapGenie's 39; narrow to the INTERACTIVE subset. For each AEG099_510
placement dump EntityID + EntityGroupIDs, so we can see if entity-bearing placements ≈ the portal
count (the pattern the other interactables use: interactive instance carries an EntityID, decoration/
destination anchors do not). Also bucket by area.

Usage: py _probe_portal_aeg2.py
"""
import sys, io, os, tempfile, collections
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats

MSB_DIR = config.require_err_mod_dir() / 'map' / 'MapStudio'
_str_type = SysType.GetType('System.String')
_msbe_read = asm.GetType('SoulsFormats.MSBE').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)
def rfb(rm, data, suf='.msb'):
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_mfg_p2' + suf)
    SysFile.WriteAllBytes(tmp, data.ToArray() if hasattr(data, 'ToArray') else data)
    r = rm.Invoke(None, Array[Object]([tmp])); os.unlink(tmp); return r

TARGET = 'AEG099_510'
def eid(p):
    try: return int(getattr(p, 'EntityID', 0) or 0)
    except Exception: return 0
def egids(p):
    out = []
    try:
        arr = getattr(p, 'EntityGroupIDs', None)
        if arr is not None:
            for v in arr:
                iv = int(v)
                if iv > 0: out.append(iv)
    except Exception: pass
    return out

total = 0
with_e = 0
by_area = collections.Counter()
by_area_e = collections.Counter()
rows = []
for msb_path in sorted(MSB_DIR.glob('*.msb.dcx')):
    parts = msb_path.name.replace('.msb.dcx', '').split('_')
    if len(parts) < 4: continue
    area, gx, gz = int(parts[0][1:]), int(parts[1]), int(parts[2])
    try:
        msb = rfb(_msbe_read, SoulsFormats.DCX.Decompress(str(msb_path)))
    except Exception:
        continue
    for p in msb.Parts.Assets:
        if int(getattr(p, 'GameEditionDisable', 0) or 0) == 1: continue
        if str(p.ModelName) != TARGET: continue
        total += 1
        by_area[area] += 1
        e = eid(p); g = egids(p)
        if e > 0:
            with_e += 1; by_area_e[area] += 1
            rows.append((area, gx, gz, str(p.Name), e, g,
                         round(float(p.Position.X),1), round(float(p.Position.Z),1)))

print(f"# {TARGET}: {total} total placements, {with_e} with EntityID>0")
print(f"# by area (total):      {dict(sorted(by_area.items()))}")
print(f"# by area (entity>0):   {dict(sorted(by_area_e.items()))}")
print(f"\n# entity-bearing {TARGET} placements ({with_e}):")
for r in sorted(rows):
    print(f"   m{r[0]:02d}_{r[1]:02d}_{r[2]:02d}  {r[3]:24s} eid={r[4]:>10}  grp={r[5]}  pos=({r[6]},{r[7]})")
