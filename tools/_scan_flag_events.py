"""Find events in m32_00 EMEVD that reference specific SET flags."""
import sys, io, os, tempfile, struct
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
sys.path.insert(0, '.')
import config
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

set_flags = [31220201, 31220205, 31220206, 31220215, 31220216, 31220219,
             31220510, 31220511, 31220519, 31220590, 31220800, 31220801]

target_entities = {
    32001201: 'AEG099_860_9001',
    32001205: 'AEG099_860_9002',
    32001206: 'AEG099_860_9003',
    32001215: 'AEG099_860_9007',
    32001216: 'AEG099_860_9005',
    32001219: 'AEG099_860_9004',
}

# Also search for AEG099_780 model hash + block 31220 flag references
aeg780 = 10099780

# Find all events referencing each flag OR entity
for flag in set_flags:
    print(f'\n=== flag {flag} (block 31220) ===')
    for ev in emevd.Events:
        evid = int(ev.ID)
        all_vals = set()
        for ins in ev.Instructions:
            try:
                ab = bytes(ins.ArgData) if ins.ArgData else b''
            except: continue
            for i in range(0, len(ab) - 3):
                v = struct.unpack_from('<I', ab, i)[0]
                all_vals.add(v)
        if flag in all_vals:
            # Also list what else is there
            ents = [e for e in target_entities if e in all_vals]
            has_780 = aeg780 in all_vals
            extra = []
            if ents: extra.append(f'entities={ents}')
            if has_780: extra.append('AEG099_780')
            # Filter out trivial / common values
            interesting = [v for v in all_vals if
                          (100 < v < 1000000) or
                          (v > 1000000 and v < 0x7FFFFFFF)]
            interesting = sorted(interesting)[:20]
            print(f'  event {evid}: len={len(ev.Instructions)} {" ".join(extra)}')
