#!/usr/bin/env python3
"""Re-generate phantom_records_report.md and phantom_records_full.csv enriched with:
   - enemy_name (TutorialTitle FMG)
   - location_name (PlaceName FMG) — both for dungeons and overworld map tiles
"""
import sys, io, struct, json, csv, re
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
from pathlib import Path
from collections import defaultdict, Counter
import config
from pythonnet import load; load('coreclr')
from System.Reflection import Assembly
from System import Array, Type as SysType, Object
asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
import SoulsFormats
_str = SysType.GetType('System.String')
_emcls = asm.GetType('SoulsFormats.EMEVD')
_emr = _emcls.BaseType.GetMethod('Read', Array[SysType]([_str]))

DATA_DIR = config.TOOLS_DIR.parent / 'data'
PROJ_DIR = config.TOOLS_DIR.parent.parent

# --- Name lookups
enemy_map = json.load(open(DATA_DIR/'enemy_tutorial_mapping.json'))
tutorial_names = {int(k): v for k, v in json.load(open(DATA_DIR/'tutorial_title_names.json', encoding='utf-8')).items()}
tutorial_ids = set(json.load(open(DATA_DIR/'tutorial_title_ids.json')))
place_names = json.load(open(DATA_DIR/'PlaceName_engus.json', encoding='utf-8'))
# normalize PlaceName keys to int
place_names = {int(k): v for k, v in place_names.items()}

def strip_color(s):
    """Strip <font> tags from PlaceName entries."""
    return re.sub(r'<[^>]+>', '', s) if s else s

# Use the variant-aware resolver (from extract_all_items.py)
def resolve_enemy_name(model, npc_param_id):
    base_id = enemy_map.get(model, 0)
    if base_id == 0:
        return f"<unknown model {model}>"
    if npc_param_id <= 0:
        return tutorial_names.get(base_id, f"<id {base_id}>")
    variant = (npc_param_id // 1000) % 10
    if variant == 0:
        return tutorial_names.get(base_id, f"<id {base_id}>")
    vid = base_id + variant * 100
    if vid in tutorial_ids:
        return tutorial_names.get(vid, tutorial_names.get(base_id, f"<id {vid}>"))
    return tutorial_names.get(base_id, f"<id {base_id}>")

# Replicate resolve_location_id from massedit_common
valid_ids = set(json.load(open(DATA_DIR/'valid_location_ids.json')))
def resolve_location_id(map_name):
    parts = map_name.replace('.msb','').split('_')
    if len(parts) < 4: return 0
    area = int(parts[0][1:]); sub = int(parts[1])
    if area in (60, 61): return 0
    loc_id = area*1000 + sub*10
    if loc_id not in valid_ids: loc_id = area*10000 + sub*100 + 1
    if loc_id not in valid_ids: loc_id = area*1000
    if loc_id not in valid_ids: loc_id = 0
    return loc_id

# Overworld region naming: area+grid → human label. Use WorldMapPointParam
# overworld "area markers" (iconId 83 = generic area marker) from the data CSV.
# Simpler: hard-coded major regions of Lands Between based on grid bounds.
LANDS_BETWEEN_REGIONS = [
    # (gx_min, gx_max, gz_min, gz_max, area_name)
    (29, 38, 48, 55, "Caelid"),
    (29, 38, 39, 48, "Mistwood/East Limgrave"),
    (21, 28, 39, 48, "West Limgrave"),
    (21, 32, 49, 60, "Weeping Peninsula"),
    (29, 41, 32, 39, "Altus Plateau"),
    (37, 50, 30, 41, "Mountaintops of the Giants"),
    (12, 24, 32, 50, "Liurnia of the Lakes"),
    (45, 55, 42, 56, "Consecrated Snowfield"),
]
DLC_REGIONS = [
    (10, 60, 10, 80, "Land of Shadow (DLC)"),
]

def resolve_location_name(map_name, grid_x=None, grid_z=None):
    """Return human-readable location string."""
    # First try PlaceName ID (works for dungeons)
    loc_id = resolve_location_id(map_name)
    if loc_id and loc_id in place_names:
        return strip_color(place_names[loc_id])
    # Overworld: use grid heuristic
    parts = map_name.replace('.msb','').split('_')
    if len(parts) >= 4 and parts[0] in ('m60','m61'):
        try:
            gx = int(parts[1]); gz = int(parts[2])
            if parts[0] == 'm60':
                for gxmin,gxmax,gzmin,gzmax,name in LANDS_BETWEEN_REGIONS:
                    if gxmin<=gx<=gxmax and gzmin<=gz<=gzmax:
                        return f"{name} (overworld m60_{gx:02d}_{gz:02d})"
                return f"Lands Between (m60_{gx:02d}_{gz:02d})"
            else:
                return f"Land of Shadow (m61_{gx:02d}_{gz:02d})"
        except: pass
    return f"<{map_name}>"


# --- Re-run phantom detection
db = json.load(open(DATA_DIR/'items_database.json', encoding='utf-8'))
msb_idx = json.load(open(DATA_DIR/'msb_entity_index.json', encoding='utf-8'))
name_to_entity = {(info['map'], info['name']): int(eid_s)
                  for eid_s, info in msb_idx.items() if info.get('kind')=='enemy'}

OLD_TEMPLATES = {90005200:(8,16,20),90005210:(8,16,20),90005300:(8,16,20),
    90005301:(8,16,20),90005860:(16,24,28),90005861:(16,24,28),
    90005880:(16,24,28),90005881:(16,24,28),90005882:(16,24,28),
    90005883:(16,24,28),90005885:(16,24,28),
    90005750:(8,16,20),90005774:(8,12,16),90005792:(8,24,28),
    90005632:(8,16,20),90005110:(8,20,24),90005390:(8,28,32),90005555:(8,12,16)}
NON_AWARDERS = {90005200,90005210,90005881,90005882,90005883,90005885}
phantom_pairs, real_pairs = set(), set()
phantom_pair_template = {}
for path in sorted((config.ERR_MOD_DIR/'event').glob('*.emevd.dcx')):
    em = _emr.Invoke(None, Array[Object]([str(path)]))
    for ev in em.Events:
        for ins in ev.Instructions:
            if int(ins.Bank)!=2000 or int(ins.ID)!=6: continue
            b = bytes(ins.ArgData) if ins.ArgData else b''
            if len(b)<8: continue
            eid = struct.unpack_from('<i',b,4)[0]
            if eid not in OLD_TEMPLATES: continue
            ent_o,lot_o,min_a = OLD_TEMPLATES[eid]
            if len(b)<min_a: continue
            ent = struct.unpack_from('<i',b,ent_o)[0]
            lot = struct.unpack_from('<i',b,lot_o)[0]
            if ent<=0 or lot<=0: continue
            pair = (ent, lot)
            if eid in NON_AWARDERS:
                phantom_pairs.add(pair); phantom_pair_template.setdefault(pair, eid)
            else:
                real_pairs.add(pair)
truly_phantom = phantom_pairs - real_pairs

LHUTEL = 358000
PATE = {2200,2201,2202,2203,2204,2205,2206,2207,2002150}
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

# Apply prefilters
db_f = [r for r in db if not r.get('from_fallback') and r.get('eventFlag',0)>0]
fc = Counter(r.get('eventFlag',0) for r in db_f)
db_f = [r for r in db_f if r.get('source')!='enemy' or fc[r['eventFlag']]==1]

# Collect all phantom records (across all source=emevd, even those that don't reach MASSEDIT)
per_cat = defaultdict(lambda: {'phantom':[]})
all_phantom = []
for r in db:  # use full db (not just prefiltered) for the "dead records" sections too
    if r.get('source') != 'emevd': continue
    cls, tmpl = classify(r)
    if cls != 'phantom': continue
    cat = categorize(r['items'])
    enriched = {
        'category': cat,
        'map': r['map'],
        'location_name': resolve_location_name(r['map']),
        'areaNo': r['areaNo'],
        'gridX': r['gridX'], 'gridZ': r['gridZ'],
        'x': r['x'], 'y': r['y'], 'z': r['z'],
        'partName': r['partName'],
        'enemyModel': r.get('enemyModel',''),
        'npcParamId': r.get('npcParamId',0),
        'enemy_name': resolve_enemy_name(r.get('enemyModel',''), r.get('npcParamId',0)),
        'entity_id': name_to_entity.get((r['map'], r['partName']), 0),
        'itemLotId': r['itemLotId'],
        'eventFlag': r.get('eventFlag',0),
        'source_template': tmpl,
        'items': '; '.join(f"id={i['id']}|{i.get('name','?')}|x{i['num']}" for i in r['items']),
        'massedit_impact': 'YES' if cat in ('Loot - Smithing Stones (Low)','Equipment - Spirits') else 'NO',
    }
    all_phantom.append(enriched)
    if cat: per_cat[cat]['phantom'].append(enriched)

# Sort
all_phantom.sort(key=lambda r: (r['massedit_impact']!='YES', r['category'] or '~', r['map'], r['partName']))

# --- Write CSV
csv_path = PROJ_DIR / 'phantom_records_full.csv'
fields = ['category','massedit_impact','location_name','map','areaNo','gridX','gridZ',
          'x','y','z','partName','enemyModel','enemy_name','npcParamId','entity_id',
          'itemLotId','eventFlag','source_template','items']
with open(csv_path,'w',encoding='utf-8',newline='') as f:
    w = csv.DictWriter(f, fieldnames=fields)
    w.writeheader()
    w.writerows(all_phantom)
print(f"CSV written: {csv_path}  ({len(all_phantom)} rows)")

# --- Write Markdown
def dedup(records):
    seen=set(); out=[]
    for r in records:
        pid = r['items'].split('|')[0].split('=')[-1]  # first item id
        try: pid=int(pid)
        except: pid=0
        k=(pid, round(r['x'],1), round(r['y'],1), round(r['z'],1))
        if k in seen: continue
        seen.add(k); out.append(r)
    return out

def count_me(name):
    p = DATA_DIR / 'massedit_generated' / f'{name}.MASSEDIT'
    if not p.exists(): return None
    return len(set(re.findall(r'id (\d+):', p.read_text(encoding='utf-8', errors='ignore'))))

md = PROJ_DIR / 'phantom_records_report.md'
with open(md,'w',encoding='utf-8') as f:
    f.write("# Phantom items_database records — full diff (enriched)\n\n")
    f.write("Effect of removing 6 non-awarder templates from "
            "`extract_all_items.py::TEMPLATE_EVENTS`:\n")
    f.write("`90005200, 90005210, 90005881, 90005882, 90005883, 90005885`.\n\n")
    f.write("**Method:** for each `source='emevd'` record in `items_database.json`, "
            "look up its `partName`→`entity_id` via `msb_entity_index.json`, then walk "
            "back up to 20 base lots to find which template's `(entity, lot)` pair "
            "generated it. Phantom = generated *only* by a non-awarder template.\n\n")
    f.write("**Enrichments in this report:**\n")
    f.write("- `enemy_name` — TutorialTitle FMG text resolved via "
            "`enemy_tutorial_mapping.json` + `tutorial_title_names.json`, "
            "with variant-aware resolution by `npcParamId`.\n")
    f.write("- `location_name` — PlaceName FMG via `resolve_location_id` for "
            "dungeons; for overworld (m60/m61) — region name from grid coords "
            "heuristic. Same data path that `generate_loot_massedit.py` uses for `textId2`.\n\n")
    f.write("---\n\n## Summary\n\n")

    # Build summary table
    cats_md = ['Loot - Smithing Stones (Low)', 'Equipment - Spirits']
    f.write("| MASSEDIT category | Current rows | Phantom records | Phantom rows lost (after dedup) | Expected rows |\n")
    f.write("|---|---:|---:|---:|---:|\n")
    total_p = total_p_dd = 0
    for c in cats_md:
        p = per_cat[c]['phantom']
        if not p: continue
        dd = dedup(p); cur = count_me(c); exp = (cur - len(dd)) if cur else None
        total_p += len(p); total_p_dd += len(dd)
        f.write(f"| `{c}.MASSEDIT` | {cur if cur else '—'} | {len(p)} | {len(dd)} | {exp if exp else '—'} |\n")
    f.write(f"| **TOTAL** | | **{total_p}** | **{total_p_dd}** | |\n\n")

    f.write("**Not affected (different generator / no LOOT_CATEGORIES match):**\n")
    f.write("- `Reforged - Rune Pieces.MASSEDIT` — generated by `generate_pieces_massedit.py` from `rune_pieces.json` (AEG099_821 MSB objects), not from `items_database`.\n")
    f.write("- `Reforged - Ember Pieces.MASSEDIT` — similarly from `ember_pieces.json`.\n")
    f.write("- Phantom Rune Piece records (id=800010) and Lhutel records (id=358000) exist in items_database but are silently dropped/filtered by `generate_loot_massedit.py`.\n\n")
    f.write("---\n\n")

    # Detailed per-category sections
    for cname in cats_md:
        p = per_cat[cname]['phantom']
        if not p: continue
        f.write(f"## `{cname}.MASSEDIT` — {len(p)} phantom records\n\n")

        f.write("### By source template\n")
        by_tmpl = defaultdict(int)
        for r in p: by_tmpl[r['source_template']] += 1
        for t, n in sorted(by_tmpl.items(), key=lambda x: -x[1]):
            f.write(f"- `{t}` — {n} records\n")
        f.write("\n")

        f.write("### Unique items affected\n")
        ui = defaultdict(int)
        for r in p:
            for chunk in r['items'].split('; '):
                bits = chunk.split('|')
                if len(bits)>=2:
                    iid=bits[0].split('=')[-1]; nm=bits[1]
                    ui[(iid, nm)] += 1
        for (iid, nm), n in sorted(ui.items(), key=lambda x: -x[1]):
            f.write(f"- {n}× `id={iid}` **{nm}**\n")
        f.write("\n")

        f.write("### By enemy (top 20 — what creature this fake-drop is attached to)\n")
        be = defaultdict(int)
        for r in p: be[(r['enemyModel'], r['enemy_name'])] += 1
        for (mdl, nm), n in sorted(be.items(), key=lambda x: -x[1])[:20]:
            f.write(f"- {n}× `{mdl}` **{nm}**\n")
        if len(be) > 20:
            f.write(f"- ... and {len(be)-20} more enemy types\n")
        f.write("\n")

        f.write("### By location (top 20)\n")
        bl = defaultdict(int)
        for r in p: bl[r['location_name']] += 1
        for loc, n in sorted(bl.items(), key=lambda x: -x[1])[:20]:
            f.write(f"- {n}× {loc}\n")
        if len(bl) > 20:
            f.write(f"- ... and {len(bl)-20} more locations\n")
        f.write("\n")

        f.write("### Full record list\n\n")
        f.write("| # | enemy | location | map tile | partName | coords | itemLotId | eventFlag | items | template |\n")
        f.write("|---:|---|---|---|---|---|---:|---:|---|---:|\n")
        for i, r in enumerate(p, 1):
            items_short = '; '.join(c.split('|')[1] for c in r['items'].split('; '))
            f.write(f"| {i} | {r['enemy_name']} (`{r['enemyModel']}`) | {r['location_name']} | "
                    f"`{r['map']}` | `{r['partName']}` | ({r['x']:.1f}, {r['y']:.1f}, {r['z']:.1f}) | "
                    f"{r['itemLotId']} | {r['eventFlag']} | {items_short} | `{r['source_template']}` |\n")
        f.write("\n---\n\n")

    # Dead records sections
    f.write("## Records that disappear from `items_database.json` with no MASSEDIT impact\n\n")
    for label, item_id, reason in [
        ("Lhutel the Headless", 358000,
         "Already filtered out in `generate_loot_massedit.py:270` (`i['id'] != 358000` in `Equipment - Spirits`). Never produced MASSEDIT markers."),
        ("Rune Piece", 800010,
         "Not matched by any `LOOT_CATEGORIES` filter in `generate_loot_massedit.py`. Real Rune Piece markers come from `rune_pieces.json` (AEG099_821 MSB objects), not from items_database."),
    ]:
        dead = [r for r in all_phantom if any(f"id={item_id}|" in c for c in [r['items']])]
        f.write(f"### {label} (id={item_id}) — {len(dead)} records\n\n")
        f.write(reason + "\n\n")
        if dead:
            f.write("**By enemy (top 15):**\n")
            be = defaultdict(int)
            for r in dead: be[(r['enemyModel'], r['enemy_name'])] += 1
            for (mdl, nm), n in sorted(be.items(), key=lambda x: -x[1])[:15]:
                f.write(f"- {n}× `{mdl}` **{nm}**\n")
            if len(be) > 15: f.write(f"- ... and {len(be)-15} more\n")
            f.write("\n**By location (top 15):**\n")
            bl = defaultdict(int)
            for r in dead: bl[r['location_name']] += 1
            for loc, n in sorted(bl.items(), key=lambda x: -x[1])[:15]:
                f.write(f"- {n}× {loc}\n")
            if len(bl) > 15: f.write(f"- ... and {len(bl)-15} more\n")
            f.write("\n*Full list in `phantom_records_full.csv` (filter `massedit_impact=NO`)*\n\n")

print(f"Markdown written: {md}  ({md.stat().st_size} bytes)")
