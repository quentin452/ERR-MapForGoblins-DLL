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

for tag, reg_path in [('MOD', config.ERR_MOD_DIR / 'regulation.bin'),
                       ('VAN', config.GAME_DIR / 'regulation.bin')]:
    print(f"\n========= {tag} regulation =========")
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(reg_path))
    for f in bnd.Files:
        fname = str(f.Name)
        is_area = 'GameAreaParam.param' in fname
        is_speffect = 'SpEffectParam.param' in fname
        if not (is_area or is_speffect):
            continue
        target_ids = {30000800} if is_area else {
            90100, 5404, 17256, 7020, 17253, 90400, 90000, 90010, 90020, 90030,
            90040, 95050, 95065, 5800, 17254, 17275, 4301, 17700, 36640000,
            96053, 96045, 96033, 96024, 160090}
        tmp = os.path.join(tempfile.gettempdir(), '_p.param')
        SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
        p = _pr.Invoke(None, Array[Object]([tmp]))
        pdef = defs.get(str(p.ParamType))
        if pdef:
            p.ApplyParamdef(pdef)
        base = fname.split('\\')[-1].split('/')[-1]
        print(f"\n  {base}:")
        for r in p.Rows:
            rid = int(r.ID)
            if rid not in target_ids:
                continue
            findings = []
            for c in r.Cells:
                nm = str(c.Def.InternalName)
                try:
                    v = int(c.Value)
                except Exception:
                    continue
                if v in (0, -1):
                    continue
                low = nm.lower()
                if ('item' in low or 'lot' in low or 'reward' in low or
                    'soul' in low or 'flag' in low or 'event' in low or
                    'getitem' in low):
                    findings.append((nm, v))
            if findings:
                print(f"    row {rid}:")
                for nm, v in findings:
                    print(f"      {nm} = {v}")
