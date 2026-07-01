#!/usr/bin/env python3
"""Verify EMEVD template 90005605 = the sending-gate warp: dump its calls (entity@arg2 + all args),
join each entity to its AEG099_510 MSB placement (area/tile/pos + PlaceName-ish), and report the
distinct interactive-gate set. Confirms Portal is a clean template-bound feature (entity@arg[2]).
"""
import sys, io, os, struct, tempfile, collections
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL)); clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats
ERR = config.require_err_mod_dir()
_st = SysType.GetType('System.String')
def _rd(tn): return asm.GetType(tn).GetMethod('Read', BindingFlags.Public|BindingFlags.Static|BindingFlags.FlattenHierarchy, None, Array[SysType]([_st]), None)
_msbe = _rd('SoulsFormats.MSBE'); _emevd = _rd('SoulsFormats.EMEVD')
def rfb(rm, data, suf):
    t = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_pv' + suf)
    SysFile.WriteAllBytes(t, data.ToArray() if hasattr(data, 'ToArray') else data)
    r = rm.Invoke(None, Array[Object]([t])); os.unlink(t); return r

TEMPLATE = 90005605
# entity -> (tile, pos) from AEG099_510 placements
gate_pos = {}
for mp in sorted((ERR/'map'/'MapStudio').glob('*.msb.dcx')):
    pt = mp.name.replace('.msb.dcx', '').split('_')
    if len(pt) < 4: continue
    area, gx, gz = int(pt[0][1:]), int(pt[1]), int(pt[2])
    try: msb = rfb(_msbe, SoulsFormats.DCX.Decompress(str(mp)), '.msb')
    except Exception: continue
    for p in msb.Parts.Assets:
        if int(getattr(p,'GameEditionDisable',0) or 0)==1: continue
        if str(p.ModelName) != 'AEG099_510': continue
        e = int(getattr(p,'EntityID',0) or 0)
        if e>0: gate_pos.setdefault(e, (f"m{area:02d}_{gx:02d}_{gz:02d}", round(float(p.Position.X),1), round(float(p.Position.Z),1)))

calls = []
for ep in sorted((ERR/'event').glob('*.emevd.dcx')):
    try: emevd = rfb(_emevd, SoulsFormats.DCX.Decompress(str(ep)), '.evd')
    except Exception: continue
    mn = ep.name.replace('.emevd.dcx','')
    for evt in emevd.Events:
        for ins in evt.Instructions:
            if int(ins.Bank)!=2000: continue
            a = bytes(ins.ArgData) if ins.ArgData else b''
            if len(a)<8: continue
            if struct.unpack_from('<i',a,4)[0]!=TEMPLATE: continue
            nargs=(len(a))//4
            argvals=[struct.unpack_from('<i',a,o)[0] for o in range(0,len(a)-3,4)]
            calls.append((mn, argvals))

print(f"# template {TEMPLATE}: {len(calls)} calls")
gates=set()
for mn,av in calls:
    ent = av[2] if len(av)>2 else 0
    gates.add(ent)
    pos = gate_pos.get(ent)
    tag = "AEG099_510 "+str(pos) if pos else "(not an AEG099_510 entity!)"
    print(f"   {mn:16s} args={av}  -> entity(arg2)={ent}  {tag}")
in_model = sum(1 for g in gates if g in gate_pos)
print(f"\n# distinct entities at arg[2]: {len(gates)}; of those, {in_model} are AEG099_510 placements")
print(f"# tiles spanned: {sorted({gate_pos[g][0] for g in gates if g in gate_pos})}")
