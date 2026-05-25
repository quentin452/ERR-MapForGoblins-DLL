"""Inspect BonfireWarpSubCategoryParam — this should map a subcategory id
(used by BonfireWarpParam.bonfireSubCategoryId) to a region text id."""
import sys, io, os, tempfile, re, json
from collections import defaultdict, Counter
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

DATA_DIR = config.TOOLS_DIR.parent / 'data'
place_names = {int(k): re.sub(r'<[^>]+>', '', v)
               for k, v in json.load(open(DATA_DIR / 'PlaceName_engus.json', encoding='utf-8')).items()}

# Load BonfireWarpSubCategoryParam and BonfireWarpTabParam
def load_param(name):
    for f in bnd.Files:
        if name in str(f.Name):
            tmp = os.path.join(tempfile.gettempdir(), '_p.param')
            SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
            p = _pr.Invoke(None, Array[Object]([tmp]))
            pdef = defs.get(str(p.ParamType))
            if pdef:
                p.ApplyParamdef(pdef)
            return p
    return None

# Inspect structures
for target in ['BonfireWarpSubCategoryParam', 'BonfireWarpTabParam']:
    p = load_param(target)
    if p is None:
        print(f"{target} NOT FOUND")
        continue
    print(f"\n=== {target} (rows={p.Rows.Count}) ===")
    first = p.Rows[0]
    print("Fields:")
    for c in first.Cells:
        nm = str(c.Def.InternalName)
        try:
            v = int(c.Value)
        except Exception:
            v = c.Value
        print(f"  {nm} = {v}")
    print("\nAll rows (id + textId1):")
    for r in p.Rows:
        rid = int(r.ID)
        text_id = None
        tab_id = None
        for c in r.Cells:
            nm = str(c.Def.InternalName)
            try:
                v = int(c.Value)
            except Exception:
                continue
            if nm == 'textId1':
                text_id = v
            elif nm == 'bonfireWarpTabId':
                tab_id = v
        text = place_names.get(text_id, f'<id {text_id}>') if text_id else '?'
        tab_part = f"  tab={tab_id}" if tab_id is not None else ""
        print(f"  row {rid}: textId1={text_id}  text={text!r}{tab_part}")
