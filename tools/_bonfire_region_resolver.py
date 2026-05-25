"""Verify: BonfireWarpParam.bonfireSubCategoryId == PlaceName FMG region id.
Build per-tile region map from BonfireWarpParam.
"""
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

bonfire = load_param('BonfireWarpParam')
subcat  = load_param('BonfireWarpSubCategoryParam')
tab     = load_param('BonfireWarpTabParam')

# Read subcat rows: row_id -> textId (and what it resolves to)
subcat_text = {}
subcat_tab = {}
for r in subcat.Rows:
    rid = int(r.ID)
    tid = None; tabid = None
    for c in r.Cells:
        nm = str(c.Def.InternalName)
        try: v = int(c.Value)
        except: continue
        if nm == 'textId': tid = v
        elif nm == 'tabId': tabid = v
    subcat_text[rid] = tid
    subcat_tab[rid] = tabid

print("=== Sample SubCategory rows ===")
for rid in [61000, 61002, 62000, 63000, 63001, 64000, 65000, 6800, 10000, 11000, 11050]:
    tid = subcat_text.get(rid)
    tab_id = subcat_tab.get(rid)
    pn_row = place_names.get(rid, '?')
    pn_text = place_names.get(tid, '?') if tid else '?'
    print(f"  subcat row {rid}: textId={tid} ('{pn_text}'); tabId={tab_id}; "
          f"PlaceName[{rid}]='{pn_row}'")

# Now walk BonfireWarpParam and build tile -> region map
print("\n=== Per-tile region from grace subcategoryId ===")
tile_subcat = defaultdict(Counter)
for r in bonfire.Rows:
    area = gx = gz = scid = None
    for c in r.Cells:
        nm = str(c.Def.InternalName)
        try: v = int(c.Value)
        except: continue
        if nm == 'areaNo': area = v
        elif nm == 'gridXNo': gx = v
        elif nm == 'gridZNo': gz = v
        elif nm == 'bonfireSubCategoryId': scid = v
    if area is None or area < 0 or scid is None: continue
    tile_subcat[(area, gx, gz)][scid] += 1

# Verify user-mentioned tiles
print("\nVerification:")
for (area, gx, gz, expected) in [
    (60, 41, 32, 'Weeping Peninsula'),
    (60, 43, 33, 'Weeping Peninsula'),
    (60, 35, 53, 'Mt. Gelmir'),
    (60, 35, 50, 'Caelid'),
    (60, 47, 51, 'Caelid'),
    (60, 37, 52, 'Mt. Gelmir'),
    (60, 39, 46, 'Limgrave'),
    (60, 41, 46, 'Liurnia'),
]:
    key = (area, gx, gz)
    counts = tile_subcat.get(key)
    if not counts:
        print(f"  m{area:02d}_{gx:02d}_{gz:02d}: expected={expected!r}; NO graces in this tile")
        continue
    parts = []
    for scid, n in counts.most_common():
        nm = place_names.get(scid, f'<id {scid}>')
        parts.append(f"{n}× {nm!r}")
    print(f"  m{area:02d}_{gx:02d}_{gz:02d}: expected={expected!r}  found={', '.join(parts)}")

# Coverage stats
m60_tiles = {(a,gx,gz) for (a,gx,gz) in tile_subcat if a == 60}
m61_tiles = {(a,gx,gz) for (a,gx,gz) in tile_subcat if a == 61}
print(f"\nCoverage: {len(m60_tiles)} m60 tiles, {len(m61_tiles)} m61 tiles have graces")

# Save final mapping
out = {}
for key, counts in tile_subcat.items():
    top, n = counts.most_common(1)[0]
    out[f"m{key[0]:02d}_{key[1]:02d}_{key[2]:02d}"] = {
        'subCategoryId': top, 'region': place_names.get(top, ''),
        'graceCount': sum(counts.values()), 'topDominance': n / sum(counts.values()),
    }
out_path = DATA_DIR / 'tile_region_map.json'
with open(out_path, 'w', encoding='utf-8') as f:
    json.dump(out, f, indent=2, ensure_ascii=False)
print(f"Saved {len(out)} tiles to {out_path}")
