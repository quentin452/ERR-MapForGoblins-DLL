#!/usr/bin/env python3
"""Rebuild phantom_records_report.md and missing_awards_report.md without
fabricating overworld region names.

Location resolution rules:
- Dungeons (areaNo != 60, 61): authoritative PlaceName from
  `valid_location_ids.json` + `PlaceName_engus.json` (same path
  generate_loot_massedit.py:resolve_location_id uses).
- Overworld tiles (m60_XX_YY, m61_XX_YY): show map code only ("m60_XX_YY").
  As an OPTIONAL hint, append region context from the wiki MD
  (Elden Ring Map List - Souls Modding.md) — only for tiles where a
  connected dungeon's "Site of Grace" line names the region. Marked
  "(wiki: ...)" so it's clearly a secondary source, not game data.
"""
import sys, io, struct, json, csv, os, tempfile, re
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
from pathlib import Path
from collections import defaultdict, Counter
import config
from pythonnet import load; load('coreclr')
from System.Reflection import Assembly
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
import SoulsFormats
_str = SysType.GetType('System.String')
_pcls = asm.GetType('SoulsFormats.PARAM')
_pr = _pcls.BaseType.GetMethod('Read', Array[SysType]([_str]))
_emcls = asm.GetType('SoulsFormats.EMEVD')
_emr = _emcls.BaseType.GetMethod('Read', Array[SysType]([_str]))
_fcls = asm.GetType('SoulsFormats.FMG')
_fr = _fcls.BaseType.GetMethod('Read', Array[SysType]([_str]))
_bcls = asm.GetType('SoulsFormats.BND4')
_br = _bcls.BaseType.GetMethod('Read', Array[SysType]([_str]))

DATA_DIR = config.TOOLS_DIR.parent / 'data'
PROJ_DIR = config.TOOLS_DIR.parent.parent

# -- Common data
msb_idx = json.load(open(DATA_DIR/'msb_entity_index.json', encoding='utf-8'))
entity_by_id = {int(k): v for k, v in msb_idx.items()}
name_to_entity = {(info['map'], info['name']): int(k)
                  for k, info in msb_idx.items() if info.get('kind')=='enemy'}
enemy_map = json.load(open(DATA_DIR/'enemy_tutorial_mapping.json'))
tutorial_names = {int(k): v for k, v in json.load(open(DATA_DIR/'tutorial_title_names.json', encoding='utf-8')).items()}
tutorial_ids = set(json.load(open(DATA_DIR/'tutorial_title_ids.json')))
place_names = {int(k): re.sub(r'<[^>]+>','',v) for k, v in json.load(open(DATA_DIR/'PlaceName_engus_merged.json', encoding='utf-8')).items()}
valid_loc = set(json.load(open(DATA_DIR/'valid_location_ids.json')))
tile_region_map = json.load(open(DATA_DIR/'tile_region_map.json', encoding='utf-8'))

def resolve_enemy_name(model, npc_param_id):
    base_id = enemy_map.get(model, 0)
    if base_id == 0:
        return None
    if npc_param_id <= 0:
        return tutorial_names.get(base_id)
    variant = (npc_param_id // 1000) % 10
    if variant == 0:
        return tutorial_names.get(base_id)
    vid = base_id + variant * 100
    if vid in tutorial_ids:
        return tutorial_names.get(vid) or tutorial_names.get(base_id)
    return tutorial_names.get(base_id)

def resolve_dungeon_place_id(map_name):
    """Mirror generate_loot_massedit's resolve_location_id for dungeons only."""
    parts = map_name.replace('.msb','').split('_')
    if len(parts) < 4: return 0
    area = int(parts[0][1:]); sub = int(parts[1])
    if area in (60, 61):
        return 0  # overworld — no authoritative dungeon name
    for cand in [area*1000+sub*10, area*10000+sub*100+1, area*1000]:
        if cand in valid_loc:
            return cand
    return 0

def resolve_location_label(map_name):
    """Return (label, source) tuple. All sources are authoritative game-data
    derived; no fabrication or wiki-based guessing.

    source ∈ {'PlaceName FMG (dungeon)',
              'BonfireWarpParam direct (overworld)',
              'BonfireWarpParam inherited (overworld neighbor)',
              'map code only'}
    """
    parts = map_name.replace('.msb','').split('_')
    if len(parts) < 4:
        return (map_name, 'map code only')
    area = int(parts[0][1:])
    if area not in (60, 61):
        # Dungeon — use PlaceName via resolve_location_id chain
        pid = resolve_dungeon_place_id(map_name)
        if pid and pid in place_names:
            return (place_names[pid], 'PlaceName FMG (dungeon)')
        return (map_name, 'map code only')
    # Overworld — use tile_region_map.json (built from BonfireWarpParam +
    # BonfireWarpSubCategoryParam + PlaceName FMG)
    try:
        gx = int(parts[1]); gz = int(parts[2])
    except: return (map_name, 'map code only')
    key = f"m{area:02d}_{gx:02d}_{gz:02d}"
    info = tile_region_map.get(key)
    if not info:
        return (key, 'map code only')
    major = info.get('majorRegion')
    sub = info.get('subRegion')
    sub_id = info.get('subCategoryId')
    tab_id = info.get('tabId')
    # Build label: "Major / Sub" if sub is named AND different from major;
    # else just "Major"; fall back to PlaceName by subCategoryId if available.
    if major and sub and sub != major:
        label = f"{major} / {sub}"
    elif major:
        # sub_id text not in FMG — show id as hint after major
        if sub_id and not sub:
            label = f"{major} (sub area #{sub_id})"
        else:
            label = major
    elif sub:
        label = sub
    else:
        label = key
    src_field = info.get('source', '')
    if src_field.startswith('BonfireWarpParam'):
        src_tag = 'BonfireWarpParam direct (overworld)'
    elif src_field.startswith('inherited'):
        src_tag = f"BonfireWarpParam inherited ({src_field.split('from ')[-1]})"
    else:
        src_tag = 'map code only'
    return (label, src_tag)

# -- Load goods names (multi-FMG, category-aware)
goods_names = {}  # (cat_letter, id) -> text
for mp in [config.ERR_MOD_DIR/'msg/engus/item_dlc02.msgbnd.dcx']:
    bndmsg = _br.Invoke(None, Array[Object]([str(mp)]))
    for f in bndmsg.Files:
        n = str(f.Name)
        nl = n.lower()
        for key, suffix in [('w','weaponname'),('a','protectorname'),('t','accessoryname'),
                            ('g','gemname'),('i','goodsname')]:
            if nl.endswith(suffix + '.fmg'):
                tmp = os.path.join(tempfile.gettempdir(), '_n.fmg')
                SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
                fmg = _fr.Invoke(None, Array[Object]([tmp]))
                for e in fmg.Entries:
                    t = str(e.Text) if e.Text else ''
                    if t and t != '[ERROR]':
                        goods_names.setdefault((key, int(e.ID)), t)
                break
def item_name(item_id, cat):
    return goods_names.get({1:'i',2:'w',3:'a',4:'t',5:'g'}.get(cat,'i'), 0) if False else goods_names.get(({1:'i',2:'w',3:'a',4:'t',5:'g'}.get(cat,'i'), item_id), f"<id {item_id} cat {cat}>")

# -- Load ItemLotParam for lot expansion
defs = {}
for xml in config.PARAMDEF_DIR.glob('*.xml'):
    try:
        d = SoulsFormats.PARAMDEF.XmlDeserialize(str(xml), False)
        if d and d.ParamType: defs[str(d.ParamType)] = d
    except: pass
bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(config.ERR_MOD_DIR/'regulation.bin'))
def load_param(name):
    for f in bnd.Files:
        if name in str(f.Name):
            tmp = os.path.join(tempfile.gettempdir(),'_p.param')
            SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
            p = _pr.Invoke(None, Array[Object]([tmp]))
            pdef = defs.get(str(p.ParamType))
            if pdef: p.ApplyParamdef(pdef)
            return p
def to_dict(p):
    d = {}
    for r in p.Rows:
        rid = int(r.ID); ent = {}
        for c in r.Cells:
            nm = str(c.Def.InternalName)
            try: v = int(c.Value)
            except: continue
            if nm.startswith('lotItemId0') or nm.startswith('lotItemCategory0') or \
               nm.startswith('lotItemNum0') or nm == 'getItemFlagId':
                ent[nm] = v
        d[rid] = ent
    return d
lot_map = to_dict(load_param('ItemLotParam_map'))
lot_enemy = to_dict(load_param('ItemLotParam_enemy'))

def expand_lot(lot_id):
    out = []
    src = None
    if lot_id in lot_map: src=('map', lot_map)
    elif lot_id in lot_enemy: src=('enemy', lot_enemy)
    if not src: return []
    src_name, lookup = src
    base = lookup.get(lot_id, {})
    items = []
    for slot in range(1,9):
        iid = base.get(f'lotItemId0{slot}',0); cat = base.get(f'lotItemCategory0{slot}',0); num = base.get(f'lotItemNum0{slot}',0)
        if iid>0 and cat>0:
            items.append({'id': iid, 'category': cat, 'num': num, 'name': item_name(iid, cat)})
    flag = base.get('getItemFlagId', 0)
    if items: out.append((lot_id, items, flag, src_name))
    if src_name == 'map':
        for off in range(1, 20):
            sub = lookup.get(lot_id+off)
            if sub is None: break
            f = sub.get('getItemFlagId', 0)
            if f <= 0: continue
            sub_items = []
            for slot in range(1,9):
                iid = sub.get(f'lotItemId0{slot}',0); cat = sub.get(f'lotItemCategory0{slot}',0); num = sub.get(f'lotItemNum0{slot}',0)
                if iid>0 and cat>0:
                    sub_items.append({'id': iid, 'category': cat, 'num': num, 'name': item_name(iid, cat)})
            if sub_items: out.append((lot_id+off, sub_items, f, src_name))
    return out

# ================================================================
#  REPORT 1: phantom_records_report.md (rebuild honest)
# ================================================================
print("=== Rebuilding phantom_records_report.md ===")

OLD_TEMPLATES = {90005200:(8,16,20),90005210:(8,16,20),90005300:(8,16,20),
    90005301:(8,16,20),90005860:(16,24,28),90005861:(16,24,28),
    90005880:(16,24,28),90005881:(16,24,28),90005882:(16,24,28),
    90005883:(16,24,28),90005885:(16,24,28),
    90005750:(8,16,20),90005774:(8,12,16),90005792:(8,24,28),
    90005632:(8,16,20),90005110:(8,20,24),90005390:(8,28,32),90005555:(8,12,16)}
NON_AWARDERS = {90005200,90005210,90005881,90005882,90005883,90005885}

EVENT_DIR = config.ERR_MOD_DIR / 'event'
phantom_pairs, real_pairs, phantom_pair_template = set(), set(), {}
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
            if ent<=0 or lot<=0: continue
            pair = (ent, lot)
            if eid in NON_AWARDERS:
                phantom_pairs.add(pair); phantom_pair_template.setdefault(pair, eid)
            else:
                real_pairs.add(pair)
truly_phantom = phantom_pairs - real_pairs

db = json.load(open(DATA_DIR/'items_database.json', encoding='utf-8'))
db_f = [r for r in db if not r.get('from_fallback') and r.get('eventFlag',0)>0]
fc = Counter(r.get('eventFlag',0) for r in db_f)
db_f = [r for r in db_f if r.get('source')!='enemy' or fc[r['eventFlag']]==1]

LHUTEL = 358000
def categorize(items):
    if any(i['id'] in {10100,10101,10102,10103,10104,10105,10160,10161,10162,10163,10164,10165}
           and i['category']==1 for i in items): return 'Loot - Smithing Stones (Low)'
    if any(i['category']==1 and 300000<=i['id']<=399999 and i['id']!=LHUTEL for i in items):
        return 'Equipment - Spirits'
    if any(i['id']==LHUTEL for i in items): return '[FILTERED-Lhutel]'
    if any(i['id']==800010 for i in items): return '[NO-MATCH-RunePiece]'
    return None

def classify(rec):
    key = (rec['map'], rec['partName'])
    eid = name_to_entity.get(key)
    if eid is None: return ('unknown', None)
    for back in range(20):
        cand = rec['itemLotId'] - back
        if (eid, cand) in real_pairs: return ('real', None)
        if (eid, cand) in truly_phantom: return ('phantom', phantom_pair_template[(eid,cand)])
    return ('no_match', None)

per_cat = defaultdict(lambda: {'phantom': []})
all_phantom = []
for r in db:
    if r.get('source') != 'emevd': continue
    cls, tmpl = classify(r)
    if cls != 'phantom': continue
    cat = categorize(r['items'])
    loc_label, loc_src = resolve_location_label(r['map'])
    enemy_nm = resolve_enemy_name(r.get('enemyModel',''), r.get('npcParamId',0))
    enriched = {
        'category': cat,
        'map': r['map'], 'location_label': loc_label, 'location_source': loc_src,
        'areaNo': r['areaNo'], 'gridX': r['gridX'], 'gridZ': r['gridZ'],
        'x': r['x'], 'y': r['y'], 'z': r['z'],
        'partName': r['partName'], 'enemyModel': r.get('enemyModel',''),
        'enemy_name': enemy_nm,
        'npcParamId': r.get('npcParamId',0),
        'entity_id': name_to_entity.get((r['map'], r['partName']), 0),
        'itemLotId': r['itemLotId'], 'eventFlag': r.get('eventFlag',0),
        'source_template': tmpl,
        'items': '; '.join(f"id={i['id']}|{i.get('name','?')}|x{i['num']}" for i in r['items']),
        'massedit_impact': 'YES' if cat in ('Loot - Smithing Stones (Low)','Equipment - Spirits') else 'NO',
    }
    all_phantom.append(enriched)
    if cat: per_cat[cat]['phantom'].append(enriched)

# Sort: YES first
all_phantom.sort(key=lambda r: (r['massedit_impact']!='YES', r['category'] or '~',
                                 r['areaNo'], r['gridX'], r['gridZ'], r['partName']))

# Write CSV
csv_path = PROJ_DIR / 'phantom_records_full.csv'
fields = ['category','massedit_impact','location_label','location_source','map',
          'areaNo','gridX','gridZ','x','y','z','partName','enemyModel','enemy_name',
          'npcParamId','entity_id','itemLotId','eventFlag','source_template','items']
with open(csv_path,'w',encoding='utf-8',newline='') as f:
    w = csv.DictWriter(f, fieldnames=fields); w.writeheader(); w.writerows(all_phantom)
print(f"  CSV: {csv_path} ({len(all_phantom)} rows)")

# Write MD
def dedup(records):
    seen=set(); out=[]
    for r in records:
        pid_str = r['items'].split('|')[0].split('=')[-1]
        try: pid=int(pid_str)
        except: pid=0
        k = (pid, round(r['x'],1), round(r['y'],1), round(r['z'],1))
        if k in seen: continue
        seen.add(k); out.append(r)
    return out

def count_me(name):
    p = DATA_DIR/'massedit_generated'/f'{name}.MASSEDIT'
    if not p.exists(): return None
    return len(set(re.findall(r'id (\d+):', p.read_text(encoding='utf-8', errors='ignore'))))

md = PROJ_DIR / 'phantom_records_report.md'
with open(md,'w',encoding='utf-8') as f:
    f.write("# Phantom items_database records — full diff (honest)\n\n")
    f.write("Effect of removing 6 non-awarder templates from "
            "`extract_all_items.py::TEMPLATE_EVENTS`: "
            "`90005200, 90005210, 90005881, 90005882, 90005883, 90005885`.\n\n")
    f.write("## Location label source\n\n")
    f.write("All labels come from game data:\n\n")
    f.write("- **Dungeons** (`areaNo ∉ {60, 61}`): `resolve_location_id(map)` →"
            " `PlaceName_engus.json` (merged from item.msgbnd + item_dlc01 + item_dlc02).\n")
    f.write("- **Overworld tiles** (m60/m61): per-tile region from "
            "`tile_region_map.json` built from `regulation.bin`:\n"
            "  - `BonfireWarpParam` row → `(areaNo, gridXNo, gridZNo, bonfireSubCategoryId)`\n"
            "  - `BonfireWarpSubCategoryParam[id]` → row id matches a PlaceName FMG entry (sub-region) plus `tabId` referencing the major region\n"
            "  - Per-tile region = dominant subCategoryId among that tile's graces\n"
            "  - For tiles without graces: inherited from nearest neighbor (Manhattan distance ≤ 4)\n\n"
            "Label format: `Major Region / Sub-Region` (or just `Major Region` when sub-region is the same as major).\n\n")
    f.write("---\n\n## Summary\n\n")

    f.write("| MASSEDIT category | Current rows | Phantom records | Phantom rows lost | Expected rows |\n")
    f.write("|---|---:|---:|---:|---:|\n")
    cats_md = ['Loot - Smithing Stones (Low)', 'Equipment - Spirits']
    total_p = total_p_dd = 0
    for c in cats_md:
        p = per_cat[c]['phantom']
        if not p: continue
        dd = dedup(p); cur = count_me(c); exp = (cur - len(dd)) if cur else None
        total_p += len(p); total_p_dd += len(dd)
        f.write(f"| `{c}.MASSEDIT` | {cur or '—'} | {len(p)} | {len(dd)} | {exp or '—'} |\n")
    f.write(f"| **TOTAL** | | **{total_p}** | **{total_p_dd}** | |\n\n")
    f.write("Phantom records in `items_database.json` outside MASSEDIT impact:\n")
    n_lh = sum(1 for r in all_phantom if any(f"id=358000|" in c for c in [r['items']]))
    n_rp = sum(1 for r in all_phantom if any(f"id=800010|" in c for c in [r['items']]))
    f.write(f"- {n_lh}× Lhutel the Headless (filtered at generate_loot:270)\n")
    f.write(f"- {n_rp}× Rune Piece (no matching `LOOT_CATEGORIES` filter; Rune Piece markers come from `rune_pieces.json` via `generate_pieces_massedit.py`)\n\n")
    f.write("---\n\n")

    for cname in cats_md:
        p = per_cat[cname]['phantom']
        if not p: continue
        f.write(f"## `{cname}.MASSEDIT` — {len(p)} phantom records\n\n")
        # By source template
        f.write("### By source template\n")
        bt = defaultdict(int)
        for r in p: bt[r['source_template']] += 1
        for t, n in sorted(bt.items(), key=lambda x:-x[1]):
            f.write(f"- `{t}` — {n}\n")
        f.write("\n")
        # By item
        f.write("### Items affected\n")
        ui = defaultdict(int)
        for r in p:
            for ch in r['items'].split('; '):
                bits = ch.split('|')
                if len(bits)>=2:
                    iid = bits[0].split('=')[-1]
                    ui[(iid, bits[1])] += 1
        for (iid, nm), n in sorted(ui.items(), key=lambda x:-x[1]):
            f.write(f"- {n}× `id={iid}` **{nm}**\n")
        f.write("\n")
        # By enemy
        f.write("### By enemy / entity\n")
        be = defaultdict(int)
        for r in p:
            nm = r['enemy_name'] or '<unknown>'
            be[(r['enemyModel'], nm)] += 1
        for (mdl, nm), n in sorted(be.items(), key=lambda x:-x[1])[:25]:
            f.write(f"- {n}× `{mdl}` **{nm}**\n")
        if len(be) > 25:
            f.write(f"- ... and {len(be)-25} more types\n")
        f.write("\n")
        # By location
        f.write("### By location\n")
        bl = defaultdict(int); src_by_loc = {}
        for r in p:
            bl[r['location_label']] += 1
            src_by_loc[r['location_label']] = r['location_source']
        for loc, n in sorted(bl.items(), key=lambda x:-x[1])[:30]:
            src = src_by_loc.get(loc, '')
            tag = ' *(PlaceName)*' if 'dungeon' in src else (' *(grace, inherited)*' if 'inherited' in src else (' *(grace direct)*' if 'direct' in src else ' *(no data)*'))
            f.write(f"- {n}× {loc}{tag}\n")
        if len(bl) > 30:
            f.write(f"- ... and {len(bl)-30} more\n")
        f.write("\n### Full record list\n\n")
        f.write("| # | enemy | location (source) | map tile | partName | coords | itemLotId | eventFlag | items | template |\n")
        f.write("|---:|---|---|---|---|---|---:|---:|---|---:|\n")
        for i, r in enumerate(p, 1):
            src = r['location_source']
            if 'dungeon' in src: src_tag = 'PlaceName'
            elif 'direct' in src: src_tag = 'grace direct'
            elif 'inherited' in src: src_tag = 'grace inherited'
            else: src_tag = 'no data'
            enemy_disp = f"{r['enemy_name']} (`{r['enemyModel']}`)" if r['enemy_name'] else f"`{r['enemyModel']}`"
            items_short = '; '.join(c.split('|')[1] for c in r['items'].split('; '))
            f.write(f"| {i} | {enemy_disp} | {r['location_label']} ({src_tag}) | `{r['map']}` | `{r['partName']}` | "
                    f"({r['x']:.1f}, {r['y']:.1f}, {r['z']:.1f}) | {r['itemLotId']} | {r['eventFlag']} | {items_short} | `{r['source_template']}` |\n")
        f.write("\n---\n\n")
print(f"  MD:  {md} ({md.stat().st_size} bytes)")

# ================================================================
#  REPORT 2: missing_awards_report.md (rebuild honest)
# ================================================================
print("\n=== Rebuilding missing_awards_report.md ===")
NEW_TEMPLATES = {
    90005724: {'name': 'Force-Death Treasure Award', 'lot': ('caller_byte', 16), 'entity': ('caller_byte', 8)},
    90005753: {'name': 'NPC Quest Reward (variant)', 'lot': ('caller_byte', 16), 'entity': ('caller_byte', 8)},
    90005768: {'name': 'Dual-Award Event Flag Check', 'lots': [('caller_byte', 12), ('caller_byte', 20)], 'entity': None},
    90005776: {'name': 'Timed Award', 'lot': ('caller_byte', 16), 'entity': None},
    90005940: {'name': 'Hardcoded Rune Piece Award', 'lot': ('fixed', 800020), 'entity': ('caller_byte', 8)},
    90005950: {'name': "Hardcoded Cooperator's Coin (kill-triggered)", 'lot': ('fixed', 5460), 'entity': ('caller_byte', 8)},
    90005959: {'name': 'NPC Reward (variant)', 'lot': ('caller_byte', 12), 'entity': ('caller_byte', 8)},
    90006050: {'name': "Hardcoded Cooperator's Coin (asset-triggered)", 'lot': ('fixed', 5460), 'entity': ('caller_byte', 8)},
    90006055: {'name': "Hardcoded Cooperator's Coin (asset, 5-flag variant)", 'lot': ('fixed', 5460), 'entity': ('caller_byte', 8)},
    90006300: {'name': 'Hardcoded Runic Trace Award', 'lot': ('fixed', 998210), 'entity': ('caller_byte', 8)},
    90006400: {'name': 'Action-button corpse loot', 'lot': ('caller_byte', 16), 'entity': ('caller_byte', 8)},
    90006900: {'name': 'Asset-interaction Award (Artifact Pieces)', 'lot': ('caller_byte', 8), 'entity': ('caller_byte', 8)},
    90007100: {'name': 'Treasure Object Award', 'lot': ('caller_byte', 16), 'entity': ('caller_byte', 8)},
}

all_calls = defaultdict(list)
for path in sorted(EVENT_DIR.glob('*.emevd.dcx')):
    if path.name == 'common_func.emevd.dcx': continue
    em = _emr.Invoke(None, Array[Object]([str(path)]))
    map_name = path.name.replace('.emevd.dcx','')
    for ev in em.Events:
        for ins in ev.Instructions:
            if int(ins.Bank)!=2000 or int(ins.ID)!=6: continue
            b = bytes(ins.ArgData) if ins.ArgData else b''
            if len(b)<8: continue
            tid = struct.unpack_from('<i', b, 4)[0]
            if tid in NEW_TEMPLATES:
                all_calls[tid].append((map_name, b))

def extract_int(b, off):
    return struct.unpack_from('<i', b, off)[0] if len(b) >= off+4 else None

records_by_template = defaultdict(list)
for tid, cfg in NEW_TEMPLATES.items():
    for caller_map, b in all_calls.get(tid, []):
        ent_id = None
        if cfg.get('entity') and cfg['entity'][0] == 'caller_byte':
            ent_id = extract_int(b, cfg['entity'][1])
        lots = []
        if 'lots' in cfg:
            for spec in cfg['lots']:
                if spec[0]=='caller_byte':
                    v = extract_int(b, spec[1])
                    if v: lots.append(v)
                elif spec[0]=='fixed': lots.append(spec[1])
        else:
            sp = cfg['lot']
            if sp[0]=='caller_byte':
                v = extract_int(b, sp[1])
                if v: lots.append(v)
            elif sp[0]=='fixed': lots.append(sp[1])
        ent_info = entity_by_id.get(ent_id, {}) if ent_id else {}
        partName = ent_info.get('name', '')
        emodel = ent_info.get('model', '')
        entity_map = ent_info.get('map', caller_map)
        x, y, z = ent_info.get('x', 0), ent_info.get('y', 0), ent_info.get('z', 0)
        ename = resolve_enemy_name(emodel, 0)
        loc_label, loc_src = resolve_location_label(entity_map)
        for lot in lots:
            expanded = expand_lot(lot)
            if not expanded:
                records_by_template[tid].append({
                    'template': tid, 'caller_map': caller_map, 'entity_id': ent_id,
                    'partName': partName, 'enemyModel': emodel, 'enemy_name': ename,
                    'entity_map': entity_map, 'location_label': loc_label, 'location_source': loc_src,
                    'x': x, 'y': y, 'z': z,
                    'lot': lot, 'sub_lot': lot, 'lot_source': 'NOT FOUND', 'flag': 0,
                    'items_str': '<no row>', 'primary_id': 0, 'primary_name': '<no row>', 'primary_cat': 0,
                })
                continue
            for sub_id, items, flag, src in expanded:
                p_it = items[0] if items else {}
                records_by_template[tid].append({
                    'template': tid, 'caller_map': caller_map, 'entity_id': ent_id,
                    'partName': partName, 'enemyModel': emodel, 'enemy_name': ename,
                    'entity_map': entity_map, 'location_label': loc_label, 'location_source': loc_src,
                    'x': x, 'y': y, 'z': z,
                    'lot': lot, 'sub_lot': sub_id, 'lot_source': src, 'flag': flag,
                    'items_str': '; '.join(f"{i['name']}×{i['num']}" for i in items),
                    'primary_id': p_it.get('id', 0), 'primary_name': p_it.get('name', '?'),
                    'primary_cat': p_it.get('category', 0),
                })

total = sum(len(v) for v in records_by_template.values())

# Write CSV
csv_path = PROJ_DIR / 'missing_awards_full.csv'
fields = ['template','caller_map','entity_id','partName','enemyModel','enemy_name',
          'entity_map','location_label','location_source','x','y','z',
          'lot','sub_lot','lot_source','flag','primary_id','primary_cat','primary_name','items_str']
flat = sorted([r for recs in records_by_template.values() for r in recs],
              key=lambda r: (r['template'], r['caller_map'], r['entity_id'] or 0))
with open(csv_path, 'w', encoding='utf-8', newline='') as f:
    w = csv.DictWriter(f, fieldnames=fields); w.writeheader(); w.writerows(flat)
print(f"  CSV: {csv_path} ({len(flat)} rows)")

# Write MD
md = PROJ_DIR / 'missing_awards_report.md'
with open(md, 'w', encoding='utf-8') as f:
    f.write("# Missing awarder templates — what would be ADDED if pipeline supported them (honest)\n\n")
    f.write("13 emevd templates currently absent from `extract_all_items.py::TEMPLATE_EVENTS` ")
    f.write("but verified (via DarkScript3 emedf) to contain real `2003:04 Award Item Lot` or ")
    f.write("`2003:36 Award Items (Including Clients)` instructions.\n\n")
    f.write("## Location label source\n\n")
    f.write("All labels come from game data:\n\n"
            "- **Dungeons** (`areaNo ∉ {60, 61}`): `resolve_location_id(map)` → PlaceName FMG (merged from item.msgbnd + DLC).\n"
            "- **Overworld tiles** (m60/m61): per-tile region from `BonfireWarpParam` → "
            "`BonfireWarpSubCategoryParam` → PlaceName FMG (`tabId` = major region, row id = sub-region). "
            "Tiles without graces inherit from nearest neighbor (Manhattan distance ≤ 4).\n\n")
    f.write("## Summary\n\n")
    f.write("| Template | Callers | Records | Primary items |\n|---:|---:|---:|---|\n")
    for tid in sorted(NEW_TEMPLATES.keys()):
        recs = records_by_template.get(tid, [])
        prims = defaultdict(int)
        for r in recs: prims[r['primary_name']] += 1
        prim_str = ', '.join(f"{nm}×{n}" for nm, n in sorted(prims.items(), key=lambda x:-x[1])[:3])
        f.write(f"| `{tid}` | {len(all_calls.get(tid,[]))} | {len(recs)} | {prim_str} |\n")
    f.write(f"| **TOTAL** | | **{total}** | |\n\n---\n\n")

    for tid in sorted(NEW_TEMPLATES.keys()):
        cfg = NEW_TEMPLATES[tid]
        recs = records_by_template.get(tid, [])
        if not recs: continue
        f.write(f"## Template `{tid}` — {cfg['name']}\n\n")
        f.write(f"- **Callers:** {len(all_calls.get(tid, []))}\n")
        f.write(f"- **Records (incl. sub-lot expansion):** {len(recs)}\n")
        if 'lot' in cfg:
            ls = cfg['lot']
            f.write(f"- **Lot source:** {'caller InitializeEvent arg byte '+str(ls[1]) if ls[0]=='caller_byte' else 'hardcoded `'+str(ls[1])+'`'}\n")
        if 'lots' in cfg:
            f.write(f"- **Lot sources (dual):** {', '.join('byte '+str(s[1]) for s in cfg['lots'])}\n")
        if cfg.get('entity'):
            f.write(f"- **Entity location:** caller arg byte {cfg['entity'][1]}\n")
        f.write("\n**Items awarded:**\n")
        ic = defaultdict(int)
        for r in recs: ic[(r['primary_id'], r['primary_name'])] += 1
        for (iid, nm), n in sorted(ic.items(), key=lambda x:-x[1]):
            f.write(f"- {n}× `id={iid}` **{nm}**\n")
        f.write("\n**By entity model:**\n")
        ec = defaultdict(int)
        for r in recs: ec[(r['enemyModel'], r['enemy_name'])] += 1
        for (mdl, nm), n in sorted(ec.items(), key=lambda x:-x[1])[:15]:
            if not mdl: f.write(f"- {n}× *(entity not in MSB)*\n")
            else: f.write(f"- {n}× `{mdl}` " + (f"**{nm}**" if nm else "*(asset)*") + "\n")
        f.write("\n**By location:**\n")
        lc = defaultdict(int); lsrc = {}
        for r in recs: lc[r['location_label']] += 1; lsrc[r['location_label']] = r['location_source']
        for loc, n in sorted(lc.items(), key=lambda x:-x[1])[:25]:
            src = lsrc.get(loc, '')
            tag = ' *(PlaceName)*' if 'dungeon' in src else (' *(grace, inherited)*' if 'inherited' in src else (' *(grace direct)*' if 'direct' in src else ' *(no data)*'))
            f.write(f"- {n}× {loc}{tag}\n")
        if len(lc) > 25: f.write(f"- ... and {len(lc)-25} more\n")
        f.write("\n**Records:**\n\n| # | item | entity | location | map | partName | coords | lot |\n|---:|---|---|---|---|---|---|---:|\n")
        for i, r in enumerate(recs[:60], 1):
            coords = f"({r['x']:.1f}, {r['y']:.1f}, {r['z']:.1f})" if r['x'] else '—'
            partName = f"`{r['partName']}`" if r['partName'] else '*(not in MSB)*'
            ent = (f"{r['enemy_name']} (`{r['enemyModel']}`)" if r['enemy_name']
                   else (f"`{r['enemyModel']}`" if r['enemyModel'] else '*(asset)*'))
            f.write(f"| {i} | **{r['primary_name']}** | {ent} | {r['location_label']} | `{r['entity_map']}` | {partName} | {coords} | {r['sub_lot']} |\n")
        if len(recs)>60:
            f.write(f"\n*(showing 60 of {len(recs)}, full in `missing_awards_full.csv`)*\n")
        f.write("\n---\n\n")
print(f"  MD:  {md} ({md.stat().st_size} bytes)")
print("\nDone.")
