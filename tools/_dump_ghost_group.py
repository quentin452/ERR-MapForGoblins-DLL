#!/usr/bin/env python3
"""
Dump the emevd event(s) that handle the 8-ghost group at m60_35_50:
  entities 1035500219, 1035500223..1035500230 (c4250 Cemetery Shades)
  shared flag 520000, dynamic lots 20000/20001/20002.

Reads mod's m60_35_50_00.emevd.dcx and common_func.emevd.dcx; prints any
event whose instructions reference these entity/flag/lot constants.
"""
import sys, io, struct
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
from pathlib import Path
import config
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly
from System import Array, Type as SysType, Object

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats

_string_type = SysType.GetType('System.String')
_emevd_cls = asm.GetType('SoulsFormats.EMEVD')
_emevd_read = _emevd_cls.BaseType.GetMethod('Read', Array[SysType]([_string_type]))
assert _emevd_read is not None, 'EMEVD.Read(string) not found'

EVENT_DIR = config.ERR_MOD_DIR / 'event'

GHOST_ENTITIES = {1035500219, 1035500223, 1035500224, 1035500225,
                  1035500226, 1035500227, 1035500228, 1035500229,
                  1035500230}
SHARED_FLAG = 520000
DYNAMIC_LOTS = {20000, 20001, 20002}
INTEREST = GHOST_ENTITIES | {SHARED_FLAG} | DYNAMIC_LOTS

def as_i32_words(b):
    words = []
    for i in range(0, len(b) - 3, 4):
        words.append(struct.unpack_from('<i', b, i)[0])
    return words

def event_param_summary(event):
    """Return a dict instrIdx -> list of (targetByte, srcByte, len) parameter binds."""
    out = {}
    for p in event.Parameters:
        out.setdefault(int(p.InstructionIndex), []).append(
            (int(p.TargetStartByte), int(p.SourceStartByte), int(p.ByteCount))
        )
    return out

def dump_event(map_name, event, why):
    print(f"\n=== {map_name} event {int(event.ID)} ({len(event.Instructions)} instr) "
          f"restBehavior={event.RestBehavior}  matched: {why}")
    params = event_param_summary(event)
    if event.Parameters.Count:
        print(f"  Parameters ({event.Parameters.Count}):")
        for p in event.Parameters:
            print(f"    instr#{int(p.InstructionIndex)} targetByte={int(p.TargetStartByte)} "
                  f"srcByte={int(p.SourceStartByte)} len={int(p.ByteCount)}")
    for idx, instr in enumerate(event.Instructions):
        b = bytes(instr.ArgData) if instr.ArgData else b''
        words = as_i32_words(b)
        bp = params.get(idx, [])
        bp_str = f"  params={bp}" if bp else ''
        print(f"  [{idx}] {int(instr.Bank)}:{int(instr.ID):02d} args_i32={words}{bp_str}")

def scan_file(path):
    emevd = _emevd_read.Invoke(None, Array[Object]([str(path)]))
    name = path.name.replace('.emevd.dcx', '')
    interest_hits = 0
    init_hits = []
    for event in emevd.Events:
        eid = int(event.ID)
        matched = set()
        # check both instruction args AND init-event parameters (event 0)
        for idx, instr in enumerate(event.Instructions):
            b = bytes(instr.ArgData) if instr.ArgData else b''
            for i in range(0, len(b) - 3):
                v = struct.unpack_from('<i', b, i)[0]
                if v in INTEREST:
                    matched.add(v)
            # For event 0 (init), instruction 2000:00 spawns events with params.
            # We track those separately below.
        if matched:
            dump_event(name, event, sorted(matched))
            interest_hits += 1

        # Special: event 0 contains InitializeEvent (2000:00) — args[0]=slot, args[1]=eventId, then params
        if eid == 0:
            for instr in event.Instructions:
                if int(instr.Bank) == 2000 and int(instr.ID) == 0:
                    b = bytes(instr.ArgData) if instr.ArgData else b''
                    words = as_i32_words(b)
                    # Check if any of our entities appears in params
                    hit = [w for w in words if w in INTEREST]
                    if hit:
                        init_hits.append((words, hit))
    if init_hits:
        print(f"\n=== {name} event 0 InitializeEvent calls referencing our constants:")
        for words, hit in init_hits[:30]:
            print(f"   InitializeEvent slot={words[0]} eventId={words[1]} params={words[2:]}  hits={hit}")
        if len(init_hits) > 30:
            print(f"   ...{len(init_hits)-30} more")
    return interest_hits

def dump_specific(path, target_event_ids):
    emevd = _emevd_read.Invoke(None, Array[Object]([str(path)]))
    name = path.name.replace('.emevd.dcx', '')
    print(f"\n=== {name}: dumping events {target_event_ids} ===")
    for event in emevd.Events:
        eid = int(event.ID)
        if eid in target_event_ids:
            dump_event(name, event, ['explicit'])

# Dump the call-site template events explicitly (they pass lots/flags via params)
dump_specific(EVENT_DIR / 'common_func.emevd.dcx',
              {90005201, 90005210, 90005250, 90005251, 90005260})
