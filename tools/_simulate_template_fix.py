#!/usr/bin/env python3
"""Simulate what records would disappear from items_database / MASSEDIT if we
remove the 6 non-awarder templates from extract_all_items.py::TEMPLATE_EVENTS.

Method:
  1. Re-scan all emevd InitializeEvent calls and split them into awarder
     vs non-awarder template calls.
  2. For each phantom (entity, fake_lot) pair: walk ItemLotParam_map sub-lots
     (replicating extract_all_items.py logic) to produce the records that
     this pair would create.
  3. Filter to records that are NOT also produced by some real awarder call
     for the same (entity, lot) — those are the ones that would survive.
  4. Categorize each phantom record using generate_loot_massedit.py's
     LOOT_CATEGORIES filters, then count by category.
  5. Look at the actual MASSEDIT_generated/*.MASSEDIT files and count
     how many WorldMapPointParam IDs are sourced from phantom records.
"""
import sys, io, struct, json, os, tempfile, re
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
_pcls = asm.GetType('SoulsFormats.PARAM')
_pr = _pcls.BaseType.GetMethod('Read', Array[SysType]([_str]))

# Template event offsets (entity_off, lot_off, min_args)
OLD_TEMPLATES = {
    90005200: (8, 16, 20),     # WRONG — non-awarder
    90005210: (8, 16, 20),     # WRONG — non-awarder
    90005300: (8, 16, 20),
    90005301: (8, 16, 20),
    90005860: (16, 24, 28),
    90005861: (16, 24, 28),
    90005880: (16, 24, 28),
    90005881: (16, 24, 28),    # WRONG — non-awarder
    90005882: (16, 24, 28),    # WRONG — non-awarder
    90005883: (16, 24, 28),    # WRONG — non-awarder
    90005885: (16, 24, 28),    # WRONG — non-awarder
    90005750: (8, 16, 20),
    90005774: (8, 12, 16),
    90005792: (8, 24, 28),
    90005632: (8, 16, 20),
    90005110: (8, 20, 24),
    90005390: (8, 28, 32),
    90005555: (8, 12, 16),
}
NON_AWARDERS = {90005200, 90005210, 90005881, 90005882, 90005883, 90005885}
AWARDERS = set(OLD_TEMPLATES.keys()) - NON_AWARDERS

# 1) collect all (entity, lot, event_id, map) tuples
EVENT_DIR = config.ERR_MOD_DIR / 'event'
calls_aw = []   # awarder calls
calls_nw = []   # non-awarder calls
for path in sorted(EVENT_DIR.glob('*.emevd.dcx')):
    map_name = path.name.replace('.emevd.dcx', '')
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
            (calls_nw if eid in NON_AWARDERS else calls_aw).append((ent, lot, eid, map_name))

print(f"InitializeEvent calls:")
print(f"  Awarder templates: {len(calls_aw)} calls")
print(f"  Non-awarder (BUGGY) templates: {len(calls_nw)} calls")

# (entity, lot) pairs in each set
set_aw = {(e, l) for e, l, _, _ in calls_aw}
set_nw = {(e, l) for e, l, _, _ in calls_nw}
only_nw = set_nw - set_aw
also_aw = set_nw & set_aw
print(f"  unique (entity, lot) pairs from awarders: {len(set_aw)}")
print(f"  unique (entity, lot) pairs from non-awarders: {len(set_nw)}")
print(f"  pairs ONLY from non-awarders (truly phantom): {len(only_nw)}")
print(f"  pairs in both (would survive thanks to awarder): {len(also_aw)}")

# 2) Load ItemLotParam_map and _enemy
defs = {}
for xml in config.PARAMDEF_DIR.glob('*.xml'):
    try:
        d = SoulsFormats.PARAMDEF.XmlDeserialize(str(xml), False)
        if d and d.ParamType: defs[str(d.ParamType)] = d
    except: pass

bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(config.ERR_MOD_DIR/'regulation.bin'))

def read_lot_param(name):
    for f in bnd.Files:
        if name in str(f.Name):
            tmp = os.path.join(tempfile.gettempdir(), '_lot.param')
            SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
            p = _pr.Invoke(None, Array[Object]([tmp]))
            pdef = defs.get(str(p.ParamType))
            if pdef: p.ApplyParamdef(pdef)
            d = {}
            for r in p.Rows:
                rid = int(r.ID); entry = {}
                for c in r.Cells:
                    nm = str(c.Def.InternalName)
                    if nm.startswith('lotItemId') or nm.startswith('lotItemCategory') or \
                       nm.startswith('lotItemNum') or nm == 'getItemFlagId':
                        try: entry[nm] = int(c.Value)
                        except: entry[nm] = c.Value
                d[rid] = entry
            return d
    return {}

item_lots_map = read_lot_param('ItemLotParam_map')
item_lots_enemy = read_lot_param('ItemLotParam_enemy')
print(f"\nItemLotParam_map rows: {len(item_lots_map)}, ItemLotParam_enemy rows: {len(item_lots_enemy)}")

# 3) Goods names for category filtering
_fcls = asm.GetType('SoulsFormats.FMG')
_fr = _fcls.BaseType.GetMethod('Read', Array[SysType]([_str]))
goods_names = {}
for msg_path in [config.ERR_MOD_DIR/'msg/engus/item_dlc02.msgbnd.dcx']:
    _bcls = asm.GetType('SoulsFormats.BND4')
    _br = _bcls.BaseType.GetMethod('Read', Array[SysType]([_str]))
    bndmsg = _br.Invoke(None, Array[Object]([str(msg_path)]))
    for f in bndmsg.Files:
        if 'GoodsName' in str(f.Name):
            tmp = os.path.join(tempfile.gettempdir(),'_g.fmg')
            SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
            fmg = _fr.Invoke(None, Array[Object]([tmp]))
            for e in fmg.Entries:
                t = str(e.Text) if e.Text else ''
                if t and t != '[ERROR]':
                    goods_names[int(e.ID)] = t

def expand_lot_to_items(lot_id):
    """Replicate extract_all_items.py sub-lot scan logic. Returns list of (sub_lot_id, items, flag)."""
    out = []
    if lot_id in item_lots_enemy:
        for offset in range(1000):
            sub = item_lots_enemy.get(lot_id + offset)
            if sub is None: continue
            items = []
            for slot in range(1,9):
                iid = sub.get(f'lotItemId0{slot}', 0); cat = sub.get(f'lotItemCategory0{slot}', 0)
                num = sub.get(f'lotItemNum0{slot}', 0)
                if iid > 0 and cat > 0:
                    items.append({'id': iid, 'category': cat, 'num': num,
                                  'name': goods_names.get(iid,'?')})
            flag = sub.get('getItemFlagId', 0)
            if items:
                out.append((lot_id + offset, items, flag))
    elif lot_id in item_lots_map:
        # base
        sub = item_lots_map[lot_id]
        items = []
        for slot in range(1,9):
            iid = sub.get(f'lotItemId0{slot}', 0); cat = sub.get(f'lotItemCategory0{slot}', 0)
            num = sub.get(f'lotItemNum0{slot}', 0)
            if iid > 0 and cat > 0:
                items.append({'id': iid, 'category': cat, 'num': num,
                              'name': goods_names.get(iid,'?')})
        flag = sub.get('getItemFlagId', 0)
        if items:
            out.append((lot_id, items, flag))
        for offset in range(1, 20):
            sub = item_lots_map.get(lot_id + offset)
            if sub is None: break
            f = sub.get('getItemFlagId', 0)
            if f <= 0: continue   # extract_all_items only generates sub-records when flag>0
            items = []
            for slot in range(1,9):
                iid = sub.get(f'lotItemId0{slot}', 0); cat = sub.get(f'lotItemCategory0{slot}', 0)
                num = sub.get(f'lotItemNum0{slot}', 0)
                if iid > 0 and cat > 0:
                    items.append({'id': iid, 'category': cat, 'num': num,
                                  'name': goods_names.get(iid,'?')})
            if items:
                out.append((lot_id + offset, items, f))
    return out

# 4) For each non-awarder (entity, lot) pair, expand into records
phantom_records = []  # one entry per (entity, sub_lot_id, items, flag)
phantom_lots_seen = set()
for (entity, lot) in only_nw:
    for sub_lot_id, items, flag in expand_lot_to_items(lot):
        phantom_records.append({'entity': entity, 'base_lot': lot, 'sub_lot': sub_lot_id,
                                'items': items, 'flag': flag})
        phantom_lots_seen.add(sub_lot_id)
print(f"\nPhantom records produced by non-awarder templates: {len(phantom_records)}")

# 5) Categorize by generate_loot_massedit.py's LOOT_CATEGORIES
# Load goods sort groups + crafting/spirit/sorcery/incantation IDs
DATA_DIR = config.PROJECT_DIR / 'data' if False else config.TOOLS_DIR.parent / 'data'

def load_set(p):
    if not p.exists(): return set()
    return set(json.load(open(p)))

CRAFTING = load_set(DATA_DIR / 'goods_crafting_ids.json')
SPIRIT = load_set(DATA_DIR / 'goods_spirit_ash_ids.json')
SORC = load_set(DATA_DIR / 'goods_sorcery_ids.json')
INCAN = load_set(DATA_DIR / 'goods_incantation_ids.json')
SORTS = {}
sp = DATA_DIR / 'goods_sort_groups.json'
if sp.exists():
    SORTS = {int(k): v for k, v in json.load(open(sp)).items()}
CONS_GROUPS = {20, 50, 70, 80}
PATE = {2200,2201,2202,2203,2204,2205,2206,2207,2002150}

def categorize(items):
    """Return category label that matches first, or None. Order matters."""
    def has_id(ids):  return any(i['id'] in ids and i['category']==1 for i in items)
    def has_name(s):  return any(s in i['name'].lower() for i in items)
    def has_cat(c):   return any(i['category']==c for i in items)
    # Smithing Stones (Low)
    if any(i['id'] in {10100,10101,10102,10103,10104,10105,10160,10161,10162,10163,10164,10165}
           and i['category']==1 for i in items):
        return 'Loot - Smithing Stones (Low)'
    if any(i['id'] in {10106,10107,10166,10167,10200,10150,10151} and i['category']==1 for i in items):
        return 'Loot - Smithing Stones'
    if any(i['id'] in {10140,10168} and i['category']==1 for i in items):
        return 'Loot - Smithing Stones (Rare)'
    if any(i['id']==150 and i['category']==1 for i in items): return 'Loot - Rune Arcs'
    if any(i['id']==10060 and i['category']==1 for i in items): return 'Loot - Dragon Hearts'
    if any(i['id']==2130 and i['category']==1 for i in items): return 'Key - Celestial Dew'
    if any(i['id']==10070 and i['category']==1 for i in items): return 'Key - Lost Ashes'
    if any(i['id']==10030 and i['category']==1 for i in items): return 'Magic - Memory Stones'
    if any(i['id']==8000 and i['category']==1 for i in items): return 'Loot - Stonesword Keys'
    if any(i['id']==8186 and i['category']==1 for i in items): return 'Key - Imbued Sword Keys'
    if any(i['id'] in {8185, 2008033} and i['category']==1 for i in items): return 'Key - Larval Tears'
    if any(i['id']==800010 and i['category']==1 for i in items): return 'Reforged - Rune Pieces'
    if any(i['id'] in PATE and i['category']==1 for i in items): return 'Loot - Prattling Pates'
    # Spirit Ashes (excluding Lhutel 358000 — already filtered)
    if any(i['category']==1 and 300000<=i['id']<=399999 and i['id']!=358000 for i in items):
        return 'Equipment - Spirits'
    if any(i['category']==1 and i['id']==358000 for i in items):
        return '[FILTERED] Lhutel (excluded already)'
    if any(i['category']==1 and i['id'] in SORC for i in items): return 'Magic - Sorceries'
    if any(i['category']==1 and i['id'] in INCAN for i in items): return 'Magic - Incantations'
    if any(i['category']==1 and i['id'] in CRAFTING for i in items): return 'Loot - Crafting Materials'
    if has_cat(2): return 'Equipment - Armaments (or Ammo)'
    if has_cat(3): return 'Equipment - Armour'
    if has_cat(4): return 'Equipment - Talismans'
    if has_cat(5): return 'Equipment - Ashes of War'
    if has_name('bell bearing'): return 'Loot - Bell-Bearings'
    if has_name('cookbook'): return 'Key - Cookbooks'
    if any(c in (SORTS.get(i['id'],-1) for i in items if i['category']==1) for c in CONS_GROUPS):
        return 'Loot - Consumables'
    return f"<uncategorized goods: {[(i['id'], i['name']) for i in items]}>"

# Categorize each phantom record (deduplicate by (entity, sub_lot))
by_category = defaultdict(int)
unique_items_by_category = defaultdict(set)
for r in phantom_records:
    cat = categorize(r['items'])
    by_category[cat] += 1
    for it in r['items']:
        unique_items_by_category[cat].add((it['id'], it['name']))

print(f"\n=== Phantom records by category (would disappear after fix) ===")
for cat in sorted(by_category.keys(), key=lambda c: -by_category[c]):
    print(f"  {by_category[cat]:5d}  {cat}")
    for iid, nm in sorted(unique_items_by_category[cat])[:3]:
        print(f"            ↳ items: id={iid} '{nm}'")
    if len(unique_items_by_category[cat]) > 3:
        print(f"            ↳ ... and {len(unique_items_by_category[cat])-3} more unique items")

# 6) Count actual MASSEDIT rows that would disappear
MASSEDIT_DIR = DATA_DIR / 'massedit_generated'
# For each MASSEDIT file, count rows whose lot is in phantom_lots_seen
# (this is approximate: MASSEDIT rows are renumbered, but generate_loot writes the
#  original itemLotId in a parallel JSON. Easier: check if a deduplicated count
#  matches phantom_records count.)
print(f"\n=== MASSEDIT row count audit ===")
if MASSEDIT_DIR.exists():
    for me in sorted(MASSEDIT_DIR.glob('*.MASSEDIT')):
        text = me.read_text(encoding='utf-8', errors='ignore')
        # Count unique row IDs (one per WorldMapPointParam ID)
        row_ids = set(re.findall(r'id (\d+):', text))
        print(f"  {me.name}: {len(row_ids)} total rows")
EOF