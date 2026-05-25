#!/usr/bin/env python3
"""Find ALL award-containing templates in common_func.emevd that are called via
InitializeEvent (2000:06) from per-map emevds, then compare against the current
TEMPLATE_EVENTS list in extract_all_items.py.

For each candidate, report:
  - which events in common_func contain Award Item Lot / Award Items Inc Clients
  - which of those are called via 2000:06 (i.e. are real templates)
  - for each template: derive entity_off and lot_off from parameter bindings
  - whether it's already in TEMPLATE_EVENTS (and if so, verify offsets match)
  - if not in TEMPLATE_EVENTS: estimate how many (entity, lot) pairs we'd gain
"""
import sys, io, json, struct
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
from pathlib import Path
from collections import defaultdict
import config
from pythonnet import load; load('coreclr')
from System.Reflection import Assembly
from System import Array, Type as SysType, Object
asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
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

# Identify award instructions
AWARD_KEYS = {k for k, v in INSTR.items() if 'award item lot' in v['name'].lower()
              or 'award items' in v['name'].lower() and 'item' in v['name'].lower()}
AWARD_LOT_ARG_IDX = {}
for k in AWARD_KEYS:
    for i, a in enumerate(INSTR[k]['args']):
        if 'item lot' in a['name'].lower():
            AWARD_LOT_ARG_IDX[k] = i
            break
print(f"Award instructions in ER emedf: {[(k, INSTR[k]['name']) for k in sorted(AWARD_KEYS)]}")

# Current TEMPLATE_EVENTS list
CURRENT_TEMPLATES = {
    90005200, 90005210, 90005300, 90005301,
    90005860, 90005861, 90005880, 90005881, 90005882, 90005883, 90005885,
    90005750, 90005774, 90005792, 90005632, 90005110, 90005390, 90005555,
}

# 1) Load common_func.emevd, find every event containing an award instruction
em_func = _emr.Invoke(None, Array[Object]([str(config.ERR_MOD_DIR/'event'/'common_func.emevd.dcx')]))
event_awards = {}  # event_id -> [{instr_idx, op, lot_static, lot_param_srcByte}, ...]
for ev in em_func.Events:
    eid = int(ev.ID)
    params_by_idx = defaultdict(list)
    for p in ev.Parameters:
        params_by_idx[int(p.InstructionIndex)].append(
            (int(p.TargetStartByte), int(p.SourceStartByte), int(p.ByteCount))
        )
    award_instrs = []
    for idx, ins in enumerate(ev.Instructions):
        key = (int(ins.Bank), int(ins.ID))
        if key not in AWARD_KEYS: continue
        lot_arg = AWARD_LOT_ARG_IDX.get(key)
        if lot_arg is None: continue
        b = bytes(ins.ArgData) if ins.ArgData else b''
        words = [struct.unpack_from('<i', b, i)[0] for i in range(0, len(b)-3, 4)]
        lot_static = words[lot_arg] if lot_arg < len(words) else None
        param_src = None
        for tb, sb, bc in params_by_idx.get(idx, []):
            if tb == lot_arg * 4:
                param_src = sb
                break
        award_instrs.append({
            'instr_idx': idx,
            'op': key,
            'op_name': INSTR[key]['name'],
            'lot_static': lot_static,
            'lot_param_src': param_src,
        })
    # Also find entity parameter (look at first instruction with entity-like param binding)
    # Heuristic: find any instruction with Entity ID arg bound to a srcByte
    entity_srcs = set()
    for idx, ins in enumerate(ev.Instructions):
        key = (int(ins.Bank), int(ins.ID))
        meta = INSTR.get(key)
        if not meta: continue
        for arg_i, a in enumerate(meta['args']):
            if 'entity' in a['name'].lower() or 'target' in a['name'].lower() or 'character' in a['name'].lower():
                for tb, sb, bc in params_by_idx.get(idx, []):
                    if tb == arg_i * 4:
                        entity_srcs.add(sb)
                        break
    if award_instrs:
        event_awards[eid] = {
            'awards': award_instrs,
            'entity_srcs': entity_srcs,
            'parameters': params_by_idx,
        }

print(f"\ncommon_func events containing award instructions: {len(event_awards)}")

# 2) Find which of these events are called via 2000:06 InitializeEvent from per-map emevds
template_callers = defaultdict(list)  # template_eid -> [(map, caller_args), ...]
EVENT_DIR = config.ERR_MOD_DIR / 'event'
for path in sorted(EVENT_DIR.glob('*.emevd.dcx')):
    if path.name == 'common_func.emevd.dcx': continue
    em = _emr.Invoke(None, Array[Object]([str(path)]))
    map_name = path.name.replace('.emevd.dcx', '')
    for ev in em.Events:
        for ins in ev.Instructions:
            if int(ins.Bank)!=2000 or int(ins.ID)!=6: continue
            b = bytes(ins.ArgData) if ins.ArgData else b''
            if len(b) < 8: continue
            tid = struct.unpack_from('<i', b, 4)[0]
            if tid in event_awards:
                args = [struct.unpack_from('<i', b, i)[0] for i in range(0, len(b)-3, 4)]
                template_callers[tid].append((map_name, args))

print(f"\ntemplates with InitializeEvent callers from per-map emevds: {len(template_callers)}")

# 3) For each template: report status
print("\n=== TEMPLATES ANALYSIS ===\n")
for tid in sorted(event_awards.keys()):
    callers = template_callers.get(tid, [])
    awards = event_awards[tid]['awards']
    entity_srcs = event_awards[tid]['entity_srcs']
    in_current = tid in CURRENT_TEMPLATES
    has_callers = bool(callers)

    # Determine lot offset (srcByte → InitializeEvent byte = srcByte + 8)
    lot_offsets = set()
    hardcoded_lots = []
    for a in awards:
        if a['lot_param_src'] is not None:
            lot_offsets.add(a['lot_param_src'] + 8)
        else:
            hardcoded_lots.append(a['lot_static'])

    # Determine entity offset (smallest entity-bound srcByte + 8 is usually entity_off)
    entity_offsets = sorted({s + 8 for s in entity_srcs})

    status = '✓ in current' if in_current else '⚠ NEW'
    if not has_callers:
        status += ' (no callers — unused)'
    print(f"{status}  template {tid}  ({len(callers)} callers)")
    print(f"    award instrs: {len(awards)}")
    if lot_offsets:
        print(f"    lot offsets (caller byte): {sorted(lot_offsets)}")
    if hardcoded_lots:
        print(f"    HARDCODED lots: {hardcoded_lots[:5]}")
    if entity_offsets:
        print(f"    entity offsets (caller byte, heuristic): {entity_offsets[:5]}")
    if callers:
        sample = callers[0][1]
        print(f"    sample caller args: {sample[:10]}")
    print()

# 4) Summary of NEW templates (with callers but not in current list)
new_with_callers = [t for t in event_awards if t not in CURRENT_TEMPLATES and template_callers.get(t)]
print(f"\n=== NEW awarder templates not in current list (with callers): {len(new_with_callers)} ===")
for t in sorted(new_with_callers):
    print(f"  {t}: {len(template_callers[t])} callers")

# 5) Estimate records that would be ADDED if we include these new templates
added_pairs = set()
for t in new_with_callers:
    awards = event_awards[t]['awards']
    if not awards: continue
    # Use first award's lot_param_src if available
    lot_src = None
    for a in awards:
        if a['lot_param_src'] is not None:
            lot_src = a['lot_param_src']; break
    if lot_src is None: continue  # hardcoded — no per-caller variation
    lot_off = lot_src + 8
    # Determine entity offset (first entity-bound srcByte)
    entity_srcs = event_awards[t]['entity_srcs']
    if not entity_srcs: continue
    ent_off = min(entity_srcs) + 8
    # Extract (entity, lot) from each caller
    for map_name, args in template_callers[t]:
        b = b''.join(struct.pack('<i', v) for v in args)
        if len(b) < max(ent_off, lot_off) + 4: continue
        ent = struct.unpack_from('<i', b, ent_off)[0]
        lot = struct.unpack_from('<i', b, lot_off)[0]
        if ent > 0 and lot > 0:
            added_pairs.add((ent, lot, t))

print(f"\n  Total (entity, lot) pairs that would be added: {len(added_pairs)}")
unique_lots = sorted({l for e,l,t in added_pairs})
print(f"  Unique lots referenced: {len(unique_lots)} (sample: {unique_lots[:15]})")
