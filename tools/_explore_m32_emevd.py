"""Dump all events from m32_00_00_00.emevd + search for AEG099_780 model hash
and flag 31220201 references."""
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

# Byte patterns to look for:
# 31220201 = 0x01DC6A89 → bytes 89 6A DC 01
# AEG099_780 model hash in decimal: 10099780 → LE bytes: 84 1C 9A 00
# AEG099_860 hash: 10099860 → bytes 94 1C 9A 00

flag_31220201 = struct.pack('<I', 31220201)
flag_31220800 = struct.pack('<I', 31220800)
aeg780 = struct.pack('<I', 10099780)
aeg860 = struct.pack('<I', 10099860)
entity_32001201 = struct.pack('<I', 32001201)

for ev in emevd.Events:
    evid = int(ev.ID)
    # Combine all instructions' args into one blob to scan
    all_args = b''
    for ins in ev.Instructions:
        try:
            all_args += bytes(ins.ArgData) if ins.ArgData else b''
        except: pass

    has_flag201 = flag_31220201 in all_args
    has_flag800 = flag_31220800 in all_args
    has_780 = aeg780 in all_args
    has_860 = aeg860 in all_args
    has_ent201 = entity_32001201 in all_args

    if has_flag201 or has_flag800 or has_780 or has_ent201:
        print(f'\nEvent {evid}: flag31220201={has_flag201} flag31220800={has_flag800} '
              f'aeg780={has_780} aeg860={has_860} ent32001201={has_ent201}')
        # List each instruction
        for k, ins in enumerate(ev.Instructions):
            try:
                ab = bytes(ins.ArgData) if ins.ArgData else b''
                ident = f'Bank:{int(ins.Bank)}[{int(ins.ID)}]'
                print(f'  [{k}] {ident}  args={ab.hex()}')
            except: pass

# Also check Initializers (calls to common events)
print('\n\n=== Event 0 initializers (calls to common_func) ===')
for ev in emevd.Events:
    if int(ev.ID) != 0: continue
    for ins in ev.Instructions:
        try:
            bank = int(ins.Bank); iid = int(ins.ID)
            ab = bytes(ins.ArgData) if ins.ArgData else b''
        except: continue
        if bank != 2000 or iid not in (0, 6): continue
        if len(ab) < 8: continue
        # RunEvent: arg[0:4]=unk, arg[4:8]=event_id, then params
        event_id = struct.unpack_from('<I', ab, 4)[0] if bank == 2000 and iid == 0 else struct.unpack_from('<I', ab, 4)[0]
        # Look for AEG099_780 model or 31220xxx flag in args
        has_780 = aeg780 in ab
        has_flag3122 = any(struct.pack('<I', 31220000 + i) in ab for i in range(1000))
        if has_780 or has_flag3122:
            print(f'RunEvent ({bank}:{iid}) -> event {event_id}: args={ab.hex()}')
    break
