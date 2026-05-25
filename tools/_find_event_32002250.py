"""Find event 32002250 definition in m32_00 EMEVD."""
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

for fname in ['m32_00_00_00.emevd.dcx', 'common.emevd.dcx', 'common_func.emevd.dcx']:
    emevd_path = config.require_err_mod_dir() / 'event' / fname
    if not emevd_path.exists(): continue
    data = SoulsFormats.DCX.Decompress(str(emevd_path)).ToArray()
    tmp = os.path.join(tempfile.gettempdir(), '_mfg_emevd.tmp')
    SysFile.WriteAllBytes(tmp, data)
    emevd = _emevd_read.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)

    print(f'\n===== {fname} =====')
    for ev in emevd.Events:
        evid = int(ev.ID)
        if evid != 32002250: continue
        print(f'Event {evid}: {len(ev.Instructions)} instructions')
        # Also dump parameters
        try:
            params_list = list(ev.Parameters)
            print(f'Parameters: {len(params_list)}')
            for p in params_list:
                # parameter has InstructionIndex, TargetStartByte, SourceStartByte, ByteCount
                print(f'  InstrIdx={p.InstructionIndex} TargetByte={p.TargetStartByte} SourceByte={p.SourceStartByte} Bytes={p.ByteCount}')
        except Exception as e:
            print(f'params err: {e}')
        for k, ins in enumerate(ev.Instructions):
            try:
                bank = int(ins.Bank); iid = int(ins.ID)
                ab = bytes(ins.ArgData) if ins.ArgData else b''
                # Decode some common instruction names
                name = f'Bank:{bank}[{iid}]'
                # IfEventFlag = 1003[1]
                # SetEventFlag = 2003[36] or 2003[69]
                # IfItemGet = ...
                if bank == 1003 and iid == 1: name = 'IfEventFlag'
                elif bank == 2003 and iid == 36: name = 'SetEventFlag'
                elif bank == 2003 and iid == 12: name = 'SetEventFlagAsync'
                elif bank == 2003 and iid == 66: name = 'SetEventFlag66'
                elif bank == 2003 and iid == 69: name = 'SetEventFlag69'
                elif bank == 2007 and iid == 1: name = 'DisplayDialog'
                elif bank == 2006 and iid == 4: name = 'CreateSFX'
                vals = [struct.unpack_from('<I', ab, i)[0] for i in range(0, max(0, len(ab) - 3), 4)]
                print(f'  [{k}] {name}  args={ab.hex()}  vals={vals}')
            except Exception as e:
                print(f'  [{k}] err: {e}')
