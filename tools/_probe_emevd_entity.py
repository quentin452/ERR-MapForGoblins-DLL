#!/usr/bin/env python3
"""Resolve EMEVD award entity ids → their MSB part (Enemy or Asset), tier, tile.

Tells whether a direct-award entity is found by the runtime (Enemy in a _00 tile) or only
by some other scope (Asset / non-_00 tier / GED-disabled) → explains an
entity-not-an-msb-enemy miss in build_disk_emevd_markers.

Usage: python _probe_emevd_entity.py [eid1 eid2 ...]  (defaults = the 15-emevd base entities)
"""
import sys, os, tempfile
import extract_all_items as E
import config
from pathlib import Path

DEFAULT = [1148560200, 1036490300, 2045460200, 2048400200, 2050460300, 2050460310,
           1254560800, 1054560800]  # last two = the 30510 boss-reward (cross-tile)
targets = set(int(a) for a in sys.argv[1:]) or set(DEFAULT)

MSB_DIR = Path(config.require_err_mod_dir()) / 'map' / 'MapStudio'
found = {t: [] for t in targets}
for msb_path in sorted(MSB_DIR.glob('*.msb.dcx')):
    info = E.parse_map_name(msb_path.name)
    if not info or info['p3'] == 99:
        continue
    stem = msb_path.name.replace('.msb.dcx', '')
    tier = stem.split('_')[-1]  # _00 / _01 / _10 ...
    try:
        msb = E._read_from_bytes(E._msbe_read, E.SoulsFormats.DCX.Decompress(str(msb_path)), '.msb')
    except Exception:
        continue
    for kind, coll in (('Enemy', msb.Parts.Enemies), ('Asset', msb.Parts.Assets)):
        for p in coll:
            try:
                eid = int(p.EntityID)
            except Exception:
                continue
            if eid in targets:
                ged = int(getattr(p, 'GameEditionDisable', 0) or 0)
                found[eid].append(dict(tile=stem, tier=tier, kind=kind, name=str(p.Name),
                                       model=str(p.ModelName), ged=ged))

for eid in sorted(targets):
    fs = found[eid]
    if not fs:
        print(f"ENTITY {eid}: *** NOT FOUND in any MSB part ***")
        continue
    has_00_enemy = any(f['tier'] == '00' and f['kind'] == 'Enemy' and f['ged'] == 0 for f in fs)
    print(f"ENTITY {eid}: {len(fs)} part(s)  runtime-visible(_00 Enemy,GED0)={'YES' if has_00_enemy else 'NO'}")
    for f in fs:
        print(f"   {f['tile']:<16} {f['kind']:<6} ged={f['ged']} {f['model']:<14} {f['name']}")
