#!/usr/bin/env python3
"""RE recon (Group 2): link an ActionButtonParam id -> the asset MODEL + EMEVD template that binds it.
For each target ABP id (elevator 'Descend' 5010, smithing 'Use smithing table' 6250, plus 'Pull lever'
levers), scan ALL EMEVD instructions for that id in the args, collect co-occurring entity ids, and
resolve them to MSB asset models. The dominant model = the feature's asset; the enclosing bank-2000
template (if any) is its harvest template. Temp files routed to the repo dir (AV blocks %TEMP%).
"""
import os, sys, io, struct, itertools, collections
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pathlib import Path
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL)); clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats
ERR = Path(config.require_err_mod_dir()); _st = SysType.GetType('System.String'); _c = itertools.count()
HERE = os.path.abspath('.')
def _rd(tn): return asm.GetType(tn).GetMethod('Read', BindingFlags.Public|BindingFlags.Static|BindingFlags.FlattenHierarchy, None, Array[SysType]([_st]), None)
_msbe = _rd('SoulsFormats.MSBE'); _emevd = _rd('SoulsFormats.EMEVD')
def rfb(rm, data):
    t = os.path.join(HERE, f'_g2tmp_{next(_c)}.dat'); SysFile.WriteAllBytes(t, data.ToArray() if hasattr(data,'ToArray') else data)
    try: return rm.Invoke(None, Array[Object]([t]))
    finally:
        try: os.unlink(t)
        except Exception: pass

TARGETS = {5010: 'elevator(Descend)', 6250: 'smithing(Use smithing table)'}

# entity -> model
ent_model = {}
for mp in sorted((ERR/'map'/'MapStudio').glob('*.msb.dcx')):
    try: msb = rfb(_msbe, SoulsFormats.DCX.Decompress(str(mp)))
    except Exception: continue
    for coll in (msb.Parts.Assets, msb.Parts.Enemies):
        for p in coll:
            e = int(getattr(p,'EntityID',0) or 0)
            if e>0: ent_model.setdefault(e, str(p.ModelName))
print(f"# entity->model: {len(ent_model)}")

# scan EMEVD: any instruction whose args contain a target ABP id → collect models of co-arg entities
model_hits = {t: collections.Counter() for t in TARGETS}
tmpl_hits = {t: collections.Counter() for t in TARGETS}
ent_hits = {t: set() for t in TARGETS}
for ep in sorted((ERR/'event').glob('*.emevd.dcx')):
    try: emevd = rfb(_emevd, SoulsFormats.DCX.Decompress(str(ep)))
    except Exception: continue
    for evt in emevd.Events:
        for ins in evt.Instructions:
            a = bytes(ins.ArgData) if ins.ArgData else b''
            if len(a) < 4: continue
            vals = [struct.unpack_from('<i', a, o)[0] for o in range(0, len(a)-3, 4)]
            vset = set(vals)
            for tgt in TARGETS:
                if tgt not in vset: continue
                tmpl = vals[1] if int(ins.Bank)==2000 and len(vals)>1 else int(ins.Bank)
                tmpl_hits[tgt][tmpl] += 1
                for v in vals:
                    m = ent_model.get(v)
                    if m and v != tgt:
                        model_hits[tgt][m] += 1; ent_hits[tgt].add(v)

for tgt, label in TARGETS.items():
    print(f"\n# ABP {tgt} = {label}: {len(ent_hits[tgt])} distinct co-arg entities")
    print(f"   models: {dict(model_hits[tgt].most_common(8))}")
    print(f"   enclosing templates/banks: {dict(tmpl_hits[tgt].most_common(8))}")
