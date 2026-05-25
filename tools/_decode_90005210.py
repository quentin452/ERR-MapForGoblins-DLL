#!/usr/bin/env python3
"""Decode event 90005210 from common_func.emevd using DarkScript3 ER emedf.

Goal: show the human-readable semantics of every instruction so we can answer
'what does 90005210 really do?' — in particular whether the value at caller-srcByte 8
(value 20000 in our case) is awarded as an item lot."""
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

emedf_path = Path(__file__).parent / 'er-common.emedf.json'
emedf = json.load(open(emedf_path, encoding='utf-8'))

# Build lookup (bank, id) -> {name, args:[{name, type}]}
INSTR = {}
for cls in emedf.get('main_classes', []):
    bank = int(cls['index'])
    for instr in cls.get('instrs', []):
        iid = int(instr['index'])
        INSTR[(bank, iid)] = {
            'name': instr['name'],
            'args': [{'name': a['name'], 'type': a.get('type'),
                      'enum_name': a.get('enum_name','')} for a in instr.get('args',[])]
        }

def fmt_val(v, arg_meta):
    """Format an int32 value as int or float depending on arg type."""
    if arg_meta and arg_meta.get('type') in (8,):  # f32
        f = struct.unpack('<f', struct.pack('<i', v))[0]
        return f"{f:g}f"
    return str(v)

def decode_instr(bank, iid, raw_bytes, params_for_idx):
    """Decode one instruction, applying param overrides."""
    key = (bank, iid)
    meta = INSTR.get(key)
    if not meta:
        return f"UNKNOWN({bank}:{iid:02d}) raw={raw_bytes.hex()}"
    name = meta['name']
    args_meta = meta['args']
    # Read static arg values (4-byte ints; emedf args may be smaller, but we approximate)
    # Build per-byte view; apply parameter overrides
    static_words = []
    for i in range(0, len(raw_bytes) - 3, 4):
        static_words.append(struct.unpack_from('<i', raw_bytes, i)[0])
    arg_strs = []
    for arg_idx, am in enumerate(args_meta):
        if arg_idx >= len(static_words):
            arg_strs.append(f"{am['name']}=?")
            continue
        v = static_words[arg_idx]
        # Find param override targeting this arg (targetByte == arg_idx*4)
        overrides = [p for p in params_for_idx if p[0] == arg_idx*4]
        if overrides:
            src = overrides[0][1]
            arg_strs.append(f"{am['name']}=<X{src}>")
        else:
            arg_strs.append(f"{am['name']}={fmt_val(v, am)}")
    enum_hint = ''
    return f"{name}({', '.join(arg_strs)})"

# Load common_func
em = _emr.Invoke(None, Array[Object]([str(config.ERR_MOD_DIR/'event'/'common_func.emevd.dcx')]))
target_event = None
for ev in em.Events:
    if int(ev.ID) == 90005210:
        target_event = ev; break
assert target_event is not None, "event 90005210 not found"

# Build params map
params_by_idx = {}
for p in target_event.Parameters:
    params_by_idx.setdefault(int(p.InstructionIndex), []).append(
        (int(p.TargetStartByte), int(p.SourceStartByte), int(p.ByteCount))
    )

# Caller signature for 90005210 (from m60_35_50_00 ev0 instr[20]):
# args = [slot, eventId, entity, 30000, 20000, deathEvent, 9.0f, 5.0f, 0, 0, 0, 0]
# Event param buffer (X0..X44, in bytes):
#   X0  = entity         (1035500219)
#   X4  = 30000
#   X8  = 20000
#   X12 = deathEvent     (1035502200)
#   X16 = float 9.0
#   X20 = float 5.0
#   X24 = 0
#   X28 = 0
#   X32 = 0
#   X36 = 0

print(f"=== Event 90005210 ({len(target_event.Instructions)} instructions, "
      f"restBehavior={target_event.RestBehavior}) ===")
print(f"Parameter slots referenced: srcBytes "
      f"{sorted({p[1] for plist in params_by_idx.values() for p in plist})}\n")

for idx, instr in enumerate(target_event.Instructions):
    bank = int(instr.Bank); iid = int(instr.ID)
    raw = bytes(instr.ArgData) if instr.ArgData else b''
    decoded = decode_instr(bank, iid, raw, params_by_idx.get(idx, []))
    print(f"  [{idx:3d}] {bank}:{iid:02d}  {decoded}")
