#!/usr/bin/env python3
"""Dump ItemLotParam_enemy rows 20000, 20001, 20002 from ERR regulation.bin."""
import sys, io, os, tempfile
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats

_str_type = SysType.GetType('System.String')
def _get_read(cls_name):
    t = asm.GetType(cls_name)
    return t.BaseType.GetMethod('Read', Array[SysType]([_str_type]))

_param_read = _get_read('SoulsFormats.PARAM')

def _read_from_bytes(read_method, data, suffix='.bin'):
    tmp = os.path.join(tempfile.gettempdir(), f'_mfg_tmp{suffix}')
    if hasattr(data, 'ToArray'):
        SysFile.WriteAllBytes(tmp, data.ToArray())
    else:
        SysFile.WriteAllBytes(tmp, data)
    return read_method.Invoke(None, Array[Object]([tmp]))

reg_path = config.ERR_MOD_DIR / 'regulation.bin'
bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(reg_path))

targets = [f for f in bnd.Files if 'ItemLotParam_enemy' in str(f.Name)]
print(f"Found {len(targets)} ItemLotParam_enemy files in regulation.bin:")
for t in targets:
    print(f"  {t.Name}")
target = targets[0]

param = _read_from_bytes(_param_read, target.Bytes, '.param')
# Apply paramdef
# Find correct paramdef by ParamType
defs = {}
for xml in config.PARAMDEF_DIR.glob('*.xml'):
    try:
        d = SoulsFormats.PARAMDEF.XmlDeserialize(str(xml), False)
        if d and d.ParamType: defs[str(d.ParamType)] = d
    except Exception: pass
pdef = defs.get(str(param.ParamType))
print(f"ParamType: {param.ParamType}; matched paramdef: {bool(pdef)}")
if pdef: param.ApplyParamdef(pdef)

# Item name lookup
name_db = {1: {}}  # category 1 = goods
def _get_fmg_read():
    return _get_read('SoulsFormats.FMG')
_fmg_read = _get_fmg_read()
for f in bnd.Files:
    n = str(f.Name)
    if n.endswith('GoodsName.fmg') and '_dlc' not in n.lower():
        fmg = _read_from_bytes(_fmg_read, f.Bytes, '.fmg')
        for e in fmg.Entries:
            t = str(e.Text) if e.Text else ''
            if t and t != '[ERROR]':
                name_db[1][int(e.ID)] = t
        break

interesting = {20000, 20001, 20002, 20003, 20004, 20005, 20006, 20007, 20008, 20009, 20010}
print(f"\nGoodsName entries loaded: {len(name_db[1])}")
print(f"Sample names: {[name_db[1].get(i) for i in [358000, 1240, 10101]]}")
all_ids = sorted(int(r.ID) for r in param.Rows)
print(f"\nRow count: {len(all_ids)}; first: {all_ids[:8]}; last: {all_ids[-5:]}")
print(f"Any 20000? {20000 in all_ids}  20001? {20001 in all_ids}  20002? {20002 in all_ids}")
print(f"Min 6-digit ID: {[x for x in all_ids if x>=100000][:5]}")
print(f"IDs around 20000: {[x for x in all_ids if 15000 <= x <= 25000][:30]}")

for row in param.Rows:
    rid = int(row.ID)
    if rid not in interesting:
        continue
    print(f"\n--- Row {rid} ---")
    flag = 0
    items = []
    for cell in row.Cells:
        nm = str(cell.Def.InternalName)
        try:
            v = int(cell.Value)
        except Exception:
            continue
        if nm == 'getItemFlagId':
            flag = v
        # slots 1..8
        if nm.startswith('lotItemId0'):
            slot = int(nm[-1])
            while len(items) < slot: items.append({})
            if v: items[slot-1]['id'] = v
        if nm.startswith('lotItemCategory0'):
            slot = int(nm[-1])
            while len(items) < slot: items.append({})
            if v: items[slot-1]['cat'] = v
        if nm.startswith('lotItemNum0'):
            slot = int(nm[-1])
            while len(items) < slot: items.append({})
            if v: items[slot-1]['num'] = v
        if nm.startswith('lotItemBasePoint0'):
            slot = int(nm[-1])
            while len(items) < slot: items.append({})
            if v: items[slot-1]['point'] = v
    print(f"  getItemFlagId = {flag}")
    for i, sl in enumerate(items, 1):
        if sl.get('id'):
            nm = name_db.get(sl.get('cat',1), {}).get(sl['id'], '?')
            print(f"  slot{i}: id={sl['id']} ({nm}) cat={sl.get('cat')} num={sl.get('num',1)} point={sl.get('point')}")
