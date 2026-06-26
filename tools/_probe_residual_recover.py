#!/usr/bin/env python3
"""RESIDUAL RECOVERY ORACLE — automated, run-everywhere "is this residual recoverable?" sweep.

Generalizes the per-category one-off probes (_probe_unknown_chests / _probe_resid_shop /
_probe_treasure_part / _probe_boss_piece_entity / _probe_emevd_join) into ONE pass: for every
surviving baked-loot [RESIDUAL-ROW] (diag_loot_pos dump), cross-reference the lot against EVERY
known disk placement mechanism and print a verdict. The recurring no-bake lesson is that an
"accepted/irreducible" residual is usually a disk-parse gap — a position the OFFLINE bake reads
that a runtime disk pass doesn't (the bossEntity@16 case, the treasure sibling-walk, the merchant
phantoms). This oracle finds the next such gap automatically instead of by hand, per category.

Per lot it checks, in priority order:
  • SHOP-INF      — item sold infinite-stock (sellQuantity=-1) in ShopLineupParam → merchant phantom, DROP
  • ASSET         — a placed Parts.Asset whose AEG pickUpItemLotParamId == lot (chest/ground pickup)
  • TREASURE      — an MSB Events.Treasure references this lot (the treasure pass should place it)
  • NPC-ENEMY     — an NpcParam refs this lot (enemy drop) AND the npc's MSB enemy is positionable
  • EMEVD-ENTITY  — a bank-2000 init carries this lot AND a positionable MSB entity in the SAME blob
                    (the generic scripted-award anchor; sub-tagged by template family)
  • BOSS-CHAIN    — lot lies in a 90005860/61/80 baseLot..+8 chain (already covered post-entity16 fix)
None of the above → IRREDUCIBLE (no readable world position on disk: a bake (0,0,0) fallback record,
an orphan ItemLotParam_enemy with no NpcParam/EMEVD, or a value-arg per-tile template we can't anchor).

A lot tagged anything but IRREDUCIBLE/SHOP-INF is a RECOVERY CANDIDATE — re-open it (the position is
on disk). SHOP-INF should already be dropped by drop_merchant_phantoms; if one shows up residual the
live ShopLineupParam read disagrees with the offline one (investigate).

Usage: py _probe_residual_recover.py [log]   (default log = the offline deploy dir)
"""
import sys, os, re, struct, tempfile
import extract_all_items as E
import config
from pathlib import Path
from collections import Counter, defaultdict
from System import Array, Object
from System.IO import File as SysFile

DEFAULT_LOG = Path(r"D:\DOWNLOAD\ERRv2.2.9.6-541-2-2-9-6-1780861369\ERRv2.2.9.6\dll\offline\logs\MapForGoblins.log")
log = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_LOG
ROW = re.compile(r'\[RESIDUAL-ROW\] cat="(?P<cat>[^"]+)" src=(?P<src>\w+) lot=(?P<lot>\d+) '
                 r'lt=(?P<lt>\d+) m(?P<a>\d+)_(?P<gx>\d+)_(?P<gz>\d+) key=(?P<key>-?\d+)')
rows = {}
for line in open(log, encoding='utf-8', errors='replace'):
    m = ROW.search(line)
    if m:
        rows[int(m.group('lot'))] = dict(cat=m.group('cat'), src=m.group('src'), lt=int(m.group('lt')),
                                         tile=f"m{m.group('a')}_{m.group('gx')}_{m.group('gz')}")
if not rows:
    sys.exit("no [RESIDUAL-ROW] lines — run the game with diag_loot_pos=true + open the map.")
targets = set(rows)
print(f"[{len(targets)} residual lots from {log.name}]")

ERR = Path(config.require_err_mod_dir())
MSB_DIR = ERR / 'map' / 'MapStudio'
print("=== loading regulation / FMG / MSB / EMEVD ===")
reg = E.SoulsFormats.SFUtil.DecryptERRegulation(str(ERR / 'regulation.bin'))
pdefs = E.load_paramdefs()

# ── ItemLotParam (item name + getItemFlagId), NpcParam refs, AEG pickups, Shop ──
lot_fields = {'getItemFlagId'}
for i in range(1, 9):
    lot_fields.update([f'lotItemId0{i}', f'lotItemCategory0{i}'])
ilm = E.param_to_dict(E.read_param(reg, 'ItemLotParam_map', pdefs), lot_fields)
ile = E.param_to_dict(E.read_param(reg, 'ItemLotParam_enemy', pdefs), lot_fields)
npc = E.param_to_dict(E.read_param(reg, 'NpcParam', pdefs), {'itemLotId_map', 'itemLotId_enemy'})
npc_lotrefs = set()
for f in npc.values():
    for k in ('itemLotId_map', 'itemLotId_enemy'):
        if f.get(k, 0) > 0: npc_lotrefs.add(f[k])
aeg = E.param_to_dict(E.read_param(reg, 'AssetEnvironmentGeometryParam', pdefs), {'pickUpItemLotParamId'})
aegrow_to_lot = {rid: f.get('pickUpItemLotParamId', 0) for rid, f in aeg.items()}
pickup_lots = {l for l in aegrow_to_lot.values() if l in targets}
shop = E.param_to_dict(E.read_param(reg, 'ShopLineupParam', pdefs), {'equipId', 'equipType', 'sellQuantity'})
ET = {2: 0, 3: 1, 4: 2, 1: 3}  # lotItemCategory → ShopLineupParam.equipType
sold_inf, sold_any = set(), set()
for f in shop.values():
    k = (f.get('equipType', -99), f.get('equipId', 0))
    sold_any.add(k)
    if f.get('sellQuantity', 0) == -1: sold_inf.add(k)

msg = None
try:
    tmp = os.path.join(tempfile.gettempdir(), f"mfg_msg_{os.getpid()}.bnd")
    SysFile.WriteAllBytes(tmp, E.SoulsFormats.DCX.Decompress(str(E.MSGBND_PATH)).ToArray())
    msg = E._bnd4_read.Invoke(None, Array[Object]([tmp]))
    try: os.unlink(tmp)
    except OSError: pass
    name_dbs = {1: E.read_fmg_names(msg, 'GoodsName.fmg'), 2: E.read_fmg_names(msg, 'WeaponName.fmg'),
                3: E.read_fmg_names(msg, 'ProtectorName.fmg'), 4: E.read_fmg_names(msg, 'AccessoryName.fmg'),
                5: E.read_fmg_names(msg, 'GemName.fmg')}
except Exception:
    name_dbs = {}

def item_name(lot, lt):
    tbl = ile if lt == 2 else ilm
    r = tbl.get(lot) or ilm.get(lot) or ile.get(lot)
    if not r: return ('(lot absent)', None)
    for s in range(1, 9):
        iid, cat = r.get(f'lotItemId0{s}', 0), r.get(f'lotItemCategory0{s}', 0)
        if iid > 0 and cat > 0:
            return (name_dbs.get(cat, {}).get(iid, f'?{iid}'), (cat, iid))
    return ('(empty lot)', None)

# ── MSB pass: enemy/asset entities (positionable, _00 + LOD), treasure lots, asset pickups ──
ent_pos = {}          # positionable EntityID → (tile, kind)
asset_pickup_hits = defaultdict(list)  # lot → [(tile, model, name)]
treasure_lots = {}    # lot → tile  (only targets)
for msb_path in sorted(MSB_DIR.glob('*.msb.dcx')):
    stem = msb_path.name.replace('.msb.dcx', '')
    if stem.endswith('_99'):
        continue
    try:
        msb = E._read_from_bytes(E._msbe_read, E.SoulsFormats.DCX.Decompress(str(msb_path)), '.msb')
    except Exception:
        continue
    for p in msb.Parts.Enemies:
        if int(getattr(p, 'GameEditionDisable', 0) or 0) == 1:
            continue
        try: eid = int(p.EntityID)
        except Exception: continue
        if eid and eid not in ent_pos: ent_pos[eid] = (stem, 'enemy')
    for p in msb.Parts.Assets:
        try: eid = int(p.EntityID)
        except Exception: eid = 0
        if eid and eid not in ent_pos: ent_pos[eid] = (stem, 'asset')
        model = str(p.ModelName)
        if model.startswith('AEG'):
            try:
                a, b = model[3:].split('_'); aegRow = int(a) * 1000 + int(b)
            except Exception:
                aegRow = -1
            lot = aegrow_to_lot.get(aegRow, 0)
            if lot in targets:
                asset_pickup_hits[lot].append((stem, model, str(p.Name)))
    for ev in msb.Events.Treasures:
        try: lid = int(ev.ItemLotID)
        except Exception: continue
        if lid in targets: treasure_lots[lid] = stem

# ── EMEVD pass: for each bank-2000 init, if it carries a target lot, record the file +
# every positionable entity-range value in the SAME blob + the template id (arg @4) ──
# kEmevdTemplates: eventId → (entityArgOff, lotArgOff) in BYTES (== extract_all_items TEMPLATE_EVENTS).
# A KNOWN-template hit is only trusted if the lot sits at lotOff AND a positionable entity sits at
# entityOff — loose blob co-occurrence (lot + some unrelated entity in the same init) is NOT a bind.
KNOWN_TMPL = {90005300: (8, 16), 90005301: (8, 16), 90005860: (16, 24), 90005861: (16, 24),
              90005880: (16, 24), 90005750: (8, 16), 90005753: (8, 16), 90005774: (8, 12),
              90005792: (20, 24), 90005632: (8, 16), 90005110: (8, 20), 90005390: (8, 28),
              90005555: (8, 12)}
ENT_MIN = 10000  # below this an "entity" is a literal arg (flag/count/state), not a placeable id
asm = E.asm
_emevd_read = asm.GetType('SoulsFormats.EMEVD').GetMethod(
    'Read', E.BindingFlags.Public | E.BindingFlags.Static | E.BindingFlags.FlattenHierarchy,
    None, E.Array[E.SysType]([E.SysType.GetType('System.String')]), None)
emevd_hits = defaultdict(list)   # lot → [(file, tmpl, [positionable ents in blob])]
boss_bases = []                  # (baseLot, entity@16) for the boss-chain check
for p in sorted((ERR / 'event').glob('*.emevd.dcx')):
    name = p.name.replace('.emevd.dcx', '')
    try:
        tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_rr.tmp')
        SysFile.WriteAllBytes(tmp, E.SoulsFormats.DCX.Decompress(str(p)).ToArray())
        emevd = _emevd_read.Invoke(None, Array[Object]([tmp])); os.unlink(tmp)
    except Exception:
        continue
    for event in emevd.Events:
        for instr in event.Instructions:
            if int(instr.Bank) != 2000:
                continue
            ab = bytes(instr.ArgData)
            n = len(ab) // 4
            if n < 2:
                continue
            ints = [struct.unpack_from('<i', ab, k * 4)[0] for k in range(n)]
            tmpl = ints[1]
            if tmpl in (90005860, 90005861, 90005880) and n >= 7:
                boss_bases.append((ints[6], ints[4]))  # baseLot@24=idx6, entity@16=idx4
            hit_lots = [v for v in ints if v in targets]
            if not hit_lots:
                continue
            # positionable entities in the blob (≥ENT_MIN, in an MSB part) — the loose anchor
            pos_ents = sorted({v for v in ints if v >= ENT_MIN and v in ent_pos})
            # bound = the KNOWN template actually has the lot at lotOff and a placeable entity at
            # entityOff (the strict, trustworthy bind — what a runtime template pass would read)
            bound_ent = 0
            if tmpl in KNOWN_TMPL:
                eo, lo = KNOWN_TMPL[tmpl]
                if lo // 4 < n and eo // 4 < n and ints[lo // 4] in targets:
                    e = ints[eo // 4]
                    if e >= ENT_MIN and e in ent_pos:
                        bound_ent = e
            for v in hit_lots:
                emevd_hits[v].append((name, tmpl, pos_ents, bound_ent if ints[KNOWN_TMPL.get(tmpl, (0, 0))[1] // 4] == v else 0))

def boss_chain_lot(lot):
    for base, ent in boss_bases:
        if 0 <= lot - base <= 8 and ent in ent_pos:
            return (base, ent)
    return None

# ── classify each residual ──
print(f"\n{'LOT':<11} {'lt':<3} {'cat':<24} VERDICT  item / anchor")
verdict_tally = Counter()
recover = []
for lot in sorted(targets, key=lambda l: (rows[l]['cat'], l)):
    r = rows[lot]
    nm, itemkey = item_name(lot, r['lt'])
    shop_inf = itemkey and (ET.get(itemkey[0]), itemkey[1]) in sold_inf
    shop_fin = itemkey and not shop_inf and (ET.get(itemkey[0]), itemkey[1]) in sold_any
    ah = asset_pickup_hits.get(lot)
    tr = treasure_lots.get(lot)
    npcref = lot in npc_lotrefs
    eh = emevd_hits.get(lot)
    eh_bound = [h for h in (eh or []) if h[3]]          # KNOWN template binds lot↔entity (strict)
    eh_pos = [h for h in (eh or []) if h[2]]            # positionable entity merely in the same blob
    bc = boss_chain_lot(lot)

    if shop_inf:
        v, detail = 'DROP-SHOP-INF', 'sold ∞ in shop (merchant phantom)'
    elif ah:
        v, detail = 'RECOVER-ASSET', f'pickup {ah[0][1]} @{ah[0][0]} ({ah[0][2]})'
    elif tr:
        v, detail = 'RECOVER-TREASURE', f'MSB Treasure event @{tr}'
    elif bc:
        v, detail = 'BOSS-CHAIN', f'baseLot {bc[0]} entity {bc[1]} (covered post-entity16)'
    elif eh_bound:
        e = eh_bound[0][3]; etile, ekind = ent_pos[e]
        # the runtime emevd pass joins entity→ENEMIES only; an asset-anchored award (e.g. a seal you
        # examine) needs an asset-join extension, a broader lever than the enemy-only direct pass.
        v = 'RECOVER-EMEVD-BOUND' if ekind == 'enemy' else 'EMEVD-BOUND-ASSET'
        detail = (f'T{eh_bound[0][1]} binds lot↔{ekind} {e} @{etile}'
                  + (' (template enemy-join should place it)' if ekind == 'enemy'
                     else ' — entity is an ASSET, runtime enemy-join cannot reach it (needs asset-join)'))
    elif eh_pos:
        fams = sorted({('T%d' % h[1]) if h[1] in KNOWN_TMPL else ('t%d' % h[1]) for h in eh_pos})
        ent0 = eh_pos[0][2][0]; etile, ekind = ent_pos[ent0]
        v, detail = 'RECOVER-EMEVD-LOOSE?', (f'init {fams}, positionable {ekind} {ent0} @{etile} in '
                                             f'blob but NOT template-bound — verify offsets before trusting')
    elif npcref:
        v, detail = 'RECOVER-NPC?', 'NpcParam refs lot (enemy) — but no positionable MSB entity found'
    else:
        bits = []
        if eh: bits.append(f'emevd-ref(no positionable ent, tmpl={sorted({h[1] for h in eh})})')
        if shop_fin: bits.append('finite-shop')
        if r['src'] == 'enemy' and not npcref: bits.append('orphan-enemy(no NpcParam)')
        v, detail = 'IRREDUCIBLE', '; '.join(bits) or 'no MSB/EMEVD/asset/shop placement'
    verdict_tally[v] += 1
    if v.startswith('RECOVER') or v == 'BOSS-CHAIN':
        recover.append((lot, r, nm, v, detail))
    print(f"{lot:<11} {r['lt']:<3} {r['cat']:<24} {v:<20} {nm} | {detail}")

print("\n=== verdict tally ===")
for v, n in verdict_tally.most_common():
    print(f"  {n:>3}  {v}")
if recover:
    print(f"\n*** {len(recover)} RECOVERY CANDIDATE(S) — position IS on disk, re-open: ***")
    for lot, r, nm, v, detail in recover:
        print(f"  {v:<22} lot={lot} {r['cat']} ({nm}) — {detail}")
else:
    print("\nNo recovery candidates: every residual is shop-drop or has no readable world position.")
