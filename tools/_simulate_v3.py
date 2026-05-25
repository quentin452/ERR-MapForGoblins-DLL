#!/usr/bin/env python3
"""Accurate simulation: which items_database records would disappear when the
6 non-awarder templates are removed, AND which of those records actually
produce MASSEDIT rows (matching the real LOOT_CATEGORIES filters AND prefilters).
Writes a detailed markdown report.
"""
import sys, io, struct, json, os, tempfile, re
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
_emcls = asm.GetType('SoulsFormats.EMEVD')
_emr = _emcls.BaseType.GetMethod('Read', Array[SysType]([_str]))

DATA_DIR = config.TOOLS_DIR.parent / 'data'
PROJ_DIR = config.TOOLS_DIR.parent.parent

# Load items_database + MSB entity index
db = json.load(open(DATA_DIR/'items_database.json', encoding='utf-8'))
msb_idx = json.load(open(DATA_DIR/'msb_entity_index.json', encoding='utf-8'))
name_to_entity = {}
for eid_s, info in msb_idx.items():
    if info.get('kind') == 'enemy':
        name_to_entity[(info['map'], info['name'])] = int(eid_s)

# --- 1) Replicate emevd scan to get (entity, lot) pairs per template
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
phantom_pairs = set()
real_pairs = set()
phantom_pair_template = {}  # (entity, lot) -> first template causing it
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
                phantom_pairs.add(pair)
                phantom_pair_template.setdefault(pair, eid)
            else:
                real_pairs.add(pair)
truly_phantom = phantom_pairs - real_pairs

# --- 2) Replicate generate_loot_massedit prefilters
print("=== Replicating generate_loot_massedit prefilters ===")
db_filtered = [r for r in db if not r.get('from_fallback')]
print(f"  After 'not from_fallback': {len(db_filtered)}")
db_filtered = [r for r in db_filtered if r.get('eventFlag', 0) > 0]
print(f"  After 'eventFlag > 0': {len(db_filtered)}")
flag_counts = Counter(r.get('eventFlag', 0) for r in db_filtered)
before = len(db_filtered)
db_filtered = [r for r in db_filtered if r.get('source')!='enemy' or flag_counts[r['eventFlag']]==1]
print(f"  After shared-enemy-flag filter: {len(db_filtered)} (-{before-len(db_filtered)})")

# --- 3) Replicate LOOT_CATEGORIES filters EXACTLY (subset that's emevd-relevant)
# Copy the filters verbatim from generate_loot_massedit.py
LHUTEL = 358000
PATE = {2200,2201,2202,2203,2204,2205,2206,2207,2002150}

# These are the EXACT category filters from generate_loot_massedit.py
def make_categories():
    cats = {}
    cats['Key - Celestial Dew']      = lambda items: any(i['id']==2130 and i['category']==1 for i in items)
    cats['Key - Cookbooks']          = lambda items: any(i['category']==1 and 'cookbook' in i.get('name','').lower() for i in items)
    cats['Key - Crystal Tears']      = lambda items: any(i['category']==1 and any(s in i.get('name','').lower() for s in
                                          ('crystal tear','cracked tear','hardtear','bubbletear','hidden tear','oil-soaked tear')) for i in items)
    cats['Key - Imbued Sword Keys']  = lambda items: any(i['id']==8186 and i['category']==1 for i in items)
    cats['Key - Larval Tears']       = lambda items: any(i['category']==1 and i['id'] in (8185,2008033) for i in items)
    cats['Key - Lost Ashes']         = lambda items: any(i['id']==10070 and i['category']==1 for i in items)
    cats['Key - Pots n Perfumes']    = lambda items: any(i['category']==1 and 'perfume' in i.get('name','').lower() for i in items)
    cats['Key - Scadutree Fragments']= lambda items: any(i['id']==10080 and i['category']==1 for i in items)
    cats['Key - Whetblades']         = lambda items: any(i['category']==1 and 'whetblade' in i.get('name','').lower() for i in items)
    cats['Magic - Memory Stones']    = lambda items: any(i['id']==10030 and i['category']==1 for i in items)
    cats['Equipment - Armaments']    = lambda items: any(i['category']==2 and i['id']<50000000 for i in items)
    cats['Equipment - Armour']       = lambda items: any(i['category']==3 for i in items)
    cats['Equipment - Talismans']    = lambda items: any(i['category']==4 for i in items)
    cats['Equipment - Spirits']      = lambda items: any(i['category']==1 and 300000<=i['id']<=399999 and i['id']!=LHUTEL for i in items)
    cats['Equipment - Ashes of War'] = lambda items: any(i['category']==5 for i in items)
    cats['Loot - Stonesword Keys']   = lambda items: any(i['id']==8000 and i['category']==1 for i in items)
    cats['Loot - Ammo']              = lambda items: any(i['category']==2 and i['id']>=50000000 for i in items)
    cats['Loot - Smithing Stones (Low)'] = lambda items: any(i['id'] in
        {10100,10101,10102,10103,10104,10105,10160,10161,10162,10163,10164,10165} and i['category']==1 for i in items)
    cats['Loot - Smithing Stones']   = lambda items: any(i['id'] in
        {10106,10107,10166,10167,10200,10150,10151} and i['category']==1 for i in items)
    cats['Loot - Smithing Stones (Rare)'] = lambda items: any(i['id'] in {10140,10168} and i['category']==1 for i in items)
    cats['Loot - Golden Runes (Low)'] = lambda items: any(i['id'] in
        {2900,2901,2902,2903,2904,2905,2906,2907,2002951,2002952,2002953} and i['category']==1 for i in items)
    cats['Loot - Golden Runes']      = lambda items: any(i['id'] in
        {2908,2909,2910,2911,2912,2913,2914,2915,2916,2917,2918,2919,2002954,2002955,2002956,2002957,2002958,2002959,2002960} and i['category']==1 for i in items)
    cats['Loot - Rune Arcs']         = lambda items: any(i['id']==150 and i['category']==1 for i in items)
    cats['Loot - Dragon Hearts']     = lambda items: any(i['id']==10060 and i['category']==1 for i in items)
    cats['Loot - Prattling Pates']   = lambda items: any(i['id'] in PATE and i['category']==1 for i in items)
    cats['Loot - Stat Boosts']       = lambda items: any(i['id'] in {1020,1021,1022,1023,1024,1025} and i['category']==1 for i in items)
    return cats
CATEGORIES = make_categories()

# --- 4) For each prefiltered record, find category + classify phantom/real
def categorize(items):
    for cname, f in CATEGORIES.items():
        if f(items): return cname
    return None  # silently dropped — no MASSEDIT impact

def classify(rec):
    """Return ('phantom', template_id) or ('real',None) or ('uncertain',None)."""
    key = (rec['map'], rec['partName'])
    eid = name_to_entity.get(key)
    if eid is None:
        return ('unknown_entity', None)
    # walk back from itemLotId looking for matching (entity, base_lot)
    for back in range(20):
        cand = rec['itemLotId'] - back
        if (eid, cand) in real_pairs:
            return ('real', None)
        if (eid, cand) in truly_phantom:
            return ('phantom', phantom_pair_template.get((eid, cand)))
    return ('no_template_match', None)

# Dedup like generate_loot_massedit
def dedup(records):
    seen = set(); out = []
    for r in records:
        pid = r['items'][0]['id'] if r['items'] else 0
        k = (pid, round(r['x'],1), round(r['y'],1), round(r['z'],1))
        if k in seen: continue
        seen.add(k); out.append(r)
    return out

# --- 5) Walk all prefiltered records, build report data
per_cat = defaultdict(lambda: {'all': [], 'phantom': [], 'real': [], 'other': []})
for r in db_filtered:
    cat = categorize(r['items'])
    if cat is None: continue
    cls, tmpl = classify(r)
    per_cat[cat]['all'].append(r)
    if cls == 'phantom':
        per_cat[cat]['phantom'].append((r, tmpl))
    elif cls == 'real':
        per_cat[cat]['real'].append(r)
    else:
        per_cat[cat]['other'].append((r, cls))

# Read actual MASSEDIT row counts
MASSEDIT_DIR = DATA_DIR / 'massedit_generated'
def count_massedit(name):
    p = MASSEDIT_DIR / f'{name}.MASSEDIT'
    if not p.exists(): return None
    text = p.read_text(encoding='utf-8', errors='ignore')
    return len(set(re.findall(r'id (\d+):', text)))

# Summary
print("\n=== Per-category impact ===")
total_phantom = 0
total_phantom_dedup = 0
for cat in sorted(per_cat, key=lambda c: -len(per_cat[c]['phantom'])):
    p = [r for r, _ in per_cat[cat]['phantom']]
    real = per_cat[cat]['real']
    if not p: continue
    phantom_dedup = dedup(p)
    current_rows = count_massedit(cat)
    total_phantom += len(p)
    total_phantom_dedup += len(phantom_dedup)
    print(f"  {cat}")
    print(f"    items_database  total={len(per_cat[cat]['all'])}  phantom={len(p)}  real={len(real)}")
    print(f"    after dedup     phantom_rows_lost={len(phantom_dedup)}")
    print(f"    MASSEDIT current={current_rows}  expected={current_rows - len(phantom_dedup) if current_rows else '?'}")

print(f"\n  TOTAL phantom items_database records: {total_phantom}")
print(f"  TOTAL phantom MASSEDIT rows (after dedup): {total_phantom_dedup}")

# --- 6) Output markdown report
md_path = PROJ_DIR / 'phantom_records_report.md'
with open(md_path, 'w', encoding='utf-8') as f:
    f.write("# Phantom items_database records — full diff\n\n")
    f.write("Effect of removing 6 non-awarder templates from ")
    f.write("`extract_all_items.py::TEMPLATE_EVENTS`:\n")
    f.write("`90005200, 90005210, 90005881, 90005882, 90005883, 90005885`.\n\n")
    f.write("**Method:** for each emevd-source record in `items_database.json`, ")
    f.write("look up its `partName`→`entity_id` via `msb_entity_index.json`, ")
    f.write("then walk back up to 20 base lots to find which template's `(entity, lot)` ")
    f.write("pair generated it. Phantom = generated *only* by a non-awarder template.\n\n")
    f.write("---\n\n")
    f.write("## Summary\n\n")
    f.write("| MASSEDIT category | Current rows | Phantom records | Phantom rows lost (after dedup) | Expected rows |\n")
    f.write("|---|---:|---:|---:|---:|\n")
    affected = []
    for cat in sorted(per_cat, key=lambda c: -len(per_cat[c]['phantom'])):
        p = [r for r, _ in per_cat[cat]['phantom']]
        if not p: continue
        phantom_dedup = dedup(p)
        current_rows = count_massedit(cat)
        expected = (current_rows - len(phantom_dedup)) if current_rows is not None else None
        affected.append(cat)
        f.write(f"| `{cat}.MASSEDIT` | {current_rows if current_rows is not None else '—'} | {len(p)} | {len(phantom_dedup)} | {expected if expected is not None else '—'} |\n")
    f.write(f"| **TOTAL** | | **{total_phantom}** | **{total_phantom_dedup}** | |\n")
    f.write("\nNot affected (different generator / no LOOT_CATEGORIES match):\n")
    f.write("- `Reforged - Rune Pieces.MASSEDIT` — generated by `generate_pieces_massedit.py` from `rune_pieces.json` (AEG099_821 MSB objects), not from `items_database`. ")
    f.write("Phantom Rune Piece records in items_database (id=800010) are not matched by any `LOOT_CATEGORIES` filter and are silently dropped during MASSEDIT generation.\n")
    f.write("- `Reforged - Ember Pieces.MASSEDIT` — similarly generated from `ember_pieces.json`.\n")
    f.write("- World/Quest/Reforged-Items/Fortunes/Sealed-Curios — none of these affected (phantom items don't match their filters).\n\n")
    f.write("Total phantom records in `items_database.json` (all of them, including ones with no MASSEDIT impact):\n")
    f.write(f"- Lhutel the Headless (id=358000): {sum(1 for r in db_filtered if any(i['id']==358000 for i in r['items']) and classify(r)[0]=='phantom')} records — filtered by `Equipment - Spirits` category exclusion (line 270 of generate_loot)\n")
    f.write(f"- Rune Piece (id=800010): {sum(1 for r in db_filtered if any(i['id']==800010 for i in r['items']) and classify(r)[0]=='phantom')} records — not matched by any LOOT_CATEGORIES filter (no MASSEDIT impact)\n\n")
    f.write("---\n\n")

    # Per-category detailed lists
    for cat in affected:
        p = per_cat[cat]['phantom']
        if not p: continue
        f.write(f"## `{cat}.MASSEDIT` — {len(p)} phantom records\n\n")
        # Group by template
        by_tmpl = defaultdict(list)
        for r, tmpl in p:
            by_tmpl[tmpl].append(r)
        f.write(f"**By source template:**\n\n")
        for tmpl, recs in sorted(by_tmpl.items(), key=lambda x: -len(x[1])):
            f.write(f"- `{tmpl}` ({len(recs)} records)\n")
        f.write("\n**By map:**\n\n")
        by_map = defaultdict(int)
        for r, _ in p: by_map[r['map']] += 1
        for m, n in sorted(by_map.items(), key=lambda x: -x[1])[:15]:
            f.write(f"- `{m}` — {n} records\n")
        if len(by_map) > 15:
            f.write(f"- ... and {len(by_map)-15} more maps\n")
        f.write("\n**Unique items affected:**\n\n")
        unique_items = defaultdict(int)
        for r, _ in p:
            for it in r['items']:
                unique_items[(it['id'], it.get('name','?'))] += 1
        for (iid, nm), cnt in sorted(unique_items.items(), key=lambda x: -x[1]):
            f.write(f"- {cnt}x `id={iid}` **{nm}**\n")
        f.write("\n**Sample records (first 10):**\n\n")
        f.write("| map | partName | enemyModel | coords (x, y, z) | lot | flag | items |\n")
        f.write("|---|---|---|---|---|---|---|\n")
        for r, tmpl in p[:10]:
            items_s = ', '.join(f"{i['name']}×{i['num']}" for i in r['items'])
            f.write(f"| `{r['map']}` | `{r['partName']}` | `{r.get('enemyModel','?')}` | "
                    f"({r['x']:.1f}, {r['y']:.1f}, {r['z']:.1f}) | {r['itemLotId']} | {r.get('eventFlag','?')} | {items_s} |\n")
        if len(p) > 10:
            f.write(f"\n*(showing 10 of {len(p)} records — full CSV available on request)*\n")
        f.write("\n")
        f.write("---\n\n")

    # Also dump informational sections for filtered/non-MASSEDIT phantoms
    f.write("## Records that disappear from `items_database.json` but had no MASSEDIT impact\n\n")
    lhutel = [r for r in db_filtered if any(i['id']==358000 for i in r['items']) and classify(r)[0]=='phantom']
    rune = [r for r in db_filtered if any(i['id']==800010 for i in r['items']) and classify(r)[0]=='phantom']
    f.write(f"### Lhutel the Headless (id=358000) — {len(lhutel)} records\n\n")
    f.write("Already filtered out in `generate_loot_massedit.py:270` "
            "(`i['id'] != 358000` in `Equipment - Spirits` category). "
            "These records existed in items_database but never produced markers. "
            "Cleanup removes the dead records.\n\n")
    f.write(f"By map: {dict(Counter(r['map'][:7] for r in lhutel).most_common(10))}\n\n")
    f.write(f"### Rune Piece (id=800010) — {len(rune)} records\n\n")
    f.write("Not matched by any LOOT_CATEGORIES filter in `generate_loot_massedit.py`. "
            "Real Rune Piece markers are generated by `generate_pieces_massedit.py` from "
            "`data/rune_pieces.json` (AEG099_821 MSB objects), not from items_database. "
            "These phantom records were never used.\n\n")
    f.write(f"By map: {dict(Counter(r['map'][:7] for r in rune).most_common(10))}\n\n")

print(f"\nReport written to: {md_path}")
print(f"Size: {md_path.stat().st_size} bytes")
