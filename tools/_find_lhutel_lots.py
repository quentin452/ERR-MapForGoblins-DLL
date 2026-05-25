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

# Goods 358000 in MOD = Lhutel; 258000 in VAN = Lhutel; mod's 258000 = "Depleted Lhutel"
for tag, reg in [('MOD', config.ERR_MOD_DIR / 'regulation.bin'),
                 ('VAN', config.GAME_DIR / 'regulation.bin')]:
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(reg))
    for f in bnd.Files:
        fn = str(f.Name)
        if 'ItemLotParam' not in fn:
            continue
        base = fn.split('\\')[-1].split('/')[-1]
        tmp = os.path.join(tempfile.gettempdir(), '_p.param')
        SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
        p = _pr.Invoke(None, Array[Object]([tmp]))
        pdef = defs.get(str(p.ParamType))
        if pdef:
            p.ApplyParamdef(pdef)
        targets = {358000, 258000}  # both possible Lhutel IDs
        hits = []
        for r in p.Rows:
            rid = int(r.ID)
            for c in r.Cells:
                nm = str(c.Def.InternalName)
                if not nm.startswith('lotItemId0'):
                    continue
                try:
                    v = int(c.Value)
                except Exception:
                    continue
                if v in targets:
                    slot = int(nm[-1])
                    flag = 0
                    for c2 in r.Cells:
                        if str(c2.Def.InternalName) == 'getItemFlagId':
                            try:
                                flag = int(c2.Value)
                            except Exception:
                                pass
                            break
                    hits.append((rid, slot, v, flag))
        if hits:
            print(f"\n  [{tag}] {base}:")
            for rid, slot, iid, flag in hits:
                print(f"    row {rid} slot{slot} id={iid} flag={flag}")
