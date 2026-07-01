#!/usr/bin/env python3
"""RE probe (Group 2, Portal/waygate): identify the AEG asset model(s) behind MapGenie 'Portal'.

Two passes over the active install's MSBs (SoulsFormats via pythonnet, same as generate_stakes.py):
 (1) enumerate every AEG asset ModelName with its placement count + area spread (find candidates);
 (2) anchor: for each WorldMapPointParam row whose PlaceName == 'Sending Gate', find the nearest
     AEG asset in that map tile → the model that IS the sending gate.

Usage: py _probe_portal_aeg.py
"""
import sys, io, os, json, tempfile, collections, math
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats

ERR_MOD_DIR = config.require_err_mod_dir()
MSB_DIR = ERR_MOD_DIR / 'map' / 'MapStudio'
_str_type = SysType.GetType('System.String')
_msbe_read = asm.GetType('SoulsFormats.MSBE').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)

def rfb(rm, data, suf='.bin'):
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_mfg_portal' + suf)
    SysFile.WriteAllBytes(tmp, data.ToArray() if hasattr(data, 'ToArray') else data)
    r = rm.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    return r

# ---- anchor targets: WMPP rows named 'Sending Gate' / 'Portal' ----
DATA = config.DATA_DIR
wmpp = json.load(open(DATA / 'WorldMapPointParam.json', encoding='utf-8'))
place = {int(k): v for k, v in json.load(open(DATA / 'PlaceName_engus.json', encoding='utf-8')).items()}
def ai(v, d=0):
    try: return int(str(v))
    except Exception: return d
def rowname(r):
    for i in range(1, 9):
        t = ai(r.get(f'textId{i}', -1), -1)
        if t > 0 and t in place and place[t] not in ('', '[ERROR]') and '<' not in place[t]:
            return place[t]
    return None
anchors = []  # (area,gx,gz,x,z,name)
for r in wmpp:
    nm = rowname(r)
    if nm and ('Sending Gate' in nm or nm == 'Portal' or 'Waygate' in nm):
        anchors.append((ai(r.get('areaNo')), ai(r.get('gridXNo')), ai(r.get('gridZNo')),
                        float(r.get('posX', 0) or 0), float(r.get('posZ', 0) or 0), nm))
print(f"# WMPP anchor rows (Sending Gate/Portal): {len(anchors)}")
for a in anchors:
    print("   anchor", a)

# ---- scan MSBs ----
model_count = collections.Counter()
model_areas = collections.defaultdict(set)
# per-tile asset list for the anchor nearest-model join
tile_assets = collections.defaultdict(list)  # (area,gx,gz) -> [(model, x, z)]
anchor_tiles = {(a[0], a[1], a[2]) for a in anchors}

for msb_path in sorted(MSB_DIR.glob('*.msb.dcx')):
    name = msb_path.name.replace('.msb.dcx', '')
    parts = name.split('_')
    if len(parts) < 4:
        continue
    area, gx, gz = int(parts[0][1:]), int(parts[1]), int(parts[2])
    try:
        msb = rfb(_msbe_read, SoulsFormats.DCX.Decompress(str(msb_path)), '.msb')
    except Exception:
        continue
    for p in msb.Parts.Assets:
        if int(getattr(p, 'GameEditionDisable', 0) or 0) == 1:
            continue
        m = str(p.ModelName)
        model_count[m] += 1
        model_areas[m].add(area)
        if (area, gx, gz) in anchor_tiles:
            tile_assets[(area, gx, gz)].append((m, float(p.Position.X), float(p.Position.Z)))

# ---- pass 2: nearest model to each anchor ----
print("\n# nearest AEG asset to each 'Sending Gate' anchor:")
gate_models = collections.Counter()
for (area, gx, gz, x, z, nm) in anchors:
    cands = tile_assets.get((area, gx, gz), [])
    if not cands:
        print(f"   {nm} @ m{area:02d}_{gx:02d}_{gz:02d} ({x:.1f},{z:.1f}) -> NO assets in tile")
        continue
    best = min(cands, key=lambda c: (c[1]-x)**2 + (c[2]-z)**2)
    d = math.hypot(best[1]-x, best[2]-z)
    gate_models[best[0]] += 1
    print(f"   {nm} @ m{area:02d}_{gx:02d}_{gz:02d} -> {best[0]} (dist {d:.1f})")

# ---- report candidate models ----
print("\n# models nearest the anchors (candidate portal models):")
for m, c in gate_models.most_common():
    print(f"   {m}: anchored {c}x | total placements {model_count[m]} | areas {sorted(model_areas[m])}")

# heuristic candidate list: models whose name hints a gate/warp, or with a plausible count
print("\n# AEG models with 'gate'/'warp'/'portal'-ish counts (10..80 placements), for eyeballing:")
for m, c in sorted(model_count.items(), key=lambda kv: -kv[1]):
    if 5 <= c <= 90:
        print(f"   {m}: {c} placements | areas {sorted(model_areas[m])[:8]}")
