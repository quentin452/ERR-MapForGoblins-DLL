"""Inspect ShopLineupParam row 57258000 (the Lhutel reward entry)."""
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

for tag, reg in [('MOD', config.ERR_MOD_DIR / 'regulation.bin'),
                 ('VAN', config.GAME_DIR / 'regulation.bin')]:
    print(f"\n========== {tag} ==========")
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(reg))
    for f in bnd.Files:
        fn = str(f.Name)
        if 'ShopLineupParam.param' not in fn:
            continue
        tmp = os.path.join(tempfile.gettempdir(), '_p.param')
        SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
        p = _pr.Invoke(None, Array[Object]([tmp]))
        pdef = defs.get(str(p.ParamType))
        if pdef:
            p.ApplyParamdef(pdef)
        # Print row 57258000 + neighbors (look for Lhutel-related shop rows in range)
        targets = {57258000}
        # Also: find any row with equipId = 358000 (in mod) or 258000 (in vanilla)
        TARGET_GOODS = 358000 if tag == 'MOD' else 258000
        lhutel_rows = []
        for r in p.Rows:
            rid = int(r.ID)
            for c in r.Cells:
                if str(c.Def.InternalName) == 'equipId':
                    try:
                        v = int(c.Value)
                    except Exception:
                        continue
                    if v == TARGET_GOODS:
                        lhutel_rows.append(rid)
        print(f"  Rows where equipId == {TARGET_GOODS}: {lhutel_rows}")
        for rid in lhutel_rows:
            print(f"\n  --- Row {rid} (full non-default fields) ---")
            for r in p.Rows:
                if int(r.ID) != rid:
                    continue
                for c in r.Cells:
                    nm = str(c.Def.InternalName)
                    try:
                        v = int(c.Value)
                    except Exception:
                        continue
                    if v not in (0, -1):
                        print(f"    {nm} = {v}")
                break
        break
