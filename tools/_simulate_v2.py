#!/usr/bin/env python3
"""Refine the template-fix simulation:
   - get an accurate count of MASSEDIT rows that would disappear after dedup
   - break down by template (which template causes the most loss)
   - list concrete examples from each affected category
"""
import sys, io, struct, json, os, tempfile
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
from pathlib import Path
from collections import defaultdict
import config
from pythonnet import load; load('coreclr')
from System.Reflection import Assembly
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
import SoulsFormats
_str = SysType.GetType('System.String')
_emcls = asm.GetType('SoulsFormats.EMEVD')
_emr = _emcls.BaseType.GetMethod('Read', Array[SysType]([_str]))

DATA_DIR = config.TOOLS_DIR.parent / 'data'

# Load items_database
db = json.load(open(DATA_DIR/'items_database.json', encoding='utf-8'))
emevd_db = [r for r in db if r.get('source')=='emevd']
print(f"items_database emevd-source records: {len(emevd_db)}")

# Load MSB entity index to map partName → entity_id
msb_idx = json.load(open(DATA_DIR/'msb_entity_index.json', encoding='utf-8'))
# Build reverse index: (map, name) → entity_id
name_to_entity = {}
for eid_str, info in msb_idx.items():
    if info.get('kind') == 'enemy':
        name_to_entity[(info['map'], info['name'])] = int(eid_str)

# Re-scan emevd: find every InitializeEvent of NON_AWARDER templates and record (entity, lot, template, map)
OLD_TEMPLATES = {
    90005200: (8, 16, 20),  90005210: (8, 16, 20),
    90005300: (8, 16, 20),  90005301: (8, 16, 20),
    90005860: (16, 24, 28), 90005861: (16, 24, 28),
    90005880: (16, 24, 28), 90005881: (16, 24, 28),
    90005882: (16, 24, 28), 90005883: (16, 24, 28),
    90005885: (16, 24, 28),
    90005750: (8, 16, 20), 90005774: (8, 12, 16), 90005792: (8, 24, 28),
    90005632: (8, 16, 20), 90005110: (8, 20, 24), 90005390: (8, 28, 32),
    90005555: (8, 12, 16),
}
NON_AWARDERS = {90005200, 90005210, 90005881, 90005882, 90005883, 90005885}
AWARDERS = set(OLD_TEMPLATES.keys()) - NON_AWARDERS

EVENT_DIR = config.ERR_MOD_DIR / 'event'
phantom_entity_lots = defaultdict(set)  # template_eid -> {(entity, lot), ...}
real_entity_lots = set()
for path in sorted(EVENT_DIR.glob('*.emevd.dcx')):
    em = _emr.Invoke(None, Array[Object]([str(path)]))
    for ev in em.Events:
        for ins in ev.Instructions:
            if int(ins.Bank)!=2000 or int(ins.ID)!=6: continue
            b = bytes(ins.ArgData) if ins.ArgData else b''
            if len(b)<8: continue
            eid = struct.unpack_from('<i', b, 4)[0]
            if eid not in OLD_TEMPLATES: continue
            ent_o, lot_o, min_a = OLD_TEMPLATES[eid]
            if len(b) < min_a: continue
            ent = struct.unpack_from('<i', b, ent_o)[0]
            lot = struct.unpack_from('<i', b, lot_o)[0]
            if ent <= 0 or lot <= 0: continue
            pair = (ent, lot)
            if eid in NON_AWARDERS:
                phantom_entity_lots[eid].add(pair)
            else:
                real_entity_lots.add(pair)

# Build flat set of pairs ONLY from non-awarders
all_phantom = set()
for s in phantom_entity_lots.values():
    all_phantom |= s
truly_phantom = all_phantom - real_entity_lots
print(f"\nTemplate breakdown of phantom (entity, lot) pairs:")
for eid in sorted(phantom_entity_lots.keys()):
    pairs = phantom_entity_lots[eid]
    truly = pairs - real_entity_lots
    print(f"  {eid}: {len(pairs):4d} total, {len(truly):4d} truly phantom (no overlap with awarders)")
print(f"  TOTAL truly phantom: {len(truly_phantom)} pairs")

# Match items_database records to their originating (entity, lot) using partName + map
# Each record has partName + map -> entity_id via msb_idx
phantom_records = []
real_records = []
unmatched = 0
for r in emevd_db:
    key = (r['map'], r['partName'])
    eid = name_to_entity.get(key)
    if eid is None:
        unmatched += 1
        continue
    # Record's base lot = itemLotId for base records; for sub-lot records, the
    # base lot is in some prior record. Approximate: check if (entity, itemLotId) OR
    # (entity, any preceding lot that maps to this sub) is in truly_phantom.
    # Simpler heuristic: walk back up to 20 IDs from itemLotId to find base.
    matched_phantom = False
    matched_real = False
    for back in range(20):
        candidate = r['itemLotId'] - back
        if (eid, candidate) in truly_phantom:
            matched_phantom = True; break
        if (eid, candidate) in real_entity_lots:
            matched_real = True; break
    if matched_phantom and not matched_real:
        phantom_records.append(r)
    else:
        real_records.append(r)

print(f"\nitems_database emevd records:")
print(f"  matched to phantom: {len(phantom_records)}")
print(f"  matched to awarder (real): {len(real_records)}")
print(f"  unmatched (partName→entity lookup failed): {unmatched}")

# Categorize phantom records using generate_loot_massedit's filters (simplified)
LHUTEL = 358000
PATE = {2200,2201,2202,2203,2204,2205,2206,2207,2002150}
SS_LOW = {10100,10101,10102,10103,10104,10105,10160,10161,10162,10163,10164,10165}
SS_HIGH = {10106,10107,10166,10167,10200,10150,10151}
SS_RARE = {10140,10168}
GOLDEN_LOW = {2900,2901,2902,2903,2904,2905,2906,2907,2002951,2002952,2002953}
GOLDEN_HIGH = {2908,2909,2910,2911,2912,2913,2914,2915,2916,2917,2918,2919,
               2002954,2002955,2002956,2002957,2002958,2002959,2002960}

def cat(items):
    def has_id(s):  return any(i['id'] in s and i['category']==1 for i in items)
    def has_name(s): return any(s in i.get('name','').lower() for i in items)
    if has_id(SS_LOW): return 'Loot - Smithing Stones (Low)'
    if has_id(SS_HIGH): return 'Loot - Smithing Stones'
    if has_id(SS_RARE): return 'Loot - Smithing Stones (Rare)'
    if has_id({150}): return 'Loot - Rune Arcs'
    if has_id({10060}): return 'Loot - Dragon Hearts'
    if has_id({10070}): return 'Key - Lost Ashes'
    if has_id({800010}): return 'Reforged - Rune Pieces'
    if has_id({800011}): return 'Reforged - Items'
    if has_id(PATE): return 'Loot - Prattling Pates'
    if has_id(GOLDEN_LOW): return 'Loot - Golden Runes (Low)'
    if has_id(GOLDEN_HIGH): return 'Loot - Golden Runes'
    if any(i['category']==1 and 300000<=i['id']<=399999 and i['id']!=LHUTEL for i in items):
        return 'Equipment - Spirits'
    if any(i['id']==LHUTEL for i in items): return '[FILTERED Lhutel]'
    if has_name('bell bearing'): return 'Loot - Bell-Bearings'
    if has_name('cookbook'): return 'Key - Cookbooks'
    if any(i['category']==2 and i['id']<50000000 for i in items): return 'Equipment - Armaments'
    if any(i['category']==2 for i in items): return 'Loot - Ammo'
    if any(i['category']==3 for i in items): return 'Equipment - Armour'
    if any(i['category']==4 for i in items): return 'Equipment - Talismans'
    if any(i['category']==5 for i in items): return 'Equipment - Ashes of War'
    return f"<uncategorized: {[(i['id'], i.get('name','?')) for i in items]}>"

# Group phantom records by category, simulate generate_loot dedup
by_cat_records = defaultdict(list)
for r in phantom_records:
    c = cat(r['items'])
    by_cat_records[c].append(r)

def dedup(records):
    seen = set(); out = []
    for r in records:
        primary_id = r['items'][0]['id'] if r['items'] else 0
        k = (primary_id, round(r['x'],1), round(r['y'],1), round(r['z'],1))
        if k in seen: continue
        seen.add(k); out.append(r)
    return out

print(f"\n=== Disappearing records by category (after dedup, matches MASSEDIT row count) ===")
for cat_name in sorted(by_cat_records, key=lambda c: -len(by_cat_records[c])):
    records = by_cat_records[cat_name]
    deduped = dedup(records)
    sample_items = set()
    for r in records[:50]:
        for it in r['items']: sample_items.add((it['id'], it.get('name','?')))
    sample = sorted(sample_items)[:5]
    maps = defaultdict(int)
    for r in records: maps[r['map'][:7]] += 1
    top_maps = sorted(maps.items(), key=lambda x:-x[1])[:5]
    print(f"\n  {cat_name}")
    print(f"    {len(records)} records → after dedup: {len(deduped)} MASSEDIT rows")
    print(f"    unique items in sample: {sample}")
    print(f"    top maps: {top_maps}")

# Output comparison to actual MASSEDIT row counts for affected categories
print(f"\n=== Loss vs current MASSEDIT row count ===")
MASSEDIT_DIR = DATA_DIR / 'massedit_generated'
for cat_name in by_cat_records:
    if cat_name.startswith('['): continue
    fname = MASSEDIT_DIR / f'{cat_name}.MASSEDIT'
    if not fname.exists(): continue
    import re
    text = fname.read_text(encoding='utf-8', errors='ignore')
    n_rows = len(set(re.findall(r'id (\d+):', text)))
    deduped = dedup(by_cat_records[cat_name])
    print(f"  {cat_name}: current {n_rows} rows  →  -{len(deduped)} phantom  →  expected {n_rows - len(deduped)}")
