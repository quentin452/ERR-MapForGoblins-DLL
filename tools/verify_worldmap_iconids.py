#!/usr/bin/env python3
"""Tier 2 verification — WorldMapPointParam.iconId -> landmark category.

Groups every WorldMapPointParam row by iconId and resolves its textId1..8 to
PlaceName text, so each iconId can be read off against the landmark names that
cluster under it (Divine Tower, Evergaol, Portal, ...). Run extract_world_map_param.py
and extract_placename_dump.py first (they write the JSONs this reads).

Usage: MFG_PROFILE=vanilla py verify_worldmap_iconids.py
"""
import sys, io, json, collections
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config

DATA = config.DATA_DIR
wmpp = json.load(open(DATA / 'WorldMapPointParam.json', encoding='utf-8'))
place = json.load(open(DATA / 'PlaceName_engus.json', encoding='utf-8'))
place = {int(k): v for k, v in place.items()}

def as_int(v, d=0):
    try: return int(str(v))
    except Exception: return d

by_icon = collections.defaultdict(lambda: {'n': 0, 'names': collections.Counter(), 'notext': 0})
for r in wmpp:
    icon = as_int(r.get('iconId', 0))
    b = by_icon[icon]
    b['n'] += 1
    got = False
    for i in range(1, 9):
        tid = as_int(r.get(f'textId{i}', -1), -1)
        if tid > 0 and tid in place:
            nm = place[tid]
            if nm and nm != '[ERROR]':
                b['names'][nm] += 1
                got = True
    if not got:
        b['notext'] += 1

print(f'# profile={config.PROFILE}  rows={len(wmpp)}  distinct iconIds={len(by_icon)}\n')
print(f"{'iconId':>6} {'rows':>5} {'notext':>6}  distinct place-names (top 12)")
print('-' * 100)
for icon in sorted(by_icon):
    b = by_icon[icon]
    top = ', '.join(f'{n}×{c}' if c > 1 else n for n, c in b['names'].most_common(12))
    print(f'{icon:>6} {b["n"]:>5} {b["notext"]:>6}  {top}')
