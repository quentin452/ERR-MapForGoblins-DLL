#!/usr/bin/env python3
"""By-FAMILY orphan-lot batch probe of the baked-loot residual.

Ingests the runtime [RESIDUAL-ROW] dump (diag_loot_pos) and classifies every surviving baked
loot row of a category FAMILY (e.g. "Equipment" = Armaments+Armour+Ashes+Spirits+Talismans)
against the deployed regulation — to tell, in one batch, which residuals are the confirmed
mis-label bug (orphan enemy-lot, no NpcParam), which are the real treasure debake-gap, and
which (if any) are genuinely recoverable.

Usage:  python _probe_residual_family.py [Equipment] [path\\to\\MapForGoblins.log]
        (no family arg → every family)
"""
import sys, re
import extract_all_items as E   # SoulsFormats bootstrap + helpers (sets utf-8 stdout)
import config
from pathlib import Path

DEFAULT_LOG = Path(r"D:\DOWNLOAD\ERRv2.2.9.6-541-2-2-9-6-1780861369\ERRv2.2.9.6\dll\offline\logs\MapForGoblins.log")
ROW = re.compile(r'\[RESIDUAL-ROW\] cat="(?P<cat>[^"]+)" src=(?P<src>\w+) lot=(?P<lot>\d+) '
                 r'lt=(?P<lt>\d+) m(?P<a>\d+)_(?P<gx>\d+)_(?P<gz>\d+) key=(?P<key>-?\d+)')

fam_arg = None
log = DEFAULT_LOG
for a in sys.argv[1:]:
    if a.lower().endswith('.log') or '\\' in a or '/' in a:
        log = Path(a)
    else:
        fam_arg = a

# Parse the LAST [RESIDUAL-ROW] block (one dump per map build → keep the most recent per lot).
rows = {}
for line in open(log, encoding='utf-8', errors='replace'):
    m = ROW.search(line)
    if m:
        rows[int(m.group('lot'))] = dict(cat=m.group('cat'), src=m.group('src'),
                                         lt=int(m.group('lt')), tile=f"m{m.group('a')}_{m.group('gx')}_{m.group('gz')}",
                                         key=int(m.group('key')))
if not rows:
    sys.exit("no [RESIDUAL-ROW] lines in the log — run with diag_loot_pos=true + open the map.")


def family(cat):  # "Equipment - Armaments" -> "Equipment"
    return cat.split(' - ', 1)[0].split(' -', 1)[0].strip()


sel = {lot: r for lot, r in rows.items() if not fam_arg or family(r['cat']).lower() == fam_arg.lower()}
print(f"[{len(sel)} residual rows" + (f" in family '{fam_arg}'" if fam_arg else " (all families)") +
      f", from {len(rows)} total]\n")

# Build regulation reverse sets ONCE.
reg = E.SoulsFormats.SFUtil.DecryptERRegulation(str(Path(config.ERR_MOD_DIR) / 'regulation.bin'))
pdefs = E.load_paramdefs()
npc = E.param_to_dict(E.read_param(reg, 'NpcParam', pdefs), {'itemLotId_map', 'itemLotId_enemy'})
npc30, npc34 = set(), set()
for f in npc.values():
    if f.get('itemLotId_enemy', 0) > 0: npc30.add(f['itemLotId_enemy'])
    if f.get('itemLotId_map', 0) > 0: npc34.add(f['itemLotId_map'])
ile = set(int(r.ID) for r in E.read_param(reg, 'ItemLotParam_enemy', pdefs).Rows)
ilm = set(int(r.ID) for r in E.read_param(reg, 'ItemLotParam_map', pdefs).Rows)


def verdict(lot, r):
    npcref = lot in npc30 or lot in npc34
    in_e, in_m = lot in ile, lot in ilm
    if npcref:
        return "REAL enemy drop (NpcParam refs it — why uncovered?)"
    if not in_e and not in_m:
        return "STALE: lot row gone from BOTH ItemLotParam tables"
    if r['src'] == 'enemy':
        return "MIS-LABEL bug (orphan enemy-lot, no NpcParam)" if in_e else "mislabel→map-lot only"
    if r['src'] == 'treasure':
        return "treasure debake-gap (real map-lot, corpse loot)" if in_m else "treasure but enemy-table only (suspicious)"
    # unknown / emevd
    where = ("enemy-table" if in_e else "") + ("+map-table" if in_m else "")
    return f"{r['src']}: in {where} (no NpcParam)"


from collections import Counter
by_fam = {}
for lot, r in sorted(sel.items(), key=lambda kv: (family(kv[1]['cat']), kv[1]['cat'], kv[0])):
    fam = family(r['cat'])
    v = verdict(lot, r)
    by_fam.setdefault(fam, Counter())[v] += 1
    print(f"  {fam:<11} {r['cat']:<26} {r['src']:<8} lot={lot:<10} lt={r['lt']} {r['tile']:<11} "
          f"key={r['key']:<10} -> {v}")

print("\n=== per-family verdict tally ===")
for fam, c in by_fam.items():
    print(f"\n{fam} ({sum(c.values())} residual rows):")
    for v, n in c.most_common():
        print(f"   {n:>3}  {v}")
