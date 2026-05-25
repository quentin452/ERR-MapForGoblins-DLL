"""Decode all 90005768 callers to understand the Queelign dual-drop pattern."""
import sys, io, os, tempfile, struct, json, re
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
_emcls = asm.GetType('SoulsFormats.EMEVD')
_emr = _emcls.BaseType.GetMethod('Read', Array[SysType]([_str]))

# Load names and lot maps
defs = {}
for xml in config.PARAMDEF_DIR.glob('*.xml'):
    try:
        d = SoulsFormats.PARAMDEF.XmlDeserialize(str(xml), False)
        if d and d.ParamType: defs[str(d.ParamType)] = d
    except: pass
bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(config.ERR_MOD_DIR/'regulation.bin'))
goods_names = {}
_fcls = asm.GetType('SoulsFormats.FMG')
_fr = _fcls.BaseType.GetMethod('Read', Array[SysType]([_str]))
_bcls = asm.GetType('SoulsFormats.BND4')
_br = _bcls.BaseType.GetMethod('Read', Array[SysType]([_str]))
bndmsg = _br.Invoke(None, Array[Object]([str(config.ERR_MOD_DIR/'msg/engus/item_dlc02.msgbnd.dcx')]))
for f in bndmsg.Files:
    n = str(f.Name)
    if any(k in n for k in ['GoodsName','WeaponName','ProtectorName','AccessoryName','GemName']):
        tmp = os.path.join(tempfile.gettempdir(),'_g.fmg')
        SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
        fmg = _fr.Invoke(None, Array[Object]([tmp]))
        for e in fmg.Entries:
            t = str(e.Text) if e.Text else ''
            if t and t!='[ERROR]':
                goods_names.setdefault(int(e.ID), t)

def load_param(name):
    for f in bnd.Files:
        if name in str(f.Name):
            tmp = os.path.join(tempfile.gettempdir(),'_p.param')
            SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
            p = _pr.Invoke(None, Array[Object]([tmp]))
            pdef = defs.get(str(p.ParamType))
            if pdef: p.ApplyParamdef(pdef)
            return p

lot_map = load_param('ItemLotParam_map')
lot_dict = {}
for r in lot_map.Rows:
    rid = int(r.ID); ent = {}
    for c in r.Cells:
        nm = str(c.Def.InternalName)
        try: v = int(c.Value)
        except: continue
        if nm.startswith('lotItemId0') or nm.startswith('lotItemCategory0') or \
           nm.startswith('lotItemNum0') or nm == 'getItemFlagId':
            ent[nm] = v
    lot_dict[rid] = ent

def expand(lot_id):
    out = []
    if lot_id not in lot_dict: return out
    # base row
    base = lot_dict[lot_id]
    items = []
    for s in range(1,9):
        iid = base.get(f'lotItemId0{s}',0); cat = base.get(f'lotItemCategory0{s}',0)
        num = base.get(f'lotItemNum0{s}',0)
        if iid>0 and cat>0:
            items.append((s, iid, cat, num, goods_names.get(iid, f'?id={iid}')))
    flag = base.get('getItemFlagId',0)
    if items: out.append((lot_id, items, flag))
    for off in range(1, 20):
        sub = lot_dict.get(lot_id + off)
        if sub is None: break
        sf = sub.get('getItemFlagId',0)
        if sf <= 0: continue
        sitems = []
        for s in range(1,9):
            iid = sub.get(f'lotItemId0{s}',0); cat = sub.get(f'lotItemCategory0{s}',0)
            num = sub.get(f'lotItemNum0{s}',0)
            if iid>0 and cat>0:
                sitems.append((s, iid, cat, num, goods_names.get(iid, f'?id={iid}')))
        if sitems: out.append((lot_id+off, sitems, sf))
    return out

# Find all 90005768 callers
EVENT_DIR = config.ERR_MOD_DIR / 'event'
print("=== All 90005768 callers ===")
for path in sorted(EVENT_DIR.glob('*.emevd.dcx')):
    if path.name == 'common_func.emevd.dcx': continue
    em = _emr.Invoke(None, Array[Object]([str(path)]))
    map_name = path.name.replace('.emevd.dcx','')
    for ev in em.Events:
        for ins in ev.Instructions:
            if int(ins.Bank)!=2000 or int(ins.ID)!=6: continue
            b = bytes(ins.ArgData) if ins.ArgData else b''
            if len(b) < 8: continue
            tid = struct.unpack_from('<i', b, 4)[0]
            if tid != 90005768: continue
            args = [struct.unpack_from('<i', b, i)[0] for i in range(0, len(b)-3, 4)]
            print(f"\n--- {map_name}, event {int(ev.ID)} ---")
            print(f"  caller args raw: {args}")
            # Map to param buffer offsets per template parameter table
            # Caller byte layout: [0=slot, 4=eventId, 8=X0, 12=X4, 16=X8, 20=X12, 24=X16, 28=X20, 32=X24, ...]
            if len(args) >= 8:
                x0  = args[2]  # byte 8
                x4  = args[3]  # byte 12 (LOT 1)
                x8  = args[4]  # byte 16
                x12 = args[5]  # byte 20 (LOT 2)
                x16 = args[6] if len(args)>6 else None  # byte 24
                x20 = args[7] if len(args)>7 else None  # byte 28
                print(f"  X0 (kill_flag?)={x0}, X4 (lot_first)={x4}, X8 (gate_flag1)={x8},"
                      f"\n  X12 (lot_second)={x12}, X16 (gate_flag2)={x16}, X20 (counter_flag_base)={x20}")
                print(f"\n  First-kill lot {x4}:")
                for sid, items, flag in expand(x4):
                    print(f"    row {sid} flag={flag}:")
                    for s, iid, cat, num, nm in items:
                        print(f"      slot{s}: id={iid} cat={cat} num={num}  '{nm}'")
                print(f"\n  Second-kill lot {x12}:")
                for sid, items, flag in expand(x12):
                    print(f"    row {sid} flag={flag}:")
                    for s, iid, cat, num, nm in items:
                        print(f"      slot{s}: id={iid} cat={cat} num={num}  '{nm}'")
