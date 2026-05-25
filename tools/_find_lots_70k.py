"""Locate ItemLotParam rows 70000 and 70500 (referenced by template 90005724).
Search in both mod and vanilla regulations, and across all params."""
import sys, io, os, tempfile, re, json
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pythonnet import load
load('coreclr')
from System.Reflection import Assembly
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
import SoulsFormats

_str = SysType.GetType('System.String')
_pcls = asm.GetType('SoulsFormats.PARAM')
_pr = _pcls.BaseType.GetMethod('Read', Array[SysType]([_str]))

defs = {}
for xml in config.PARAMDEF_DIR.glob('*.xml'):
    try:
        d = SoulsFormats.PARAMDEF.XmlDeserialize(str(xml), False)
        if d and d.ParamType:
            defs[str(d.ParamType)] = d
    except Exception:
        pass

# Goods names for resolution
goods_names = {}
_fcls = asm.GetType('SoulsFormats.FMG')
_fr = _fcls.BaseType.GetMethod('Read', Array[SysType]([_str]))
_bcls = asm.GetType('SoulsFormats.BND4')
_br = _bcls.BaseType.GetMethod('Read', Array[SysType]([_str]))
bndmsg = _br.Invoke(None, Array[Object]([str(config.ERR_MOD_DIR/'msg/engus/item_dlc02.msgbnd.dcx')]))
for f in bndmsg.Files:
    if 'GoodsName' in str(f.Name):
        tmp = os.path.join(tempfile.gettempdir(),'_g.fmg')
        SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
        fmg = _fr.Invoke(None, Array[Object]([tmp]))
        for e in fmg.Entries:
            t = str(e.Text) if e.Text else ''
            if t and t != '[ERROR]':
                goods_names.setdefault(int(e.ID), t)

TARGETS = {70000, 70500}

def dump_row(name, r):
    items = []
    flag = 0
    for c in r.Cells:
        nm = str(c.Def.InternalName)
        try: v = int(c.Value)
        except: continue
        if v in (0, -1): continue
        if nm.startswith('lotItemId0'):
            slot = int(nm[-1])
            items.append((slot, 'id', v, goods_names.get(v, f'?')))
        if nm.startswith('lotItemCategory0'):
            slot = int(nm[-1]); items.append((slot, 'cat', v, ''))
        if nm.startswith('lotItemNum0'):
            slot = int(nm[-1]); items.append((slot, 'num', v, ''))
        if nm == 'getItemFlagId':
            flag = v
    print(f"    flag={flag}")
    slots = {}
    for s, k, v, extra in items:
        slots.setdefault(s, {})[k] = (v, extra)
    for s in sorted(slots):
        print(f"    slot{s}: {slots[s]}")

for label, reg_path in [('MOD', config.ERR_MOD_DIR/'regulation.bin'),
                         ('VANILLA', config.GAME_DIR/'regulation.bin')]:
    print(f"\n========== {label}: {reg_path} ==========")
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(reg_path))
    # Walk ALL .param files, look for our row IDs
    for f in bnd.Files:
        fname = str(f.Name)
        if not fname.lower().endswith('.param'): continue
        base = fname.rsplit('\\', 1)[-1]
        tmp = os.path.join(tempfile.gettempdir(),'_p.param')
        SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
        try: p = _pr.Invoke(None, Array[Object]([tmp]))
        except Exception: continue
        pdef = defs.get(str(p.ParamType))
        if pdef:
            try: p.ApplyParamdef(pdef)
            except Exception: pass
        for r in p.Rows:
            rid = int(r.ID)
            if rid in TARGETS:
                print(f"\n  {base} row {rid}:")
                dump_row(base, r)
