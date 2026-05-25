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

for target in ('BonfireWarpParam', 'GameAreaParam', 'MapGdRegionInfoParam',
               'MapGdRegionDrawParam', 'PlayRegionParam'):
    for f in bnd.Files:
        if target not in str(f.Name):
            continue
        tmp = os.path.join(tempfile.gettempdir(), '_p.param')
        SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
        p = _pr.Invoke(None, Array[Object]([tmp]))
        pdef = defs.get(str(p.ParamType))
        if pdef:
            p.ApplyParamdef(pdef)
        print(f"\n=== {target} (ParamType={p.ParamType}, rows={p.Rows.Count}) ===")
        if p.Rows.Count == 0:
            continue
        # Print fields of first row
        first = p.Rows[0]
        print(f"First row id={int(first.ID)} fields:")
        for c in first.Cells:
            nm = str(c.Def.InternalName)
            try:
                v = int(c.Value)
            except Exception:
                v = c.Value
            print(f"  {nm} = {v}")
        # Sample a few more rows compactly
        print("Sample IDs:", [int(p.Rows[i].ID) for i in range(min(20, p.Rows.Count))])
        break
