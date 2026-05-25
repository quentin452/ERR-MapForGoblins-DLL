#!/usr/bin/env python3
"""Decode specific template event bodies to understand what they do.
Verify award semantics for new candidate templates."""
import sys, io, json, struct
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
from pathlib import Path
import config
from pythonnet import load; load('coreclr')
import clr
from System.Reflection import Assembly
from System import Array, Type as SysType, Object
asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats
_str = SysType.GetType('System.String')
_emcls = asm.GetType('SoulsFormats.EMEVD')
_emr = _emcls.BaseType.GetMethod('Read', Array[SysType]([_str]))

emedf = json.load(open(Path(__file__).parent / 'er-common.emedf.json', encoding='utf-8'))
INSTR = {}
for cls in emedf['main_classes']:
    bank = int(cls['index'])
    for ins in cls.get('instrs', []):
        iid = int(ins['index'])
        INSTR[(bank, iid)] = {
            'name': ins['name'],
            'args': [{'name': a['name'], 'type': a.get('type')} for a in ins.get('args', [])]
        }

def decode(bank, iid, raw, params):
    """Decode instruction with parameter overrides showing as <X{srcByte}>."""
    key = (bank, iid)
    meta = INSTR.get(key)
    name = meta['name'] if meta else f"UNKNOWN({bank}:{iid:02d})"
    args = meta['args'] if meta else []
    words = [struct.unpack_from('<i', raw, i)[0] for i in range(0, len(raw)-3, 4)] if raw else []
    parts = []
    for ai, am in enumerate(args):
        if ai >= len(words):
            parts.append(f"{am['name']}=?")
            continue
        v = words[ai]
        ov = next((p for p in params if p[0] == ai*4), None)
        if ov:
            parts.append(f"{am['name']}=<X{ov[1]}>")
        else:
            if am.get('type') == 8:  # float
                f = struct.unpack('<f', struct.pack('<i', v))[0]
                parts.append(f"{am['name']}={f:g}f")
            else:
                parts.append(f"{am['name']}={v}")
    return f"{name}({', '.join(parts)})"

em_func = _emr.Invoke(None, Array[Object]([str(config.ERR_MOD_DIR/'event'/'common_func.emevd.dcx')]))
events = {int(ev.ID): ev for ev in em_func.Events}

TARGETS = [90006900, 90005724, 90005768, 90005950, 90006050, 90006400, 90005776]

for tid in TARGETS:
    ev = events.get(tid)
    if not ev:
        print(f"!! {tid} not found\n"); continue
    params_by_idx = {}
    for p in ev.Parameters:
        params_by_idx.setdefault(int(p.InstructionIndex), []).append(
            (int(p.TargetStartByte), int(p.SourceStartByte), int(p.ByteCount))
        )
    print(f"=== Template {tid} ({len(ev.Instructions)} instr, rest={ev.RestBehavior}) ===")
    for idx, ins in enumerate(ev.Instructions):
        b = bytes(ins.ArgData) if ins.ArgData else b''
        decoded = decode(int(ins.Bank), int(ins.ID), b, params_by_idx.get(idx, []))
        print(f"  [{idx:3d}] {decoded}")
    print()
