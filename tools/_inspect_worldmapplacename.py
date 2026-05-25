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
_pcls = asm.GetType('SoulsFormats.PARAM')
_pr = _pcls.BaseType.GetMethod('Read', Array[SysType]([_str]))
_fcls = asm.GetType('SoulsFormats.FMG')
_fr = _fcls.BaseType.GetMethod('Read', Array[SysType]([_str]))
_bcls = asm.GetType('SoulsFormats.BND4')
_br = _bcls.BaseType.GetMethod('Read', Array[SysType]([_str]))

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
    if 'WorldMapPlaceNameParam' not in str(f.Name):
        continue
    tmp = os.path.join(tempfile.gettempdir(), '_p.param')
    SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
    p = _pr.Invoke(None, Array[Object]([tmp]))
    pdef = defs.get(str(p.ParamType))
    if pdef:
        p.ApplyParamdef(pdef)
    print(f"ParamType: {p.ParamType}")
    print(f"Rows: {p.Rows.Count}")
    # Print first row's structure
    if p.Rows.Count > 0:
        r = p.Rows[0]
        print(f"\nFirst row id={int(r.ID)}; fields:")
        for c in r.Cells:
            print(f"  {c.Def.InternalName}  ({c.Def.DisplayType}) = {c.Value}")
        # Print 5 sample rows compactly
        print(f"\nSample rows:")
        for i in range(min(10, p.Rows.Count)):
            row = p.Rows[i]
            vals = {}
            for c in row.Cells:
                nm = str(c.Def.InternalName)
                try:
                    vals[nm] = int(c.Value)
                except Exception:
                    vals[nm] = c.Value
            print(f"  id={int(row.ID)}: {vals}")
    break

# Also load PlaceName FMG for cross-reference
place_names = {}
bndmsg = _br.Invoke(None, Array[Object]([str(config.ERR_MOD_DIR / 'msg/engus/item_dlc02.msgbnd.dcx')]))
for f in bndmsg.Files:
    if 'PlaceName' in str(f.Name) and 'tutorial' not in str(f.Name).lower():
        tmp = os.path.join(tempfile.gettempdir(), '_p.fmg')
        SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
        fmg = _fr.Invoke(None, Array[Object]([tmp]))
        for e in fmg.Entries:
            t = str(e.Text) if e.Text else ''
            if t and t != '[ERROR]':
                place_names[int(e.ID)] = re.sub(r'<[^>]+>', '', t)
        break
print(f"\nPlaceName entries loaded: {len(place_names)}")
# Look at Weeping Peninsula etc
for k, v in sorted(place_names.items()):
    if any(s in v for s in ('Weeping', 'Mt. Gelmir', 'Gelmir', 'Liurnia', 'Caelid', 'Limgrave', 'Altus')):
        print(f"  {k}: {v!r}")
