import sys, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pythonnet import load
load('coreclr')
from System.Reflection import Assembly
from System import Array, Type as SysType, Object
asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
import SoulsFormats
_str = SysType.GetType('System.String')
_bcls = asm.GetType('SoulsFormats.BND4')
_br = _bcls.BaseType.GetMethod('Read', Array[SysType]([_str]))

bnd = _br.Invoke(None, Array[Object]([str(config.ERR_MOD_DIR / 'msg/engus/item_dlc02.msgbnd.dcx')]))
for f in bnd.Files:
    n = str(f.Name).split('\\')[-1]
    print(f"  slot={int(f.ID):3d}  {n}")
