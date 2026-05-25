"""Build tile -> region mapping from BonfireWarpParam.

Each grace (BonfireWarpParam row) has:
  - areaNo, gridXNo, gridZNo  -> the map tile it lives in
  - textId1                   -> PlaceName FMG entry (e.g. 'Limgrave - Weeping Peninsula - Tombsward Catacombs')

For each (area, gridX, gridZ) we collect all textId1 values of graces there,
resolve them to PlaceName text, and pick the dominant region label.
Authoritative source: regulation.bin -> BonfireWarpParam + PlaceName_engus.json.
"""
import sys
import io
import os
import re
import json
import tempfile
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

for f in bnd.Files:
    if 'BonfireWarpParam' not in str(f.Name):
        continue
    tmp = os.path.join(tempfile.gettempdir(), '_b.param')
    SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
    bonfire = _pr.Invoke(None, Array[Object]([tmp]))
    pdef = defs.get(str(bonfire.ParamType))
    if pdef:
        bonfire.ApplyParamdef(pdef)
    break

DATA_DIR = config.TOOLS_DIR.parent / 'data'
place_names = {int(k): re.sub(r'<[^>]+>', '', v)
               for k, v in json.load(open(DATA_DIR / 'PlaceName_engus.json', encoding='utf-8')).items()}

tile_to_text = defaultdict(Counter)
sample_full_text = {}

for r in bonfire.Rows:
    vals = {}
    for c in r.Cells:
        nm = str(c.Def.InternalName)
        try:
            vals[nm] = int(c.Value)
        except Exception:
            vals[nm] = c.Value
    area = vals.get('areaNo', -1)
    gx = vals.get('gridXNo', -1)
    gz = vals.get('gridZNo', -1)
    if area < 0:
        continue
    # Collect all text IDs (textId1..textId8)
    texts = []
    for i in range(1, 9):
        tid = vals.get(f'textId{i}', -1)
        if tid > 0:
            txt = place_names.get(tid, f'<id {tid}>')
            texts.append((tid, txt))
    key = (area, gx, gz)
    for tid, txt in texts:
        tile_to_text[key][txt] += 1
    if texts and key not in sample_full_text:
        sample_full_text[key] = texts

# Print verification for user's tiles
print("=== Tile -> Bonfire textId1 mapping samples ===")
for (area, gx, gz, expected) in [
    (60, 41, 32, 'Weeping Peninsula'),
    (60, 43, 33, 'Weeping Peninsula'),
    (60, 35, 53, 'Mt. Gelmir'),
    (60, 35, 50, 'Caelid'),
    (60, 47, 51, 'Caelid'),
    (60, 37, 52, 'Mt. Gelmir'),
]:
    key = (area, gx, gz)
    c = tile_to_text.get(key)
    print(f"\n  m{area:02d}_{gx:02d}_{gz:02d}: expected={expected!r}")
    if not c:
        print("    (no graces in this tile in BonfireWarpParam)")
    else:
        for txt, n in c.most_common():
            print(f"    {n}× {txt!r}")

# Show overall counts and distinct text patterns
print(f"\nTotal tiles with graces: {len(tile_to_text)}")
all_texts = Counter()
for c in tile_to_text.values():
    for t, n in c.items():
        all_texts[t] += n
print(f"Distinct text entries: {len(all_texts)}")
print("Most common text entries (top 30):")
for t, n in all_texts.most_common(30):
    print(f"  {n}× {t!r}")
