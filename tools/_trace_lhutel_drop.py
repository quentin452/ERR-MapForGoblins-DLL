"""Trace how Lhutel (lot 20000) is awarded for the Tombsward Catacombs boss
(entity 30000800, npc 36640012, model c3664)."""
import sys, io, struct, json, os, tempfile, re
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
from collections import defaultdict
import config
from pythonnet import load
load('coreclr')
from System.Reflection import Assembly
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
import SoulsFormats
_str = SysType.GetType('System.String')
_emcls = asm.GetType('SoulsFormats.EMEVD')
_emr = _emcls.BaseType.GetMethod('Read', Array[SysType]([_str]))
_pcls = asm.GetType('SoulsFormats.PARAM')
_pr = _pcls.BaseType.GetMethod('Read', Array[SysType]([_str]))

emedf = json.load(open('er-common.emedf.json', encoding='utf-8'))
INSTR = {}
for cls in emedf['main_classes']:
    bank = int(cls['index'])
    for ins in cls.get('instrs', []):
        INSTR[(bank, int(ins['index']))] = {
            'name': ins['name'],
            'args': [a.get('name','?') for a in ins.get('args', [])]
        }

def decode(bank, iid, raw, params):
    meta = INSTR.get((bank, iid))
    name = meta['name'] if meta else f"{bank}:{iid:02d}"
    arg_names = meta['args'] if meta else []
    words = [struct.unpack_from('<i', raw, i)[0] for i in range(0, len(raw)-3, 4)] if raw else []
    parts = []
    for i, an in enumerate(arg_names):
        if i >= len(words): parts.append(f"{an}=?"); continue
        v = words[i]
        ov = next((p for p in params if p[0]==i*4), None)
        parts.append(f"{an}=<X{ov[1]}>" if ov else f"{an}={v}")
    return f"{name}({', '.join(parts)})"

def decode_event(em, target_eid, label='event'):
    for ev in em.Events:
        if int(ev.ID) != target_eid: continue
        params_by_idx = defaultdict(list)
        for p in ev.Parameters:
            params_by_idx[int(p.InstructionIndex)].append(
                (int(p.TargetStartByte), int(p.SourceStartByte), int(p.ByteCount))
            )
        print(f"\n=== {label} {target_eid} ({len(ev.Instructions)} instr, rest={ev.RestBehavior}) ===")
        for idx, ins in enumerate(ev.Instructions):
            b = bytes(ins.ArgData) if ins.ArgData else b''
            print(f"  [{idx:3d}] {decode(int(ins.Bank), int(ins.ID), b, params_by_idx.get(idx,[]))}")
        return ev
    print(f"!! event {target_eid} not found")
    return None

# 1) Decode 90005646 in common_func
em_func = _emr.Invoke(None, Array[Object]([str(config.ERR_MOD_DIR/'event/common_func.emevd.dcx')]))
decode_event(em_func, 90005646, 'common_func event')

# 2) Decode 9005800/01/11/22 — they're in common_func, not common
for eid in (9005800, 9005801, 9005811, 9005822):
    decode_event(em_func, eid, 'common_func event')

# 3) Search for SpawnObjTreasure (2003:69) anywhere with lot 20000
print("\n\n=== SpawnObjTreasure / Award* anywhere referencing 20000 (any byte) ===")
for path in sorted((config.ERR_MOD_DIR/'event').glob('*.emevd.dcx')):
    em = _emr.Invoke(None, Array[Object]([str(path)]))
    map_name = path.name.replace('.emevd.dcx','')
    for ev in em.Events:
        for idx, ins in enumerate(ev.Instructions):
            bank, iid = int(ins.Bank), int(ins.ID)
            # 2003:69 = SpawnObjTreasure, 2003:04 = AwardItemLot, 2003:36 = AwardItems
            if bank != 2003 or iid not in (4, 36, 69): continue
            b = bytes(ins.ArgData) if ins.ArgData else b''
            has = False
            for i in range(0, len(b)-3, 4):
                if struct.unpack_from('<i', b, i)[0] == 20000:
                    has = True; break
            if has:
                args = [struct.unpack_from('<i', b, i)[0] for i in range(0,len(b)-3,4)]
                nm = INSTR.get((bank,iid),{}).get('name', f"{bank}:{iid}")
                print(f"  {map_name} ev{int(ev.ID)} instr[{idx}] {nm} args={args}")
