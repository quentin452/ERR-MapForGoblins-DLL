"""Scan m32_00 EMEVD — for each event, find EntityIDs and flag IDs co-occurring.
Output pairs: (entity_id, candidate_flag_ids)."""
import sys, io, os, tempfile, struct
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
sys.path.insert(0, '.')
import config
from pathlib import Path
from pythonnet import load; load('coreclr')
import clr
clr.AddReference(str(config.SOULSFORMATS_DLL))
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
import SoulsFormats

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
_str_type = SysType.GetType('System.String')
_emevd_read = asm.GetType('SoulsFormats.EMEVD').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)

# Load m32_00 EMEVD
emevd_path = config.require_err_mod_dir() / 'event' / 'm32_00_00_00.emevd.dcx'
data = SoulsFormats.DCX.Decompress(str(emevd_path)).ToArray()
tmp = os.path.join(tempfile.gettempdir(), '_mfg_emevd.tmp')
SysFile.WriteAllBytes(tmp, data)
emevd = _emevd_read.Invoke(None, Array[Object]([tmp]))
os.unlink(tmp)

# Our target MSB entity IDs for AEG099_860 in m32_00
target_entities = {
    32001201: 'AEG099_860_9001',
    32001205: 'AEG099_860_9002',
    32001206: 'AEG099_860_9003',
    32001215: 'AEG099_860_9007',
    32001216: 'AEG099_860_9005',
    32001219: 'AEG099_860_9004',
}

# Block 31220 flags (set in save — these are the "collected" flags)
set_flags = {31220201, 31220205, 31220206, 31220215, 31220216, 31220219,
             31220510, 31220511, 31220519, 31220590, 31220800, 31220801}

# For each event, gather all u32 values in all instruction args
# Find events that contain target_entities AND set_flags
print('Events with co-occurring entity and flag:')
for ev in emevd.Events:
    evid = int(ev.ID)
    values = set()
    for ins in ev.Instructions:
        try:
            ab = bytes(ins.ArgData) if ins.ArgData else b''
        except: continue
        for i in range(0, len(ab) - 3):
            v = struct.unpack_from('<I', ab, i)[0]
            values.add(v)

    found_ents = target_entities.keys() & values
    found_flags = set_flags & values
    if found_ents and found_flags:
        print(f'\nEvent {evid}: entities={[(e, target_entities[e]) for e in found_ents]} flags={found_flags}')
        # Print instruction list
        for k, ins in enumerate(ev.Instructions):
            try:
                bank = int(ins.Bank); iid = int(ins.ID)
                ab = bytes(ins.ArgData) if ins.ArgData else b''
            except: continue
            # Only print if contains entity or flag
            has_any = any(struct.pack('<I', v) in ab for v in found_ents | found_flags)
            if has_any:
                refs = []
                for v in found_ents:
                    if struct.pack('<I', v) in ab: refs.append(f'ent {v}')
                for v in found_flags:
                    if struct.pack('<I', v) in ab: refs.append(f'flag {v}')
                print(f'  [{k}] Bank:{bank}[{iid}]  refs={refs}')
