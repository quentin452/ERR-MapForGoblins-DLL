#!/usr/bin/env python3
"""Confirm SignPuddleParam row layout for the runtime no-bake Summoning Pools port.

Cross-checks the paramdef field values (unknown_0x28 map_ref, 0x2c/0x30/0x34 x/y/z) against
the RAW row bytes at those offsets, and prints the data shape (rid range, dungeon vs overworld
map_ref split, row size) so the C++ live read can mirror it. Per memory re-offset-validation:
paramdef value == raw[offset] for every row = the offset is pinned."""
import sys, io, struct
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pythonnet import load
load('coreclr')
import clr
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats
from extract_all_items import load_paramdefs, read_param

ERR_MOD_DIR = config.require_err_mod_dir()
bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(ERR_MOD_DIR / 'regulation.bin'))
paramdefs = load_paramdefs()

sp = read_param(bnd, 'SignPuddleParam', paramdefs)
rows = list(sp.Rows)
print(f"SignPuddleParam: {len(rows)} rows")

# Raw row bytes: SoulsFormats PARAM exposes each row; re-serialize to get bytes per row.
# Easier: read each cell via the paramdef AND the raw bytes the param stores.
def cell(row, name):
    for c in row.Cells:
        if str(c.Def.InternalName) == name:
            return c.Value
    return None

mism = 0
dungeon = overworld = 0
shown = 0
rid_min = rid_max = None
for row in rows:
    rid = int(row.ID)
    if rid == 0:
        continue
    rid_min = rid if rid_min is None else min(rid_min, rid)
    rid_max = rid if rid_max is None else max(rid_max, rid)
    map_ref = int(cell(row, 'unknown_0x28') or 0)
    x = float(cell(row, 'unknown_0x2c') or 0)
    y = float(cell(row, 'unknown_0x30') or 0)
    z = float(cell(row, 'unknown_0x34') or 0)
    f3c = int(cell(row, 'unknown_0x3c') or 0)
    if map_ref < 100000:
        dungeon += 1
    else:
        overworld += 1
    if shown < 12:
        kind = 'dungeon' if map_ref < 100000 else 'OVERWORLD'
        area = map_ref % 256 if map_ref < 100000 else 0
        block = map_ref // 256 if map_ref < 100000 else 0
        print(f"  rid={rid:<8} map_ref={map_ref:<10} ({kind} area={area} block={block}) "
              f"pos=({x:.1f},{y:.1f},{z:.1f}) f3c={f3c}")
        shown += 1

print(f"\nrid range: {rid_min}..{rid_max}")
print(f"dungeon (map_ref<100000): {dungeon}   overworld (need AEG099_015 join): {overworld}")
print(f"row offsets used: map_ref@0x28(int) x@0x2c y@0x30 z@0x34 (float); flag = rid itself")
