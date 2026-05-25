import sys, io, os, tempfile
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
_pcls = asm.GetType('SoulsFormats.PARAM')
_pr = _pcls.BaseType.GetMethod('Read', Array[SysType]([_str]))

defs = {}
for xml in config.PARAMDEF_DIR.glob('*.xml'):
    try:
        d = SoulsFormats.PARAMDEF.XmlDeserialize(str(xml), False)
        if d and d.ParamType:
            defs[str(d.ParamType)] = d
    except Exception:
        pass

bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(config.ERR_MOD_DIR / 'regulation.bin'))
for f in bnd.Files:
    if 'WorldMapPieceParam' not in str(f.Name):
        continue
    tmp = os.path.join(tempfile.gettempdir(), '_p.param')
    SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
    p = _pr.Invoke(None, Array[Object]([tmp]))
    pdef = defs.get(str(p.ParamType))
    if pdef:
        p.ApplyParamdef(pdef)
    print(f"ParamType: {p.ParamType}; rows: {p.Rows.Count}")
    if p.Rows.Count > 0:
        r = p.Rows[0]
        print("First row fields:")
        for c in r.Cells:
            print(f"  {c.Def.InternalName} = {c.Value}")
        print("\nSample rows:")
        for i in range(min(15, p.Rows.Count)):
            row = p.Rows[i]
            vals = {}
            for c in row.Cells:
                try:
                    vals[str(c.Def.InternalName)] = int(c.Value)
                except Exception:
                    pass
            print(f"  id={int(row.ID)}: {vals}")
    break
