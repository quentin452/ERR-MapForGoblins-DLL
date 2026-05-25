"""Search ALL params in mod and vanilla for any cell value == 358000 (mod Lhutel id)
or 258000 (vanilla Lhutel id) or 20000 (the lot row id)."""
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

SKIP_PARAMS = {'AtkParam', 'BehaviorParam', 'CharaInitParam', 'AnimParam',
               'TutorialBody', 'BulletParam', 'AssetEnvironmentGeometryParam',
               'WeaponParam', 'FaceParam', 'CalcCorrect', 'MagicParam',
               'EquipMtrlSetParam', 'EquipParam', 'ProtectorParam'}

for tag, reg in [('MOD', config.ERR_MOD_DIR / 'regulation.bin')]:
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(reg))
    print(f"\n========= {tag} =========")
    for f in bnd.Files:
        fn = str(f.Name)
        if not fn.lower().endswith('.param'):
            continue
        base = fn.split('\\')[-1].split('/')[-1].replace('.param', '')
        if any(skip in base for skip in SKIP_PARAMS):
            continue
        tmp = os.path.join(tempfile.gettempdir(), '_p.param')
        SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
        try:
            p = _pr.Invoke(None, Array[Object]([tmp]))
        except Exception:
            continue
        pdef = defs.get(str(p.ParamType))
        if pdef:
            try:
                p.ApplyParamdef(pdef)
            except Exception:
                pass
        # Walk every cell; flag any field whose name has goods/lot/item context and value matches
        hits = []
        for r in p.Rows:
            rid = int(r.ID)
            for c in r.Cells:
                try:
                    v = int(c.Value)
                except Exception:
                    continue
                if v in (358000, 20000):
                    nm = str(c.Def.InternalName)
                    low = nm.lower()
                    # Skip animation/atk fields
                    if any(k in low for k in ('anim', 'atk', 'sfx', 'bullet', 'behavior',
                                              'sound', 'voice', 'particle', 'speffect',
                                              'mesh', 'lod', 'msg', 'tex', 'mat')):
                        continue
                    hits.append((rid, nm, v))
        if hits:
            print(f"\n  {base}:")
            for rid, nm, v in hits[:30]:
                print(f"    row {rid}.{nm} = {v}")
            if len(hits) > 30:
                print(f"    ... and {len(hits)-30} more")
