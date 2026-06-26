#!/usr/bin/env python3
"""Per-tile EMEVD award rule "lot@idx(n-1)" blast-radius — REAL items vs over-emit phantoms?

The 2 deferred Larval Tears (1047370100 m60_47_37, 1049550700 m60_49_55) are awarded by per-tile
templates (callee>=1e9) whose lot sits at the LAST arg (idx n-1), with idx(n-2)=constant 1. The
shipped perTileEnemyAward reads lot@idx(n-2)=1 (junk) → misses them. This probe tests a rule that
recovers them — lot@idx(n-1), anchor = the nearest positionable ENEMY (asset optionally) — and
RESOLVES EVERY collateral lot to its FMG item name + anchor enemy model so we can judge each as a
real drop or a phantom (the memory's question: over-emit bug vs real coverage).

"covered" baseline = lots a current pass already places (treasure / NpcParam / direct-template
enemy-join / the SHIPPED perTile idx(n-2) rule). NEW-notable = what the new rule would add.
"""
import os, struct, tempfile
import extract_all_items as E
import config
from pathlib import Path
from collections import defaultdict, Counter
from System import Array, Object
from System.IO import File as SysFile

TARGETS = {1047370100, 1049550700}
ERR = Path(config.require_err_mod_dir())
MSB_DIR = ERR / 'map' / 'MapStudio'

print("=== regulation ===")
reg = E.SoulsFormats.SFUtil.DecryptERRegulation(str(ERR / 'regulation.bin'))
pdefs = E.load_paramdefs()
lf = {'getItemFlagId'}
for i in range(1, 9): lf.update([f'lotItemId0{i}', f'lotItemCategory0{i}'])
ilm = E.param_to_dict(E.read_param(reg, 'ItemLotParam_map', pdefs), lf)
ile = E.param_to_dict(E.read_param(reg, 'ItemLotParam_enemy', pdefs), lf)
npc = E.param_to_dict(E.read_param(reg, 'NpcParam', pdefs), {'itemLotId_map', 'itemLotId_enemy'})
npc_lotrefs = {f[k] for f in npc.values() for k in ('itemLotId_map', 'itemLotId_enemy') if f.get(k, 0) > 0}

msg = None
try:
    tmp = os.path.join(tempfile.gettempdir(), f"mfg_msg_{os.getpid()}.bnd")
    SysFile.WriteAllBytes(tmp, E.SoulsFormats.DCX.Decompress(str(E.MSGBND_PATH)).ToArray())
    msg = E._bnd4_read.Invoke(None, Array[Object]([tmp])); os.unlink(tmp)
    name_dbs = {1: E.read_fmg_names(msg, 'GoodsName.fmg'), 2: E.read_fmg_names(msg, 'WeaponName.fmg'),
                3: E.read_fmg_names(msg, 'ProtectorName.fmg'), 4: E.read_fmg_names(msg, 'AccessoryName.fmg'),
                5: E.read_fmg_names(msg, 'GemName.fmg')}
except Exception:
    name_dbs = {}

def item_name(lot):
    r = ilm.get(lot) or ile.get(lot)
    if not r: return '(lot absent)'
    for s in range(1, 9):
        iid, cat = r.get(f'lotItemId0{s}', 0), r.get(f'lotItemCategory0{s}', 0)
        if iid > 0 and cat > 0:
            return name_dbs.get(cat, {}).get(iid, f'cat{cat}#{iid}')
    return '(empty lot)'

def notable(lot):
    for tbl in (ilm, ile):
        r = tbl.get(lot)
        if r and r.get('getItemFlagId', 0) != 0 and any(
                r.get(f'lotItemId0{s}', 0) > 0 and r.get(f'lotItemCategory0{s}', 0) > 0 for s in range(1, 9)):
            return True
    return False

print("=== MSB enemy/asset entities + treasure lots ===")
ent_enemy, ent_asset, treasure_lots = {}, {}, set()
for mp in sorted(MSB_DIR.glob('*.msb.dcx')):
    stem = mp.name.replace('.msb.dcx', '')
    if stem.endswith('_99'): continue
    try: msb = E._read_from_bytes(E._msbe_read, E.SoulsFormats.DCX.Decompress(str(mp)), '.msb')
    except Exception: continue
    for p in msb.Parts.Enemies:
        if int(getattr(p, 'GameEditionDisable', 0) or 0) == 1: continue
        try: eid = int(p.EntityID)
        except Exception: continue
        if eid and eid not in ent_enemy: ent_enemy[eid] = (stem, str(p.ModelName))
    for p in msb.Parts.Assets:
        try: eid = int(p.EntityID)
        except Exception: eid = 0
        if eid and eid not in ent_asset: ent_asset[eid] = (stem, str(p.ModelName))
    for ev in msb.Events.Treasures:
        try: treasure_lots.add(int(ev.ItemLotID))
        except Exception: pass

TMPL = {90005300,90005301,90005860,90005861,90005880,90005750,90005753,90005774,90005792,
        90005632,90005110,90005390,90005555,90005500,90005501}
TOFF = {90005300:(8,16,20),90005301:(8,16,20),90005860:(16,24,28),90005861:(16,24,28),
        90005880:(16,24,28),90005750:(8,16,20),90005753:(8,16,20),90005774:(8,12,16),
        90005792:(20,24,28),90005632:(8,16,20),90005110:(8,20,24),90005390:(8,28,32),
        90005555:(8,12,16),90005500:(20,8,24),90005501:(20,8,24)}

print("=== EMEVD ===")
asm = E.asm
_emevd_read = asm.GetType('SoulsFormats.EMEVD').GetMethod(
    'Read', E.BindingFlags.Public | E.BindingFlags.Static | E.BindingFlags.FlattenHierarchy,
    None, E.Array[E.SysType]([E.SysType.GetType('System.String')]), None)
inits = []
for p in sorted((ERR / 'event').glob('*.emevd.dcx')):
    name = p.name.replace('.emevd.dcx', '')
    try:
        tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_pl.tmp')
        SysFile.WriteAllBytes(tmp, E.SoulsFormats.DCX.Decompress(str(p)).ToArray())
        emevd = _emevd_read.Invoke(None, Array[Object]([tmp])); os.unlink(tmp)
    except Exception: continue
    for ev in emevd.Events:
        for ins in ev.Instructions:
            if int(ins.Bank) != 2000: continue
            ab = bytes(ins.ArgData); n = len(ab)//4
            if n < 2: continue
            inits.append((name, [struct.unpack_from('<i', ab, k*4)[0] for k in range(n)]))

def pkind(v):
    if v in ent_enemy: return ('enemy', ent_enemy[v][1])
    if v in ent_asset: return ('asset', ent_asset[v][1])
    return (None, None)

# baseline coverage
covered = set(treasure_lots) | set(npc_lotrefs)
for fn, ints in inits:
    n = len(ints); t = ints[1]
    if t in TOFF:
        eo, lo, ml = TOFF[t]
        if n*4 >= ml and lo//4 < n and eo//4 < n and ints[lo//4] > 0 and ints[eo//4] in ent_enemy:
            covered.add(ints[lo//4])
    if t >= 1000000000 and n >= 4:                       # shipped perTile: entity@8, lot@idx(n-2)
        if ints[2] > 0 and ints[n-2] > 0 and ints[2] != ints[n-2] and ints[2] in ent_enemy:
            covered.add(ints[n-2])

# RULE under test: per-tile callee>=1e9, lot@idx(n-1), nearest positionable ENEMY anchor
emit = {}   # lot -> (anchor_kind, anchor_model, anchor_eid, tile, file)
for fn, ints in inits:
    n = len(ints); t = ints[1]
    if t < 1000000000 or n < 4: continue
    lot = ints[n-1]
    if lot <= 0: continue
    lotidx = n-1
    best, bestd = None, 99
    for i, v in enumerate(ints):
        if i == lotidx or i == 1: continue
        k, model = pkind(v)
        if k != 'enemy': continue          # ENEMY-only anchor (asset would re-open the chest over-match)
        d = abs(i - lotidx)
        if d < bestd: bestd, best = d, (k, model, v)
    if not best: continue
    emit[lot] = (*best, ints[0] if False else fn)

tgt = sorted(l for l in emit if l in TARGETS)
new = sorted(l for l in emit if l not in TARGETS and l not in covered)
cov = sorted(l for l in emit if l not in TARGETS and l in covered)
new_notable = [l for l in new if notable(l)]
print(f"\n=== RULE: per-tile lot@idx(n-1), nearest ENEMY anchor ===")
print(f"emits {len(emit)} lots: {len(tgt)} TARGET · {len(cov)} already-covered · "
      f"{len(new)} NEW-uncovered ({len(new_notable)} NOTABLE)")
print(f"targets recovered: {tgt}")
print(f"\n--- the 2 TARGETS ---")
for l in sorted(TARGETS):
    if l in emit:
        k, m, eid, fn = emit[l]
        print(f"  lot={l} '{item_name(l)}' anchor={k} {m} eid={eid} @{fn}")
    else:
        print(f"  lot={l} NOT recovered by this rule")
print(f"\n--- NEW-uncovered & NOTABLE collateral ({len(new_notable)}) — REAL item vs phantom? ---")
model_tally = Counter()
for l in new_notable:
    k, m, eid, fn = emit[l]
    model_tally[m] += 1
    print(f"  lot={l:<11} '{item_name(l)}'  via {k} {m} eid={eid} @{fn}")
print(f"\n--- anchor enemy-model tally over the NEW-notable collateral ---")
for m, c in model_tally.most_common():
    print(f"  {c:>3}  {m}")
print(f"\n(non-notable NEW = {len(new)-len(new_notable)} — filtered by the lot_row_in_table gate at runtime)")
