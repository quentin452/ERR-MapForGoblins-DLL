#!/usr/bin/env python3
"""Datamine the 222 Emevd residual (the baked LootSource::Emevd lots the runtime
direct-template pass does NOT cover) to decide HOW to recover them at runtime.

The bake positions all 529 Emevd lots via THREE mechanisms (extract_all_items.py):
  (A) direct template awards   -> SHIPPED runtime pass (loot_emevd_drops), ~307 lots.
  (B) event-1200 flag->lot     -> common.emevd RunEvent(1200,[flag,lot]) + a per-map
                                  SetEventFlag(flag,1); the setter event references an
                                  MSB enemy entity = the position (boss-preferred).
  (C) sequence-sibling         -> for a base lot from (A)/(B), the bake walks
                                  base+1, base+2, ... contiguously in ItemLotParam_enemy
                                  (stop at first gap); each sub-lot is its own award
                                  (e.g. 20000 Lhutel -> 20001 Smithing Stone [2]).

This reproduces (A)+(B)+(C) offline and reports, for the 222 residual, how many each
mechanism recovers + the false-positive risk, so we can scope a runtime pass.

Run: PYTHONUTF8=1 PYTHONPATH="C:/Users/iamacat/AppData/Local/Temp/mfg_aux" \
     MFG_PROFILE=err py -3.14 tools/datamine_emevd_residual.py
"""
import sys, io, os, json, struct, tempfile
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pathlib import Path
from collections import Counter, defaultdict
from pythonnet import load
load('coreclr')
import clr
clr.AddReference(str(config.SOULSFORMATS_DLL))
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
import SoulsFormats

# Reuse the extractor's param/MSB helpers.
import extract_all_items as E

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
_str = SysType.GetType('System.String')
_emevd_read = asm.GetType('SoulsFormats.EMEVD').GetMethod(
    'Read', BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str]), None)
_msbe_read = asm.GetType('SoulsFormats.MSBE').GetMethod(
    'Read', BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str]), None)

def _read(meth, data):
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid())+'_re.tmp')
    SysFile.WriteAllBytes(tmp, data.ToArray() if hasattr(data, 'ToArray') else data)
    r = meth.Invoke(None, Array[Object]([tmp])); os.unlink(tmp); return r

TEMPLATE_EVENTS = {
    90005300:(8,16,20), 90005301:(8,16,20),
    90005860:(16,24,28), 90005861:(16,24,28), 90005880:(16,24,28),
    90005750:(8,16,20), 90005753:(8,16,20),
    90005774:(8,12,16), 90005792:(20,24,28),
    90005632:(8,16,20), 90005110:(8,20,24), 90005390:(8,28,32), 90005555:(8,12,16),
}


def lot_item_name(lot_id, item_lots_map, item_lots_enemy, name_dbs):
    lot = item_lots_enemy.get(lot_id) or item_lots_map.get(lot_id)
    if not lot:
        return '?'
    for s in range(1, 9):
        iid = lot.get(f'lotItemId0{s}', 0); cat = lot.get(f'lotItemCategory0{s}', 0)
        if iid > 0 and cat > 0:
            return name_dbs.get(cat, {}).get(iid, f'item{iid}')
    return '(empty)'


def main():
    moddir = config.require_err_mod_dir()
    # ── params ──
    print('=== regulation ===')
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(moddir/'regulation.bin'))
    pdefs = E.load_paramdefs()
    lot_fields = set()
    for i in range(1, 9):
        lot_fields.update([f'lotItemId0{i}', f'lotItemCategory0{i}', f'lotItemNum0{i}'])
    lot_fields.add('getItemFlagId')
    item_lots_map = E.param_to_dict(E.read_param(bnd, 'ItemLotParam_map', pdefs), lot_fields)
    item_lots_enemy = E.param_to_dict(E.read_param(bnd, 'ItemLotParam_enemy', pdefs), lot_fields)
    print(f'  map lots {len(item_lots_map)}, enemy lots {len(item_lots_enemy)}')
    # FMG names (for samples)
    name_dbs = {}
    try:
        name_dbs = E.read_fmg_names(bnd, None) if False else {}
    except Exception:
        name_dbs = {}

    emevd_dir = moddir/'event'
    msb_dir = moddir/'map'/'MapStudio'

    # ── (A) direct template awards: covered lots + entity refs ──
    direct_calls = []   # (entity, lot)
    flag_to_lot = {}    # event-1200 RunEvent
    setter_events = []  # (flag, map, eventId)
    for p in sorted(emevd_dir.glob('*.emevd.dcx')):
        mapn = p.name.replace('.emevd.dcx', '')
        try: em = _read(_emevd_read, SoulsFormats.DCX.Decompress(str(p)))
        except Exception: continue
        for ev in em.Events:
            evid = int(ev.ID)
            for ins in ev.Instructions:
                bank = int(ins.Bank); iid = int(ins.ID)
                args = bytes(ins.ArgData) if ins.ArgData else b''
                # direct template (bank 2000 init, arg[4]=template id)
                if bank == 2000 and len(args) >= 8:
                    tid = struct.unpack_from('<i', args, 4)[0]
                    t = TEMPLATE_EVENTS.get(tid)
                    if t and len(args) >= t[2]:
                        ent = struct.unpack_from('<i', args, t[0])[0]
                        lot = struct.unpack_from('<i', args, t[1])[0]
                        if ent > 0 and lot > 0:
                            direct_calls.append((ent, lot))
                    # event-1200 RunEvent in common ev0
                    if mapn == 'common' and evid == 0 and iid == 0 and len(args) >= 16:
                        callee = struct.unpack_from('<i', args, 4)[0]
                        if callee == 1200:
                            tf = struct.unpack_from('<i', args, 8)[0]
                            lo = struct.unpack_from('<i', args, 12)[0]
                            if tf > 0 and lo > 0:
                                flag_to_lot[tf] = lo
                # SetEventFlag(target,flag,state=1)  (2003:66 / 2003:69)
                if bank == 2003 and iid in (66, 69) and len(args) >= 12:
                    flag = struct.unpack_from('<i', args, 4)[0]
                    state = struct.unpack_from('<i', args, 8)[0]
                    if state == 1:
                        setter_events.append((flag, mapn, evid))
    direct_lots = {lot for _, lot in direct_calls}
    print(f'\n(A) direct: {len(direct_calls)} calls, {len(direct_lots)} distinct lots')
    print(f'(B) event-1200: {len(flag_to_lot)} flag->lot pairs; '
          f'{sum(1 for f,_,_ in setter_events if f in flag_to_lot)} matching setters')

    # ── entity -> position (all MSB enemy parts) ──
    ent_pos = {}
    for mp in sorted(msb_dir.glob('*.msb.dcx')):
        mi = E.parse_map_name(mp.name)
        if not mi or mi.get('p3') == 99: continue
        try: msb = _read(_msbe_read, SoulsFormats.DCX.Decompress(str(mp)))
        except Exception: continue
        for q in (getattr(msb.Parts, 'Enemies', []) or []):
            if int(getattr(q, 'GameEditionDisable', 0) or 0) == 1: continue
            eid = int(getattr(q, 'EntityID', 0) or 0)
            if eid > 0 and eid not in ent_pos:
                ent_pos[eid] = {'map': mi['map'], 'name': str(q.Name)}
    map_entities = defaultdict(set)
    for eid, info in ent_pos.items():
        map_entities[info['map']].add(eid)

    # ── (B) event-1200 recovery: setter event -> referenced entity -> base lot ──
    # cache event-id -> referenced known entities, per map (only maps with setters)
    setter_maps = {m for _, m, _ in setter_events}
    map_ev_ents = {}
    for mapn in sorted(setter_maps):
        ep = emevd_dir/f'{mapn}.emevd.dcx'
        if not ep.exists(): continue
        try: em = _read(_emevd_read, SoulsFormats.DCX.Decompress(str(ep)))
        except Exception: continue
        valid = map_entities.get(mapn, set())
        for ev in em.Events:
            refs = set()
            for ins in ev.Instructions:
                a = bytes(ins.ArgData) if ins.ArgData else b''
                for i in range(0, len(a)-3, 4):
                    v = struct.unpack_from('<i', a, i)[0]
                    if v in valid: refs.add(v)
            if refs: map_ev_ents[(mapn, int(ev.ID))] = refs
    ev1200_base = {}   # lot -> entity (recovered base lots)
    for flag, mapn, evid in setter_events:
        if flag not in flag_to_lot: continue
        refs = map_ev_ents.get((mapn, evid), set())
        if not refs: continue
        boss = sorted(e for e in refs if 800 <= (e % 1000) <= 899)
        chosen = boss[0] if boss else sorted(refs)[0]
        ev1200_base.setdefault(flag_to_lot[flag], chosen)
    print(f'(B) event-1200 base lots recovered with an entity: {len(ev1200_base)}')

    # treasure base lots (MSB treasure) — the bake stops a MAP-table sibling walk at these.
    treasure_base_lots = set()
    try:
        db = json.load(open('data/items_database.json'))
        treasure_base_lots = {r['itemLotId'] for r in db if r.get('source') == 'treasure'}
    except Exception:
        pass

    # ── (C) sequence siblings — EXACT bake rule (extract_all_items.py:974-997 + 1014-1021):
    #   enemy-table base: walk base+1.. in ItemLotParam_enemy, stop at first gap.
    #   map-table base:   walk base+1.. in ItemLotParam_map, stop at gap OR a treasure base lot.
    #   a sibling is EMITTED only if getItemFlagId > 0 (raw) AND it has a non-empty item.
    def siblings(base):
        out = []
        if base in item_lots_enemy:
            off = 1
            while base + off in item_lots_enemy:
                out.append(base + off); off += 1
                if off > 50: break
        elif base in item_lots_map:
            off = 1
            while True:
                sid = base + off
                if sid in treasure_base_lots: break
                if sid not in item_lots_map: break
                out.append(sid); off += 1
                if off > 20: break
        return out
    sib_to_base = {}
    for base in list(direct_lots) + list(ev1200_base.keys()):
        for s in siblings(base):
            sib_to_base.setdefault(s, base)
    print(f'(C) sequence siblings of direct/ev1200 base lots: {len(sib_to_base)}')

    # ── classify the baked Emevd residual ──
    baked = json.load(open('.scratch/entries.json'))
    baked_emevd = {e['lotId'] for e in baked if e.get('src') == 'Emevd'}
    residual = baked_emevd - direct_lots
    print(f'\n=== 529 baked Emevd: {len(direct_lots & baked_emevd)} direct-covered, '
          f'{len(residual)} residual ===')

    by_ev1200 = {l for l in residual if l in ev1200_base}
    by_sib = {l for l in residual if l in sib_to_base} - by_ev1200
    unrec = residual - by_ev1200 - by_sib
    print(f'  (B) recoverable via event-1200 base+entity: {len(by_ev1200)}')
    print(f'  (C) recoverable via sequence-sibling:       {len(by_sib)}')
    print(f'  NEITHER (still unrecovered):                {len(unrec)}')

    # false-positive / risk gauges
    print('\n=== risk gauges ===')
    # ev1200: how many setters had NO boss-like entity (ambiguous pick)?
    amb = 0
    for flag, mapn, evid in setter_events:
        if flag not in flag_to_lot: continue
        refs = map_ev_ents.get((mapn, evid), set())
        if refs and not any(800 <= (e % 1000) <= 899 for e in refs) and len(refs) > 1:
            amb += 1
    print(f'  ev1200 setters with >1 ref and no boss-like entity (ambiguous): {amb}')
    # sib: longest sibling chain + siblings with a getItemFlagId (real one-time vs clutter)
    chain_len = Counter()
    for base in list(direct_lots) + list(ev1200_base.keys()):
        n = len(siblings(base))
        if n: chain_len[n] += 1
    print(f'  sibling chain-length histogram (base->#siblings): {dict(sorted(chain_len.items()))}')

    # ── mechanism-C emission sizing: what would the runtime sibling scan emit? ──
    # The runtime walks base+offset for EVERY placed base (direct + ev1200), emitting each
    # NON-EMPTY sibling. Measure that universe vs the baked-167 to decide if a notability
    # filter (persistent getItemFlagId, like the enemy pass) is needed for parity.
    def lot_nonempty(lot):
        l = item_lots_enemy.get(lot) or item_lots_map.get(lot)
        if not l: return False
        return any(l.get(f'lotItemId0{s}', 0) > 0 and l.get(f'lotItemCategory0{s}', 0) > 0
                   for s in range(1, 9))
    def lot_flag(lot):
        l = item_lots_enemy.get(lot) or item_lots_map.get(lot)
        return int(l.get('getItemFlagId', 0)) if l else 0
    placed_bases = set(direct_lots) | set(ev1200_base.keys())
    all_sibs = set(sib_to_base.keys()) - placed_bases
    # the bake's exact emit rule: getItemFlagId > 0 (RAW) AND non-empty item
    sib_emit = {s for s in all_sibs if lot_flag(s) > 0 and lot_nonempty(s)}
    print('\n=== mechanism-C emission sizing (exact bake rule: raw flag>0 + non-empty) ===')
    print(f'  total siblings of placed bases (excl bases): {len(all_sibs)}')
    print(f'  ...emitted (flag>0 + non-empty):             {len(sib_emit)}')
    print(f'  baked-167 covered by emitted siblings:       {len(by_sib & sib_emit)} / {len(by_sib)}')
    print(f'  emitted siblings NOT in baked Emevd (over-emit): {len(sib_emit - baked_emevd)}')
    # TRUE new-marker count: a sibling already placed by ANOTHER runtime pass (its lot is an
    # MSB treasure, an enemy-disk lot, or a baked row of ANY source) is dedup'd away, not a flood.
    baked_any = {e['lotId'] for e in baked if e.get('lotId')}
    baked_by_src = defaultdict(set)
    for e in baked:
        if e.get('lotId'): baked_by_src[e.get('src')].add(e['lotId'])
    truly_new = sib_emit - baked_any - direct_lots - set(ev1200_base) - treasure_base_lots
    print(f'  emitted siblings already baked under SOME source: {len(sib_emit & baked_any)}')
    for s in ('Treasure', 'Enemy', 'Emevd', 'Unknown'):
        print(f'      ...as {s}: {len(sib_emit & baked_by_src[s])}')
    print(f'  emitted siblings that are MSB treasure lots:      {len(sib_emit & treasure_base_lots)}')
    print(f'  >>> TRULY NEW markers (not baked anywhere, not a treasure/ev1200/direct base): {len(truly_new)}')
    # what ARE the over-emit markers? Resolve REAL names from item.msgbnd (NOT regulation)
    # and classify by lotItemCategory (1 goods / 2 weapon / 3 armor / 4 talisman / 5 gem) so we
    # can tell "bake forgot real notable gear" from "low-value goods clutter".
    from System.Reflection import Assembly as _A  # noqa
    import SoulsFormats as _SF
    msgbnd = E._read_from_bytes(E._bnd4_read, _SF.DCX.Decompress(str(E.MSGBND_PATH)), '.bnd')
    name_dbs = {
        1: E.read_fmg_names(msgbnd, 'GoodsName.fmg'),
        2: E.read_fmg_names(msgbnd, 'WeaponName.fmg'),
        3: E.read_fmg_names(msgbnd, 'ProtectorName.fmg'),
        4: E.read_fmg_names(msgbnd, 'AccessoryName.fmg'),
        5: E.read_fmg_names(msgbnd, 'GemName.fmg'),
    }
    CATNAME = {1: 'goods', 2: 'weapon', 3: 'armor', 4: 'talisman', 5: 'gem'}
    def first_item(lot):
        l = item_lots_enemy.get(lot) or item_lots_map.get(lot)
        if not l: return (0, 0, '(no-lot)')
        for s in range(1, 9):
            iid = l.get(f'lotItemId0{s}', 0); cat = l.get(f'lotItemCategory0{s}', 0)
            if iid > 0 and cat > 0:
                nm = name_dbs.get(cat, {}).get(iid) or f'cat{cat}_id{iid}'
                return (cat, iid, nm)
        return (0, 0, '(empty)')
    over_by_cat = Counter()
    over_names = Counter()
    equip_over = []   # weapon/armor/talisman/gem the bake skipped (= real notable gear?)
    for s in truly_new:
        cat, iid, nm = first_item(s)
        over_by_cat[CATNAME.get(cat, f'cat{cat}')] += 1
        over_names[nm] += 1
        if cat in (2, 3, 4, 5):
            equip_over.append((s, nm))
    print(f'\n  over-emit ({len(truly_new)}) by lotItemCategory:')
    for cn, c in over_by_cat.most_common():
        print(f'    {c:4} {cn}')
    print(f'  over-emit by item NAME (top 30):')
    for nm, c in over_names.most_common(30):
        print(f'    {c:4} {nm}')
    print(f'\n  EQUIPMENT over-emit (weapon/armor/talisman/gem the bake did NOT show) = {len(equip_over)}:')
    eq_names = Counter(nm for _, nm in equip_over)
    for nm, c in eq_names.most_common(30):
        print(f'    {c:4} {nm}')

    # ── REFINEMENT: restrict C to ev1200 (boss) bases only — does it keep the 162
    #    baked siblings while cutting the direct-template-base over-emit? ──
    sib_ev_only = set()
    for base in ev1200_base:
        for s in siblings(base):
            if lot_flag(s) > 0 and lot_nonempty(s):
                sib_ev_only.add(s)
    sib_ev_only -= placed_bases
    sib_direct_only = set()
    for base in direct_lots:
        for s in siblings(base):
            if lot_flag(s) > 0 and lot_nonempty(s):
                sib_direct_only.add(s)
    sib_direct_only -= placed_bases
    print('\n=== REFINEMENT: ev1200-base siblings vs direct-base siblings ===')
    print(f'  ev1200-base siblings emitted:   {len(sib_ev_only)} '
          f'(covers {len(by_sib & sib_ev_only)}/162 baked, '
          f'over-emit {len(sib_ev_only - baked_any)})')
    print(f'  direct-base siblings emitted:   {len(sib_direct_only)} '
          f'(covers {len(by_sib & sib_direct_only)}/162 baked, '
          f'over-emit {len(sib_direct_only - baked_any)})')

    # samples
    print('\n=== samples ===')
    for tag, s in (('ev1200', by_ev1200), ('sibling', by_sib), ('UNREC', unrec)):
        print(f'  [{tag}] {len(s)}:')
        for lot in sorted(s)[:10]:
            base = ev1200_base.get(lot) or sib_to_base.get(lot)
            nm = lot_item_name(lot, item_lots_map, item_lots_enemy, name_dbs)
            print(f'    lot {lot} item={nm!r} base={base}')


if __name__ == '__main__':
    main()
