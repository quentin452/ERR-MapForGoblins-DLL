"""Dump Event 0 initializer args — looking for flag IDs and entity IDs together."""
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

emevd_path = config.require_err_mod_dir() / 'event' / 'm32_00_00_00.emevd.dcx'
data = SoulsFormats.DCX.Decompress(str(emevd_path)).ToArray()
tmp = os.path.join(tempfile.gettempdir(), '_mfg_emevd.tmp')
SysFile.WriteAllBytes(tmp, data)
emevd = _emevd_read.Invoke(None, Array[Object]([tmp]))
os.unlink(tmp)

target_entities = {32001201, 32001205, 32001206, 32001215, 32001216, 32001219}
set_flags = {31220201, 31220205, 31220206, 31220215, 31220216, 31220219,
             31220510, 31220511, 31220519, 31220590, 31220800, 31220801}

# Event 0 is the initializer — its Instructions call common events
for ev in emevd.Events:
    if int(ev.ID) != 0: continue
    print(f'Event 0: {len(ev.Instructions)} initializers')
    for k, ins in enumerate(ev.Instructions):
        try:
            bank = int(ins.Bank); iid = int(ins.ID)
            ab = bytes(ins.ArgData) if ins.ArgData else b''
        except: continue
        if bank not in (2000,): continue
        # Parse all u32 in the blob
        vals = []
        for i in range(0, len(ab) - 3, 4):
            vals.append(struct.unpack_from('<I', ab, i)[0])
        # Check if ANY target or flag in this init
        has_ent = any(v in target_entities for v in vals)
        has_flag = any(v in set_flags for v in vals)
        has_block_31220 = any(31220000 <= v < 31221000 for v in vals)
        if has_ent or has_flag or has_block_31220:
            interesting_ents = [v for v in vals if v in target_entities]
            interesting_flags = [v for v in vals if v in set_flags]
            interesting_block = [v for v in vals if 31220000 <= v < 31221000 and v not in set_flags]
            event_called = vals[1] if len(vals) >= 2 else None
            print(f'\n  [{k}] Bank:{bank}[{iid}]  RunEvent target={event_called}')
            print(f'      args = {ab.hex()}')
            print(f'      vals = {vals}')
            if interesting_ents: print(f'      entities: {interesting_ents}')
            if interesting_flags: print(f'      SET flags: {interesting_flags}')
            if interesting_block: print(f'      block 31220 other: {interesting_block}')
    break
