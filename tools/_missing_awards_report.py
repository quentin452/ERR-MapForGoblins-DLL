#!/usr/bin/env python3
"""Generate missing_awards_report.md and missing_awards_full.csv — analog of
phantom_records_report.md but for the 13 NEW awarder templates that the current
pipeline doesn't know about.

For each template, for each caller, resolve:
  - entity_id → MSB part (map, partName, model, npcParamId, coords)
  - lot_id → ItemLotParam_map row → primary item (id, name, category)
  - enemy_name via TutorialTitle FMG
  - location_name via PlaceName FMG / overworld region heuristic
"""
import sys, io, struct, json, csv, os, tempfile, re
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

# --- Load supporting data
msb_idx = json.load(open(DATA_DIR/'msb_entity_index.json', encoding='utf-8'))
# Forward: entity_id (int) -> {map, name, model, x, y, z, ...}
entity_by_id = {int(k): v for k, v in msb_idx.items()}
enemy_map = json.load(open(DATA_DIR/'enemy_tutorial_mapping.json'))
tutorial_names = {int(k): v for k, v in json.load(open(DATA_DIR/'tutorial_title_names.json', encoding='utf-8')).items()}
tutorial_ids = set(json.load(open(DATA_DIR/'tutorial_title_ids.json')))
place_names_raw = {int(k): v for k, v in json.load(open(DATA_DIR/'PlaceName_engus.json', encoding='utf-8')).items()}
def strip_color(s): return re.sub(r'<[^>]+>','',s) if s else s
place_names = {k: strip_color(v) for k, v in place_names_raw.items()}
valid_loc = set(json.load(open(DATA_DIR/'valid_location_ids.json')))

def resolve_enemy_name(model, npc_param_id):
    base_id = enemy_map.get(model, 0)
    if base_id == 0: return f"<unknown model {model}>"
    if npc_param_id <= 0: return tutorial_names.get(base_id, f"<id {base_id}>")
    variant = (npc_param_id // 1000) % 10
    if variant == 0: return tutorial_names.get(base_id, f"<id {base_id}>")
    vid = base_id + variant * 100
    if vid in tutorial_ids:
        return tutorial_names.get(vid, tutorial_names.get(base_id, f"<id {vid}>"))
    return tutorial_names.get(base_id, f"<id {base_id}>")

LB_REGIONS = [
    (29,38,48,55,"Caelid"), (29,38,39,48,"Mistwood/East Limgrave"),
    (21,28,39,48,"West Limgrave"), (21,32,49,60,"Weeping Peninsula"),
    (29,41,32,39,"Altus Plateau"), (37,50,30,41,"Mountaintops of the Giants"),
    (12,24,32,50,"Liurnia of the Lakes"), (45,55,42,56,"Consecrated Snowfield"),
]
def resolve_location_id(map_name):
    parts = map_name.replace('.msb','').split('_')
    if len(parts) < 4: return 0
    area = int(parts[0][1:]); sub = int(parts[1])
    if area in (60,61): return 0
    for cand in [area*1000+sub*10, area*10000+sub*100+1, area*1000]:
        if cand in valid_loc: return cand
    return 0

def resolve_location_name(map_name):
    lid = resolve_location_id(map_name)
    if lid and lid in place_names: return place_names[lid]
    parts = map_name.replace('.msb','').split('_')
    if len(parts) >= 4 and parts[0] in ('m60','m61'):
        try:
            gx=int(parts[1]); gz=int(parts[2])
            if parts[0]=='m60':
                for gxmn,gxmx,gzmn,gzmx,name in LB_REGIONS:
                    if gxmn<=gx<=gxmx and gzmn<=gz<=gzmx: return f"{name} (overworld m60_{gx:02d}_{gz:02d})"
                return f"Lands Between (m60_{gx:02d}_{gz:02d})"
            else:
                return f"Land of Shadow (m61_{gx:02d}_{gz:02d})"
        except: pass
    return f"<{map_name}>"

# --- Load ItemLotParam_map for item lookup
defs = {}
for xml in config.PARAMDEF_DIR.glob('*.xml'):
    try:
        d = SoulsFormats.PARAMDEF.XmlDeserialize(str(xml), False)
        if d and d.ParamType: defs[str(d.ParamType)] = d
    except: pass
bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(config.ERR_MOD_DIR/'regulation.bin'))

# Goods names
goods_names = {}
bndmsg = _br.Invoke(None, Array[Object]([str(config.ERR_MOD_DIR/'msg/engus/item_dlc02.msgbnd.dcx')]))
for f in bndmsg.Files:
    nm = str(f.Name)
    if 'GoodsName' in nm or 'WeaponName' in nm or 'ProtectorName' in nm or 'AccessoryName' in nm or 'GemName' in nm:
        tmp = os.path.join(tempfile.gettempdir(),'_n.fmg')
        SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
        fmg = _fr.Invoke(None, Array[Object]([tmp]))
        for e in fmg.Entries:
            t = str(e.Text) if e.Text else ''
            if t and t != '[ERROR]':
                # Disambiguate by category prefix in stored name
                if 'WeaponName' in nm: goods_names.setdefault(('w',int(e.ID)), t)
                elif 'ProtectorName' in nm: goods_names.setdefault(('a',int(e.ID)), t)
                elif 'AccessoryName' in nm: goods_names.setdefault(('t',int(e.ID)), t)
                elif 'GemName' in nm: goods_names.setdefault(('g',int(e.ID)), t)
                else: goods_names.setdefault(('i',int(e.ID)), t)

def item_name(item_id, cat):
    cat_map = {1: 'i', 2: 'w', 3: 'a', 4: 't', 5: 'g'}
    key = (cat_map.get(cat, 'i'), item_id)
    return goods_names.get(key, f"<id {item_id} cat {cat}>")

def load_param(name):
    for f in bnd.Files:
        if name in str(f.Name):
            tmp = os.path.join(tempfile.gettempdir(),'_p.param')
            SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
            p = _pr.Invoke(None, Array[Object]([tmp]))
            pdef = defs.get(str(p.ParamType))
            if pdef: p.ApplyParamdef(pdef)
            return p
    return None

def lot_dict_from(p):
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

lot_map = lot_dict_from(load_param('ItemLotParam_map'))
lot_enemy = lot_dict_from(load_param('ItemLotParam_enemy'))

def expand_lot(lot_id):
    """Replicate the sub-lot walking logic for a base lot. Returns list of (sub_id, items, flag)."""
    out = []
    src = None
    if lot_id in lot_map: src = ('map', lot_map)
    elif lot_id in lot_enemy: src = ('enemy', lot_enemy)
    if not src: return []
    src_name, lookup = src
    # base
    sub = lookup.get(lot_id, {})
    items = []
    for slot in range(1, 9):
        iid = sub.get(f'lotItemId0{slot}', 0); cat = sub.get(f'lotItemCategory0{slot}', 0)
        num = sub.get(f'lotItemNum0{slot}', 0)
        if iid > 0 and cat > 0:
            items.append({'id': iid, 'category': cat, 'num': num, 'name': item_name(iid, cat)})
    flag = sub.get('getItemFlagId', 0)
    if items: out.append((lot_id, items, flag, src_name))
    # sub-lots for _map only
    if src_name == 'map':
        for offset in range(1, 20):
            sub = lookup.get(lot_id + offset)
            if sub is None: break
            f = sub.get('getItemFlagId', 0)
            if f <= 0: continue
            sub_items = []
            for slot in range(1, 9):
                iid = sub.get(f'lotItemId0{slot}', 0); cat = sub.get(f'lotItemCategory0{slot}', 0)
                num = sub.get(f'lotItemNum0{slot}', 0)
                if iid > 0 and cat > 0:
                    sub_items.append({'id': iid, 'category': cat, 'num': num, 'name': item_name(iid, cat)})
            if sub_items: out.append((lot_id + offset, sub_items, f, src_name))
    return out

# --- New templates config (manually verified offsets)
NEW_TEMPLATES = {
    90005724: {'name': 'Force-Death Treasure Award', 'lot': ('caller_byte', 16), 'entity': ('caller_byte', 8)},
    90005753: {'name': 'NPC Quest Reward (variant)', 'lot': ('caller_byte', 16), 'entity': ('caller_byte', 8)},
    90005768: {'name': 'Dual-Award Event Flag Check', 'lots': [('caller_byte', 12), ('caller_byte', 20)], 'entity': None},
    90005776: {'name': 'Timed Award (after seconds + flag)', 'lot': ('caller_byte', 16), 'entity': None},
    90005940: {'name': 'Hardcoded Rune Piece Award', 'lot': ('fixed', 800020), 'entity': ('caller_byte', 8)},
    90005950: {'name': "Hardcoded Cooperator's Coin (kill)", 'lot': ('fixed', 5460), 'entity': ('caller_byte', 8)},
    90005959: {'name': 'NPC Reward (variant)', 'lot': ('caller_byte', 12), 'entity': ('caller_byte', 8)},
    90006050: {'name': "Hardcoded Cooperator's Coin (asset)", 'lot': ('fixed', 5460), 'entity': ('caller_byte', 8)},
    90006055: {'name': "Hardcoded Cooperator's Coin (asset, 5-flag)", 'lot': ('fixed', 5460), 'entity': ('caller_byte', 8)},
    90006300: {'name': 'Hardcoded Runic Trace Award', 'lot': ('fixed', 998210), 'entity': ('caller_byte', 8)},
    90006400: {'name': 'Action-button Drop (corpse loot)', 'lot': ('caller_byte', 16), 'entity': ('caller_byte', 8)},
    90006900: {'name': 'Asset-interaction Award (Artifact Pieces)', 'lot': ('caller_byte', 8), 'entity': ('caller_byte', 8)},
    90007100: {'name': 'Treasure Object Award', 'lot': ('caller_byte', 16), 'entity': ('caller_byte', 8)},
}

# --- Scan emevd
EVENT_DIR = config.ERR_MOD_DIR / 'event'
all_calls = defaultdict(list)
for path in sorted(EVENT_DIR.glob('*.emevd.dcx')):
    if path.name == 'common_func.emevd.dcx': continue
    em = _emr.Invoke(None, Array[Object]([str(path)]))
    map_name = path.name.replace('.emevd.dcx', '')
    for ev in em.Events:
        for ins in ev.Instructions:
            if int(ins.Bank)!=2000 or int(ins.ID)!=6: continue
            b = bytes(ins.ArgData) if ins.ArgData else b''
            if len(b)<8: continue
            tid = struct.unpack_from('<i', b, 4)[0]
            if tid in NEW_TEMPLATES:
                all_calls[tid].append((map_name, b))

# --- Build enriched records
def extract_int(b, off): return struct.unpack_from('<i', b, off)[0] if len(b) >= off+4 else None

records_by_template = defaultdict(list)
for tid, cfg in NEW_TEMPLATES.items():
    for map_name, b in all_calls.get(tid, []):
        # Entity
        ent_id = None
        if cfg.get('entity') and cfg['entity'][0] == 'caller_byte':
            ent_id = extract_int(b, cfg['entity'][1])
        # Lots (support single or list)
        lots = []
        if 'lots' in cfg:
            for spec in cfg['lots']:
                if spec[0] == 'caller_byte':
                    v = extract_int(b, spec[1])
                    if v: lots.append(v)
                elif spec[0] == 'fixed':
                    lots.append(spec[1])
        else:
            spec = cfg['lot']
            if spec[0] == 'caller_byte':
                v = extract_int(b, spec[1])
                if v: lots.append(v)
            elif spec[0] == 'fixed':
                lots.append(spec[1])

        # Resolve entity
        ent_info = entity_by_id.get(ent_id, {}) if ent_id else {}
        # For some templates the entity might be in a different map than the emevd file's map,
        # but most often it matches.
        partName = ent_info.get('name', '')
        emodel = ent_info.get('model', '')
        entity_map = ent_info.get('map', map_name)
        x, y, z = ent_info.get('x', 0), ent_info.get('y', 0), ent_info.get('z', 0)
        enemy_name = resolve_enemy_name(emodel, 0)  # npcParamId not available here; use base
        loc_name = resolve_location_name(entity_map)

        for lot in lots:
            expanded = expand_lot(lot)
            if not expanded:
                # Lot not found in either param — record placeholder
                records_by_template[tid].append({
                    'template': tid,
                    'caller_map': map_name,
                    'entity_id': ent_id, 'entity_partName': partName, 'enemyModel': emodel,
                    'enemy_name': enemy_name,
                    'entity_map': entity_map, 'location_name': loc_name,
                    'x': x, 'y': y, 'z': z,
                    'lot': lot, 'sub_lot': lot, 'lot_source': 'NOT FOUND',
                    'flag': 0, 'items_str': '<no row>', 'primary_id': 0,
                    'primary_name': '<no row>', 'primary_cat': 0,
                })
                continue
            for sub_id, items, flag, src in expanded:
                items_str = '; '.join(f"{i['name']}×{i['num']}" for i in items)
                p = items[0] if items else {}
                records_by_template[tid].append({
                    'template': tid,
                    'caller_map': map_name,
                    'entity_id': ent_id, 'entity_partName': partName, 'enemyModel': emodel,
                    'enemy_name': enemy_name,
                    'entity_map': entity_map, 'location_name': loc_name,
                    'x': x, 'y': y, 'z': z,
                    'lot': lot, 'sub_lot': sub_id, 'lot_source': src, 'flag': flag,
                    'items_str': items_str,
                    'primary_id': p.get('id', 0),
                    'primary_name': p.get('name', '?'),
                    'primary_cat': p.get('category', 0),
                })

total = sum(len(v) for v in records_by_template.values())
print(f"Total enriched records across {len(records_by_template)} templates: {total}")

# --- Write CSV
csv_path = PROJ_DIR / 'missing_awards_full.csv'
fields = ['template','caller_map','entity_id','entity_partName','enemyModel','enemy_name',
          'entity_map','location_name','x','y','z','lot','sub_lot','lot_source','flag',
          'primary_id','primary_cat','primary_name','items_str']
flat = [r for recs in records_by_template.values() for r in recs]
flat.sort(key=lambda r: (r['template'], r['caller_map'], r['entity_id'] or 0))
with open(csv_path, 'w', encoding='utf-8', newline='') as f:
    w = csv.DictWriter(f, fieldnames=fields); w.writeheader(); w.writerows(flat)
print(f"CSV written: {csv_path}  ({len(flat)} rows)")

# --- Write Markdown
md_path = PROJ_DIR / 'missing_awards_report.md'
with open(md_path, 'w', encoding='utf-8') as f:
    f.write("# Missing awarder templates — what would be ADDED if pipeline supported them\n\n")
    f.write("13 emevd templates currently absent from `extract_all_items.py::TEMPLATE_EVENTS` ")
    f.write("but verified (via DarkScript3 emedf) to contain real `2003:04 Award Item Lot` or ")
    f.write("`2003:36 Award Items (Including Clients)` instructions.\n\n")
    f.write("**Method:**\n")
    f.write("1. Scan `common_func.emevd` for every event containing an award instruction.\n")
    f.write("2. Filter to events called via `2000:06 InitializeEvent` from per-map emevds.\n")
    f.write("3. For each call, extract `(entity_id, lot_id)` using manually-verified offsets ")
    f.write("(decoded body of each template — see `_decode_template_body.py`).\n")
    f.write("4. For hardcoded lots, use the constant from the template body.\n")
    f.write("5. Resolve entity_id → MSB part (model, partName, coords) → enemy name via TutorialTitle FMG.\n")
    f.write("6. Resolve lot_id → ItemLotParam_map row → primary item (name from GoodsName FMG).\n\n")

    f.write("## Summary\n\n")
    f.write("| Template | Callers | Records | Primary items | Suggested category |\n")
    f.write("|---:|---:|---:|---|---|\n")
    for tid in sorted(NEW_TEMPLATES.keys()):
        recs = records_by_template.get(tid, [])
        prims = defaultdict(int)
        for r in recs: prims[r['primary_name']] += 1
        prim_str = ', '.join(f"{nm}×{n}" for nm, n in sorted(prims.items(), key=lambda x:-x[1])[:3])
        suggested = ''
        if 'Artifact' in prim_str: suggested = 'Reforged - Artifact Pieces (NEW)'
        elif 'Coin' in prim_str: suggested = 'Reforged - Cooperator Coins (NEW)'
        elif 'Rune Piece' in prim_str: suggested = 'Reforged - Rune Pieces (merge with existing)'
        elif 'Runic Trace' in prim_str: suggested = 'Reforged - Runic Traces (NEW)'
        elif 'Smithing' in prim_str: suggested = 'Loot - Smithing Stones (Low/normal)'
        elif 'Volcanic Storm' in prim_str or 'Sorcery' in prim_str: suggested = 'Magic - Sorceries'
        elif 'Whetstone' in prim_str or 'Perfume' in prim_str: suggested = 'Loot - Reusables / Key - Pots'
        f.write(f"| `{tid}` | {len(all_calls.get(tid,[]))} | {len(recs)} | {prim_str} | {suggested} |\n")
    f.write(f"| **TOTAL** | | **{total}** | | |\n\n---\n\n")

    # Per-template detailed
    for tid in sorted(NEW_TEMPLATES.keys()):
        cfg = NEW_TEMPLATES[tid]
        recs = records_by_template.get(tid, [])
        if not recs: continue
        f.write(f"## Template `{tid}` — {cfg['name']}\n\n")
        f.write(f"- **Callers:** {len(all_calls.get(tid,[]))}\n")
        f.write(f"- **Records (including sub-lot expansion):** {len(recs)}\n")
        if 'lot' in cfg:
            lspec = cfg['lot']
            if lspec[0] == 'caller_byte':
                f.write(f"- **Lot location:** InitializeEvent arg byte {lspec[1]} (parameterized)\n")
            else:
                f.write(f"- **Lot location:** hardcoded constant `{lspec[1]}` in template body\n")
        if 'lots' in cfg:
            f.write(f"- **Lot locations:** {', '.join(str(s) for s in cfg['lots'])} (dual-award)\n")
        if cfg.get('entity'):
            f.write(f"- **Entity location:** InitializeEvent arg byte {cfg['entity'][1]}\n")
        f.write("\n")

        # Top items
        items_c = defaultdict(int)
        for r in recs:
            items_c[(r['primary_id'], r['primary_name'])] += 1
        f.write("**Items awarded:**\n")
        for (iid, nm), n in sorted(items_c.items(), key=lambda x:-x[1]):
            f.write(f"- {n}× `id={iid}` **{nm}**\n")
        f.write("\n")

        # Top enemies
        enem_c = defaultdict(int)
        for r in recs: enem_c[(r['enemyModel'], r['enemy_name'])] += 1
        if enem_c:
            f.write("**By enemy/entity model:**\n")
            for (mdl, nm), n in sorted(enem_c.items(), key=lambda x:-x[1])[:15]:
                if not mdl: f.write(f"- {n}× (no MSB part — entity not resolved)\n")
                else: f.write(f"- {n}× `{mdl}` **{nm}**\n")
            if len(enem_c) > 15:
                f.write(f"- ... and {len(enem_c)-15} more\n")
            f.write("\n")

        # Top locations
        loc_c = defaultdict(int)
        for r in recs: loc_c[r['location_name']] += 1
        f.write("**By location:**\n")
        for loc, n in sorted(loc_c.items(), key=lambda x:-x[1])[:20]:
            f.write(f"- {n}× {loc}\n")
        if len(loc_c) > 20:
            f.write(f"- ... and {len(loc_c)-20} more\n")
        f.write("\n")

        # Sample / full record list
        f.write("**Records:**\n\n")
        f.write("| # | item | enemy | location | map | partName | coords | lot |\n")
        f.write("|---:|---|---|---|---|---|---|---:|\n")
        for i, r in enumerate(recs[:60], 1):
            coords = f"({r['x']:.1f}, {r['y']:.1f}, {r['z']:.1f})" if r['x'] else '—'
            partName = f"`{r['entity_partName']}`" if r['entity_partName'] else '*(not in MSB)*'
            enemy = f"{r['enemy_name']} (`{r['enemyModel']}`)" if r['enemyModel'] else '*(asset)*'
            f.write(f"| {i} | **{r['primary_name']}** | {enemy} | {r['location_name']} | `{r['entity_map']}` | {partName} | {coords} | {r['sub_lot']} |\n")
        if len(recs) > 60:
            f.write(f"\n*(showing 60 of {len(recs)} records — full list in `missing_awards_full.csv`)*\n")
        f.write("\n---\n\n")

print(f"Markdown written: {md_path}  ({md_path.stat().st_size} bytes)")
