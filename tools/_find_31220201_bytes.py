"""Search raw m32_00 EMEVD binary for literal bytes of flag 31220201."""
import sys, struct, os, tempfile
sys.path.insert(0, '.')
import config
from pythonnet import load; load('coreclr')
import clr
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats
from System.IO import File as SysFile

emevd_path = config.require_err_mod_dir() / 'event' / 'm32_00_00_00.emevd.dcx'
data = bytes(SoulsFormats.DCX.Decompress(str(emevd_path)).ToArray())
print(f'EMEVD size: {len(data)}')

# Search for 31220201 = 0x01DC6A89 → bytes 89 6A DC 01
for target_flag in [31220201, 31220205, 31220206, 31220215, 31220216, 31220219,
                     31220510, 31220511, 31220519, 31220590, 31220800, 31220801,
                     32000201, 32001201, 32002250]:
    pat = struct.pack('<I', target_flag)
    off = 0
    positions = []
    while True:
        i = data.find(pat, off)
        if i < 0: break
        positions.append(i)
        off = i + 4
    print(f'  flag {target_flag} bytes {pat.hex()}: {len(positions)} hits at {positions[:5]}')
