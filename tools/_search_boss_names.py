import sys, io, os, tempfile, re
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pythonnet import load
load('coreclr')
from System.Reflection import Assembly
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
import SoulsFormats
_str = SysType.GetType('System.String')
_fcls = asm.GetType('SoulsFormats.FMG')
_fr = _fcls.BaseType.GetMethod('Read', Array[SysType]([_str]))
_bcls = asm.GetType('SoulsFormats.BND4')
_br = _bcls.BaseType.GetMethod('Read', Array[SysType]([_str]))

patterns = re.compile(r'(necromancer garris|stray mimic tear|patches\b|^mimic tear|cemetery shade)',
                      re.IGNORECASE)

for src_root in [config.ERR_MOD_DIR / 'msg/engus', config.GAME_DIR / 'msg/engus']:
    print(f'\n=== {src_root} ===')
    for p in sorted(src_root.glob('*.msgbnd.dcx')):
        bnd = _br.Invoke(None, Array[Object]([str(p)]))
        for f in bnd.Files:
            full = str(f.Name)
            if not full.lower().endswith('.fmg'):
                continue
            tmp = os.path.join(tempfile.gettempdir(), '_x.fmg')
            SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
            try:
                fmg = _fr.Invoke(None, Array[Object]([tmp]))
            except Exception:
                continue
            for e in fmg.Entries:
                t = str(e.Text) if e.Text else ''
                if t and patterns.search(t):
                    fname = full.rsplit('\\', 1)[-1]
                    print(f"  {p.name}/{fname}  id={int(e.ID)}  text={t!r}")
