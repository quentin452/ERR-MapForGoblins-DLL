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

for src in [config.ERR_MOD_DIR / 'msg/engus', config.GAME_DIR / 'msg/engus']:
    print(f"\n=== {src} ===")
    for p in sorted(src.glob('*.msgbnd.dcx')):
        bnd = _br.Invoke(None, Array[Object]([str(p)]))
        names = sorted({str(f.Name).rsplit('\\', 1)[-1] for f in bnd.Files})
        print(f"\n  {p.name}: {len(names)} FMGs")
        for n in names:
            print(f"    {n}")
