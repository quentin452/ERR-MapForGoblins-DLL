#!/usr/bin/env python3
"""Validate all templates in extract_all_items.py::TEMPLATE_EVENTS:
   - load common_func.emevd
   - for each template, find every item-award instruction (2003:04 AwardItemLot,
     2003:36 AwardItemsIncludingClients, 2003:78 etc.) — full list resolved from emedf.
   - for each award, determine which parameter slot (X0/X4/X8/...) holds the lot
   - compare against the offset declared in TEMPLATE_EVENTS
   - print verdict: 'CORRECT' / 'WRONG: actual lot at offset X' / 'NO AWARD: not an awarder'
"""
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
            'args': [{'name': a['name'], 'type': a.get('type'), 'enum': a.get('enum_name', '')}
                     for a in ins.get('args', [])]
        }

# Find every instruction whose name mentions Award/ItemLot
AWARD_KEYS = set()
for k, v in INSTR.items():
    nm = v['name'].lower()
    if 'award' in nm or 'item lot' in nm or 'itemlot' in nm:
        AWARD_KEYS.add(k)
print("Award/ItemLot-related instructions in emedf:")
for k in sorted(AWARD_KEYS):
    args = INSTR[k]['args']
    arg_desc = ', '.join(f"{a['name']}:{a['type']}" for a in args)
    print(f"  {k[0]}:{k[1]:02d}  {INSTR[k]['name']}({arg_desc})")
print()

# Also find which arg of each award instruction is the item lot
# by name match
AWARD_LOT_ARG_IDX = {}  # (bank, id) -> arg index of "item lot" arg
for k in AWARD_KEYS:
    args = INSTR[k]['args']
    for i, a in enumerate(args):
        nm = a['name'].lower()
        if 'item lot' in nm or 'itemlot' in nm or ('lot' in nm and 'flag' not in nm):
            AWARD_LOT_ARG_IDX[k] = i
            break

# Templates declared by extract_all_items.py
TEMPLATE_EVENTS = {
    90005200: (8, 16, 20),
    90005210: (8, 16, 20),
    90005300: (8, 16, 20),
    90005301: (8, 16, 20),
    90005860: (16, 24, 28),
    90005861: (16, 24, 28),
    90005880: (16, 24, 28),
    90005881: (16, 24, 28),
    90005882: (16, 24, 28),
    90005883: (16, 24, 28),
    90005885: (16, 24, 28),
    90005750: (8, 16, 20),
    90005774: (8, 12, 16),
    90005792: (8, 24, 28),
    90005632: (8, 16, 20),
    90005110: (8, 20, 24),
    90005390: (8, 28, 32),
    90005555: (8, 12, 16),
}

# Read common_func once
em_func = _emr.Invoke(None, Array[Object]([str(config.ERR_MOD_DIR/'event'/'common_func.emevd.dcx')]))
events_by_id = {int(ev.ID): ev for ev in em_func.Events}

def analyze_event(eid, declared_lot_off):
    ev = events_by_id.get(eid)
    if ev is None:
        return f"NOT FOUND in common_func.emevd"
    # Parameter table
    params_by_idx = {}
    for p in ev.Parameters:
        params_by_idx.setdefault(int(p.InstructionIndex), []).append(
            (int(p.TargetStartByte), int(p.SourceStartByte), int(p.ByteCount))
        )
    # Walk instructions, find award ones
    awards = []
    for idx, ins in enumerate(ev.Instructions):
        key = (int(ins.Bank), int(ins.ID))
        if key not in AWARD_KEYS: continue
        lot_arg_idx = AWARD_LOT_ARG_IDX.get(key)
        if lot_arg_idx is None: continue
        b = bytes(ins.ArgData) if ins.ArgData else b''
        static_words = [struct.unpack_from('<i', b, i)[0] for i in range(0, len(b)-3, 4)]
        lot_target_byte = lot_arg_idx * 4
        # Check param overrides for this instruction at lot_target_byte
        param_src = None
        for tb, sb, bc in params_by_idx.get(idx, []):
            if tb == lot_target_byte:
                param_src = sb
                break
        lot_val_static = static_words[lot_arg_idx] if lot_arg_idx < len(static_words) else None
        awards.append({
            'instr_idx': idx,
            'op': key,
            'op_name': INSTR[key]['name'],
            'lot_static': lot_val_static,
            'lot_param_src_byte': param_src,
        })

    if not awards:
        return ("NO AWARD: template body does NOT contain any item-award instruction.\n"
                "       The (entity_off, lot_off) extraction is meaningless for this event.")

    lines = []
    actual_offsets = set()
    for a in awards:
        lot_src = a['lot_param_src_byte']
        if lot_src is not None:
            # The caller passes lot at this srcByte inside the event-param block.
            # The InitializeEvent (2000:06) arg layout: [slot(4), eventId(4), then param block].
            # so InitializeEvent ArgData byte offset = 8 + lot_src
            caller_byte = 8 + lot_src
            actual_offsets.add(caller_byte)
            lines.append(f"   instr[{a['instr_idx']}] {a['op'][0]}:{a['op'][1]:02d} {a['op_name']}: "
                         f"lot bound to <X{lot_src}>  →  caller InitializeEvent arg byte {caller_byte}")
        else:
            lines.append(f"   instr[{a['instr_idx']}] {a['op'][0]}:{a['op'][1]:02d} {a['op_name']}: "
                         f"lot HARDCODED = {a['lot_static']} (not parameterized)")
    verdict = ''
    if actual_offsets:
        if declared_lot_off in actual_offsets:
            verdict = f"   ✓ CORRECT: declared lot_off={declared_lot_off} matches actual"
        else:
            verdict = (f"   ✗ WRONG: declared lot_off={declared_lot_off}, "
                       f"actual offset(s): {sorted(actual_offsets)}")
    else:
        verdict = (f"   ⚠ HARDCODED LOTS: declared lot_off={declared_lot_off} doesn't apply; "
                   f"this template always awards fixed lot(s)")
    return '\n'.join(lines) + '\n' + verdict


for eid in sorted(TEMPLATE_EVENTS.keys()):
    ent_off, lot_off, min_args = TEMPLATE_EVENTS[eid]
    print(f"=== Template {eid}  (declared entity_off={ent_off}, lot_off={lot_off}) ===")
    print(analyze_event(eid, lot_off))
    print()
