#!/usr/bin/env python3
"""T3 diagnostic: is the WorldMapPointParam field-boss row (textId2==5100) in
OVERWORLD frame, while the MSB entity pos we currently bake is in DUNGEON frame?

Joins the already-baked data/boss_list.json (entity frame) against the live WMP
5100 rows (row frame) by clearedEventFlagId, and prints both frames side by side.
If dungeon bosses (baked entity area NOT in 60/61) carry a WMP row whose areaNo
IS 60/61 with an overworld grid+pos, the row is authored in overworld frame and
T3 holds: bake the row pos instead of the entity pos -> zero-RE drift fix.
"""
import json
import sys
import io

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

import config
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly
Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats  # noqa: F401

ERR_MOD_DIR = config.require_err_mod_dir()
bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(ERR_MOD_DIR / 'regulation.bin'))
from extract_all_items import load_paramdefs, read_param, param_to_dict
paramdefs = load_paramdefs()

# 5100 field-boss rows, indexed by clearedEventFlagId (the join key).
wmp = read_param(bnd, 'WorldMapPointParam', paramdefs)
fields = {'clearedEventFlagId', 'textId1', 'textId2', 'textId4',
          'areaNo', 'gridXNo', 'gridZNo', 'posX', 'posY', 'posZ'}
wmp_data = param_to_dict(wmp, fields)
by_flag = {}
for rid, row in wmp_data.items():
    if row.get('textId2') != 5100:
        continue
    f = int(row.get('clearedEventFlagId', 0) or 0)
    if f > 0:
        by_flag.setdefault(f, (rid, row))

boss_list = json.load(open(config.DATA_DIR / 'boss_list.json', encoding='utf-8'))

print(f"WMP 5100 rows w/ flag: {len(by_flag)}  |  boss_list entries: {len(boss_list)}\n")

OVERWORLD = (60, 61)
hdr = (f"{'name':28} {'entity(area,gx,gz)':18} {'entity(x,z)':>18}  ||  "
       f"{'wmp(area,gx,gz)':16} {'wmp(x,z)':>18}  frame")
print(hdr)
print('-' * len(hdr))

n_dungeon = n_dungeon_overworld_row = n_no_row = 0
for b in boss_list:
    flag = int(b.get('clearedEventFlagId', 0) or 0)
    name = (b.get('vanillaPlaceName') or '?')[:27]
    e_area, e_gx, e_gz = b['areaNo'], b['gridX'], b['gridZ']
    e_x, e_z = b['x'], b['z']
    is_dungeon = e_area not in OVERWORLD
    rec = by_flag.get(flag)
    if rec is None:
        if is_dungeon:
            n_no_row += 1
        ent = f"{e_area},{e_gx},{e_gz}"
        print(f"{name:28} {ent:18} {f'{e_x:.1f},{e_z:.1f}':>18}  ||  "
              f"{'(no 5100 row)':16} {'':>18}  {'DUNGEON' if is_dungeon else 'overworld'}")
        continue
    rid, row = rec
    w_area = int(row.get('areaNo', 0) or 0)
    w_gx = int(row.get('gridXNo', 0) or 0)
    w_gz = int(row.get('gridZNo', 0) or 0)
    w_x = float(row.get('posX', 0) or 0)
    w_z = float(row.get('posZ', 0) or 0)
    row_overworld = w_area in OVERWORLD
    if is_dungeon:
        n_dungeon += 1
        if row_overworld:
            n_dungeon_overworld_row += 1
    frame = ('DUNGEON-entity / overworld-row' if is_dungeon and row_overworld else
             'DUNGEON both' if is_dungeon else
             'overworld both')
    ent = f"{e_area},{e_gx},{e_gz}"
    wmp_s = f"{w_area},{w_gx},{w_gz}"
    print(f"{name:28} {ent:18} {f'{e_x:.1f},{e_z:.1f}':>18}  ||  "
          f"{wmp_s:16} {f'{w_x:.1f},{w_z:.1f}':>18}  {frame}")

print()
print(f"dungeon bosses (entity area not 60/61): {n_dungeon}")
print(f"  ... whose 5100 WMP row IS overworld (60/61): {n_dungeon_overworld_row}")
print(f"  ... dungeon bosses with NO 5100 row at all: {n_no_row}")
print()
if n_dungeon and n_dungeon_overworld_row == n_dungeon:
    print("T3 VERDICT: HOLDS — every dungeon boss's 5100 row is in overworld frame.")
    print("            Bake the WMP row (areaNo,gridXNo,gridZNo,posX,posZ) -> zero-RE fix.")
elif n_dungeon_overworld_row:
    print(f"T3 VERDICT: PARTIAL — {n_dungeon_overworld_row}/{n_dungeon} dungeon rows are overworld.")
else:
    print("T3 VERDICT: FAILS — dungeon 5100 rows are NOT in overworld frame (or absent).")
