"""Build authoritative per-tile region mapping.

Source chain (all from game data, no heuristics):
  1. BonfireWarpParam      -> tile (areaNo, gridXNo, gridZNo)
                            -> bonfireSubCategoryId
  2. BonfireWarpSubCategoryParam[id]
                            -> row_id is itself a PlaceName ID (sub-region)
                            -> tabId references BonfireWarpTabParam (major region row id, also a PlaceName ID)
  3. PlaceName FMG (merged base + dlc01 + dlc02) -> text

For tiles without any grace (rare), inherit from nearest tile in the same `area` with grace data (Manhattan radius up to 3).

Output:
  data/tile_region_map.json  — { "m60_XX_YY": {region, sub_region, subCategoryId, tabId, source, ...} }
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

bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(config.ERR_MOD_DIR/'regulation.bin'))

DATA_DIR = config.TOOLS_DIR.parent / 'data'
place_names = {int(k): re.sub(r'<[^>]+>', '', v)
               for k, v in json.load(open(DATA_DIR/'PlaceName_engus_merged.json', encoding='utf-8')).items()}

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

# Build subcat -> {tabId, sub_region_name, major_region_name}
subcat = load_param('BonfireWarpSubCategoryParam')
sub_map = {}
for r in subcat.Rows:
    rid = int(r.ID)
    tab_id = None
    for c in r.Cells:
        if str(c.Def.InternalName) == 'tabId':
            try: tab_id = int(c.Value)
            except: pass
    sub_name = place_names.get(rid)
    tab_name = place_names.get(tab_id) if tab_id is not None else None
    sub_map[rid] = {
        'subCategoryId': rid, 'tabId': tab_id,
        'subRegion': sub_name,
        'majorRegion': tab_name,
    }

# Build tile -> {subcat_counts}
bonfire = load_param('BonfireWarpParam')
tile_subcat = defaultdict(Counter)
for r in bonfire.Rows:
    area = gx = gz = scid = None
    for c in r.Cells:
        nm = str(c.Def.InternalName)
        try: v = int(c.Value)
        except: continue
        if   nm == 'areaNo': area = v
        elif nm == 'gridXNo': gx = v
        elif nm == 'gridZNo': gz = v
        elif nm == 'bonfireSubCategoryId': scid = v
    if area is None or area < 0 or scid is None: continue
    tile_subcat[(area, gx, gz)][scid] += 1

# Pick dominant subcat per tile
tile_region = {}
for key, counts in tile_subcat.items():
    top, n = counts.most_common(1)[0]
    info = sub_map.get(top, {})
    total = sum(counts.values())
    tile_region[key] = {
        'subCategoryId': top,
        'tabId': info.get('tabId'),
        'subRegion': info.get('subRegion'),
        'majorRegion': info.get('majorRegion'),
        'graceCount': total,
        'dominance': round(n / total, 3) if total else 0,
        'source': 'BonfireWarpParam (authoritative)',
    }

# For tiles in m60/m61 without graces: inherit from nearest neighbor in same area
inherited = 0
for area in (60, 61):
    direct_tiles = {(gx, gz) for (a, gx, gz) in tile_region if a == area}
    if not direct_tiles: continue
    # Compute bounding box of existing tiles in this area
    gx_min = min(gx for gx, gz in direct_tiles)
    gx_max = max(gx for gx, gz in direct_tiles)
    gz_min = min(gz for gx, gz in direct_tiles)
    gz_max = max(gz for gx, gz in direct_tiles)
    # For every (gx, gz) in box that's missing, find nearest with grace data
    for gx in range(gx_min, gx_max + 1):
        for gz in range(gz_min, gz_max + 1):
            key = (area, gx, gz)
            if key in tile_region: continue
            # Find nearest tile (Manhattan distance, max radius 4)
            best = None; best_d = 99
            for d in range(1, 5):
                cands = []
                for ngx, ngz in direct_tiles:
                    md = abs(ngx - gx) + abs(ngz - gz)
                    if md == d: cands.append((ngx, ngz))
                if cands:
                    # Pick the one with most graces
                    cands.sort(key=lambda c: -tile_region[(area, c[0], c[1])]['graceCount'])
                    best = cands[0]; best_d = d
                    break
            if best:
                nkey = (area, best[0], best[1])
                src_info = tile_region[nkey]
                tile_region[key] = {
                    'subCategoryId': src_info['subCategoryId'],
                    'tabId': src_info['tabId'],
                    'subRegion': src_info['subRegion'],
                    'majorRegion': src_info['majorRegion'],
                    'graceCount': 0,
                    'dominance': None,
                    'source': f"inherited from m{area:02d}_{best[0]:02d}_{best[1]:02d} (manhattan d={best_d})",
                }
                inherited += 1

# Save
out = {}
for (a, gx, gz), info in tile_region.items():
    out[f"m{a:02d}_{gx:02d}_{gz:02d}"] = info
with open(DATA_DIR/'tile_region_map.json', 'w', encoding='utf-8') as f:
    json.dump(out, f, indent=2, ensure_ascii=False)

print(f"Tile region map: {len(tile_region)} tiles total")
print(f"  Direct (from BonfireWarpParam): {sum(1 for v in tile_region.values() if v['source'].startswith('BonfireWarpParam'))}")
print(f"  Inherited from neighbors:       {inherited}")

# Verify user's tiles
print("\n=== Verification ===")
for (a, gx, gz, expected) in [
    (60, 41, 32, 'Weeping Peninsula'),
    (60, 43, 33, 'Weeping Peninsula'),
    (60, 35, 53, 'Mt. Gelmir'),
    (60, 35, 50, 'Liurnia (Caria Manor)'),
    (60, 35, 49, '?'),  # the user's beacon example tile
    (60, 47, 51, '?'),
    (60, 39, 46, 'Limgrave'),
    (60, 41, 46, 'Liurnia'),
    (60, 43, 32, '?'),
]:
    key = (a, gx, gz)
    info = tile_region.get(key)
    label = f"m{a:02d}_{gx:02d}_{gz:02d}"
    if not info:
        print(f"  {label}: STILL no data (expected {expected})")
        continue
    sub = info['subRegion'] or f"<id {info['subCategoryId']}>"
    maj = info['majorRegion'] or '?'
    src = '(direct)' if info['source'].startswith('Bonfire') else f"({info['source']})"
    print(f"  {label}: {maj} / {sub}  {src}  [expected: {expected}]")
