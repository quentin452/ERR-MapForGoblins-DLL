#!/usr/bin/env python3
"""BLAST-RADIUS probe for the 4 proposed EMEVD-LOOSE recovery mechanisms.

Before writing any C++, quantify each rule's blast radius (memory's hard lesson: heuristic
emevd rules over-emit — the m60 chest rule matched 395). For each mechanism, report:
  • which of the 7 RECOVERY-CANDIDATE target lots it recovers, and
  • the FULL set of lots it would emit a marker for (size = the blast radius), split into
    target / already-covered-by-a-known-pass / NEW-uncovered.

"covered" = lots a CURRENT runtime pass already places (so emitting them again is a harmless
de-duped double, not a new phantom): MSB Treasure events, NpcParam-enemy refs, the existing
direct-template enemy-join, the current perTileEnemyAward (entity@8/lot@idx(n-2)), and the
boss-chain. A rule is SAFE if its NEW-uncovered emissions are few and each is a real item.

Mechanisms:
  A  ASSET-JOIN on existing templates — add Parts.Assets to the entity→pos map; count how many
     more direct-template lots resolve (Blaidd 90005750 entity@8=asset).
  B  NEW multi-reward templates 90005500/90005501 — entity@20(idx5), lot@8(idx2), asset-join
     (Golden Rune[200] m12, Somber Scadushard m20).
  C  BOSS no-piece base emit — bossFlagLot binds whose base..+8 chain has NO Rune/Ember piece:
     emit the BASE lot (if notable) at the resolved boss/scarab position (Radagon's Scarseal).
  D  PER-TILE lot@last — the 2 Larval Tear per-tile templates put the lot at idx(n-1), not the
     current perTileEnemyAward idx(n-2). Count the blast radius of also accepting lot@last.
"""
import os, struct, tempfile
import extract_all_items as E
import config
from pathlib import Path
from collections import defaultdict, Counter
from System import Array, Object
from System.IO import File as SysFile

TARGETS = {42030000, 1042330100, 1043520500, 1047370100, 1049550700, 12050510, 20010520, 1034500110}
ENT_MIN = 10000
ERR = Path(config.require_err_mod_dir())
MSB_DIR = ERR / 'map' / 'MapStudio'

# ── regulation: ItemLotParam_map/_enemy (notable = getItemFlagId!=0 + a real item), NpcParam refs ──
print("=== regulation ===")
reg = E.SoulsFormats.SFUtil.DecryptERRegulation(str(ERR / 'regulation.bin'))
pdefs = E.load_paramdefs()
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

def notable(lot, tbl):
    """exists in <tbl> with a getItemFlagId + a real item (the runtime lot_row_in_table gate)."""
    r = (ile if tbl == 2 else ilm).get(lot)
    if not r: return False
    if r.get('getItemFlagId', 0) == 0: return False
    return any(r.get(f'lotItemId0{s}', 0) > 0 and r.get(f'lotItemCategory0{s}', 0) > 0 for s in range(1, 9))

# ── MSB: positionable enemy + asset entities; Treasure event lots ──
print("=== MSB (enemy/asset entities, treasure lots) ===")
ent_enemy, ent_asset, treasure_lots = {}, {}, set()
for msb_path in sorted(MSB_DIR.glob('*.msb.dcx')):
    stem = msb_path.name.replace('.msb.dcx', '')
    if stem.endswith('_99'): continue
    try:
        msb = E._read_from_bytes(E._msbe_read, E.SoulsFormats.DCX.Decompress(str(msb_path)), '.msb')
    except Exception:
        continue
    for p in msb.Parts.Enemies:
        if int(getattr(p, 'GameEditionDisable', 0) or 0) == 1: continue
        try: eid = int(p.EntityID)
        except Exception: continue
        if eid and eid not in ent_enemy: ent_enemy[eid] = stem
    for p in msb.Parts.Assets:
        try: eid = int(p.EntityID)
        except Exception: eid = 0
        if eid and eid not in ent_asset: ent_asset[eid] = (stem, str(p.ModelName))
    for ev in msb.Events.Treasures:
        try: treasure_lots.add(int(ev.ItemLotID))
        except Exception: pass

# kEmevdTemplates (runtime) — eventId: (entityOff, lotOff, minLen) in BYTES
TMPL = {90005300:(8,16,20),90005301:(8,16,20),90005860:(16,24,28),90005861:(16,24,28),
        90005880:(16,24,28),90005750:(8,16,20),90005753:(8,16,20),90005774:(8,12,16),
        90005792:(20,24,28),90005632:(8,16,20),90005110:(8,20,24),90005390:(8,28,32),90005555:(8,12,16)}
BOSS = {90005860, 90005861, 90005880}

# ── EMEVD scan: collect every bank-2000 init's ints + tmpl ──
print("=== EMEVD ===")
asm = E.asm
_emevd_read = asm.GetType('SoulsFormats.EMEVD').GetMethod(
    'Read', E.BindingFlags.Public | E.BindingFlags.Static | E.BindingFlags.FlattenHierarchy,
    None, E.Array[E.SysType]([E.SysType.GetType('System.String')]), None)
inits = []  # (file, ints)
for p in sorted((ERR / 'event').glob('*.emevd.dcx')):
    name = p.name.replace('.emevd.dcx', '')
    try:
        tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_bl.tmp')
        SysFile.WriteAllBytes(tmp, E.SoulsFormats.DCX.Decompress(str(p)).ToArray())
        emevd = _emevd_read.Invoke(None, Array[Object]([tmp])); os.unlink(tmp)
    except Exception:
        continue
    for event in emevd.Events:
        for instr in event.Instructions:
            if int(instr.Bank) != 2000: continue
            ab = bytes(instr.ArgData); n = len(ab) // 4
            if n < 2: continue
            inits.append((name, [struct.unpack_from('<i', ab, k*4)[0] for k in range(n)]))

def pos_kind(eid):
    if eid in ent_enemy: return 'enemy'
    if eid in ent_asset: return 'asset'
    return None

# ── baseline coverage: lots a CURRENT pass already places ──
covered = set(treasure_lots) | set(npc_lotrefs)
# current direct enemy-join
for fname, ints in inits:
    n = len(ints); tmpl = ints[1]
    if tmpl in TMPL:
        eo, lo, ml = TMPL[tmpl]
        if n*4 >= ml and lo//4 < n and eo//4 < n:
            ent, lot = ints[eo//4], ints[lo//4]
            if lot > 0 and ent in ent_enemy: covered.add(lot)
    # current perTileEnemyAward: callee>=1e9, entity@8, lot@idx(n-2)
    if tmpl >= 1000000000 and n >= 4:
        ent, lot = ints[2], ints[n-2]
        if ent > 0 and lot > 0 and ent != lot and ent in ent_enemy: covered.add(lot)

def report(title, emit):
    """emit = dict lot -> detail. Split target / covered / new. The runtime emits a marker
    ONLY for a notable lot (lot_row_in_table gate) → new∩notable is the REAL new-marker count."""
    tgt = sorted(l for l in emit if l in TARGETS)
    new = sorted(l for l in emit if l not in TARGETS and l not in covered)
    cov = sorted(l for l in emit if l not in TARGETS and l in covered)
    new_notable = [l for l in new if notable(l, 1) or notable(l, 2)]
    print(f"\n### {title}")
    print(f"   emits {len(emit)} lots:  {len(tgt)} TARGET · {len(cov)} already-covered (dedup) · "
          f"{len(new)} NEW-uncovered ({len(new_notable)} of them NOTABLE = real new markers)")
    print(f"   targets recovered: {[E_short(l, emit[l]) for l in tgt]}")
    if new_notable:
        print(f"   NEW-uncovered & NOTABLE ({len(new_notable)}) — these become real new markers:")
        for l in new_notable[:50]:
            print(f"      lot={l}  {emit[l]}")
        if len(new_notable) > 50: print(f"      … +{len(new_notable)-50} more")

def E_short(l, d): return f"{l}({d})"

# ── A. ASSET-JOIN on existing templates ──
emitA = {}
for fname, ints in inits:
    n = len(ints); tmpl = ints[1]
    if tmpl not in TMPL: continue
    eo, lo, ml = TMPL[tmpl]
    if n*4 < ml or lo//4 >= n or eo//4 >= n: continue
    ent, lot = ints[eo//4], ints[lo//4]
    if lot <= 0: continue
    if ent in ent_enemy: continue            # already covered by enemy-join (baseline)
    if ent in ent_asset:                     # NEW: resolves only via asset
        emitA[lot] = f"T{tmpl} asset@{eo} {ent_asset[ent][1]}@{ent_asset[ent][0]}"
report("A. ASSET-JOIN on existing templates (entity resolves as an ASSET)", emitA)

# ── B. NEW multi-reward templates 90005500/501: entity@20(idx5), lot@8(idx2) ──
emitB = {}
for fname, ints in inits:
    n = len(ints); tmpl = ints[1]
    if tmpl not in (90005500, 90005501) or n < 6: continue
    lot, ent = ints[2], ints[5]
    if lot <= 0: continue
    k = pos_kind(ent)
    emitB[lot] = f"T{tmpl} @{fname} ent={ent}({k})"
report("B. NEW templates 90005500/501 (lot@8, anchor@20)", emitB)

# ── C. BOSS no-piece base emit ──
# replicate the bossFlagLot resolve: chosen = flag if enemy else entity16 if positionable.
emitC = {}
for fname, ints in inits:
    n = len(ints); tmpl = ints[1]
    if tmpl not in BOSS or n < 7: continue
    flag, ent16, base = ints[2], ints[4], ints[6]
    if base <= 0: continue
    chosen = flag if flag in ent_enemy else (ent16 if pos_kind(ent16) else 0)
    if not chosen: continue
    # is base itself a Rune/Ember piece row? (then the piece path emits it, not the base-emit)
    base_is_piece = False
    r0 = ilm.get(base)
    if r0:
        for s in range(1, 9):
            if r0.get(f'lotItemCategory0{s}', 0) == 1 and r0.get(f'lotItemId0{s}', 0) in (800010, 850010):
                base_is_piece = True
    if base_is_piece: continue
    if not (notable(base, 1) or notable(base, 2)): continue
    emitC[base] = f"T{tmpl} chosen={chosen}({pos_kind(chosen)}) @{fname}"
report("C. BOSS no-piece base-lot emit (notable base, no piece in chain)", emitC)

# ── D. PER-TILE lot@last (idx n-1) in addition to current idx(n-2) ──
emitD = {}
for fname, ints in inits:
    n = len(ints); tmpl = ints[1]
    if tmpl < 1000000000 or n < 4: continue
    ent = ints[2]
    lot_last = ints[n-1]
    if lot_last > 0 and ent in ent_enemy and ent != lot_last:
        emitD[lot_last] = f"t{tmpl} ent={ent}(enemy) @{fname}"
report("D. PER-TILE lot@last (idx n-1), entity@8 enemy", emitD)

# ── E. LOOSE-ANCHOR fallback for direct templates + boss: documented entity fails →
#       use the NEAREST positionable 4-byte window (enemy preferred, else asset). Lot at the
#       template's lotOff (boss: base@24). Notability gate applied by report(). ──
emitE = {}
for fname, ints in inits:
    n = len(ints); tmpl = ints[1]
    is_boss = tmpl in BOSS
    if tmpl in TMPL and not is_boss:
        eo, lo, ml = TMPL[tmpl]
        if n*4 < ml or lo//4 >= n or eo//4 >= n: continue
        ent, lot = ints[eo//4], ints[lo//4]
        lotidx = lo//4
    elif is_boss and n >= 7:
        ent, lot, lotidx = ints[4], ints[6], 6        # ent16, base@24
        flag = ints[2]
        if flag in ent_enemy or pos_kind(ent): continue  # already resolvable (handled elsewhere)
    else:
        continue
    if lot <= 0: continue
    if pos_kind(ent): continue                  # documented entity already resolves → not loose
    # nearest positionable window to the lot index (enemy preferred)
    best = None; bestd = 99
    for i, v in enumerate(ints):
        if i == lotidx: continue
        k = pos_kind(v)
        if not k: continue
        d = abs(i - lotidx) - (0.5 if k == 'enemy' else 0)  # tie-break enemy
        if d < bestd: bestd, best = d, (v, k)
    if not best: continue
    emitE[lot] = f"T{tmpl} loose {best[1]} {best[0]} @{fname}"
report("E. LOOSE-ANCHOR fallback (documented entity fails → nearest positionable window)", emitE)

print("\n=== which targets does each mechanism cover? ===")
for l in sorted(TARGETS):
    where = []
    if l in emitA: where.append('A')
    if l in emitB: where.append('B')
    if l in emitC: where.append('C')
    if l in emitD: where.append('D')
    if l in emitE: where.append('E')
    print(f"  {l:<11} -> {where or 'NONE'}")
