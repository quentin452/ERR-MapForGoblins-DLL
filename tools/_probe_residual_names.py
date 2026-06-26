#!/usr/bin/env python3
"""Resolve the ACTUAL item NAME + provenance of every baked-loot residual row.

Extends _probe_residual_family.py: for each surviving baked [RESIDUAL-ROW] (diag_loot_pos dump),
resolve the lotId -> ItemLotParam_map/_enemy -> item name (FMG), and cross-check the bake's src tag
against the deployed regulation (NpcParam ref? which lot table? row present?). Answers, per row,
"what item is this and is the bake's category/source right?" — so an "accepted" residual can be
re-examined by NAME instead of trusted blind.

Usage:  py _probe_residual_names.py ["Equipment - Armaments,Loot - Gloveworts"] [log]
        (no cat filter -> every residual row)
"""
import sys, re
import extract_all_items as E
import config
from pathlib import Path

DEFAULT_LOG = Path(r"D:\DOWNLOAD\ERRv2.2.9.6-541-2-2-9-6-1780861369\ERRv2.2.9.6\dll\offline\logs\MapForGoblins.log")
ROW = re.compile(r'\[RESIDUAL-ROW\] cat="(?P<cat>[^"]+)" src=(?P<src>\w+) lot=(?P<lot>\d+) '
                 r'lt=(?P<lt>\d+) m(?P<a>\d+)_(?P<gx>\d+)_(?P<gz>\d+) key=(?P<key>-?\d+)')

cats_arg, log = None, DEFAULT_LOG
for a in sys.argv[1:]:
    if a.lower().endswith('.log') or '\\' in a or '/' in a:
        log = Path(a)
    else:
        cats_arg = {c.strip().lower() for c in a.split(',')}

rows = {}
for line in open(log, encoding='utf-8', errors='replace'):
    m = ROW.search(line)
    if m:
        rows[int(m.group('lot'))] = dict(cat=m.group('cat'), src=m.group('src'), lt=int(m.group('lt')),
                                         tile=f"m{m.group('a')}_{m.group('gx')}_{m.group('gz')}", key=int(m.group('key')))
if not rows:
    sys.exit("no [RESIDUAL-ROW] lines — run with diag_loot_pos=true + open the map.")
sel = {l: r for l, r in rows.items() if not cats_arg or r['cat'].lower() in cats_arg}

print("=== Loading regulation + FMG names ===")
reg = E.SoulsFormats.SFUtil.DecryptERRegulation(str(Path(config.ERR_MOD_DIR) / 'regulation.bin'))
pdefs = E.load_paramdefs()
lot_fields = set()
for i in range(1, 9):
    lot_fields.update([f'lotItemId0{i}', f'lotItemCategory0{i}', f'lotItemNum0{i}'])
ilm = E.param_to_dict(E.read_param(reg, 'ItemLotParam_map', pdefs), lot_fields)
ile = E.param_to_dict(E.read_param(reg, 'ItemLotParam_enemy', pdefs), lot_fields)
npc = E.param_to_dict(E.read_param(reg, 'NpcParam', pdefs), {'itemLotId_map', 'itemLotId_enemy'})
npc_lotrefs = set()
for f in npc.values():
    for k in ('itemLotId_map', 'itemLotId_enemy'):
        if f.get(k, 0) > 0: npc_lotrefs.add(f[k])
import os, tempfile
from System import Array, Object
from System.IO import File as SysFile
def _read_bnd_tolerant(path_obj):  # like E._read_from_bytes but survives a locked-temp unlink
    tmp = os.path.join(tempfile.gettempdir(), f"mfg_msg_{os.getpid()}.bnd")
    SysFile.WriteAllBytes(tmp, E.SoulsFormats.DCX.Decompress(str(path_obj)).ToArray())
    res = E._bnd4_read.Invoke(None, Array[Object]([tmp]))
    try: os.unlink(tmp)
    except OSError: pass
    return res
msg = _read_bnd_tolerant(E.MSGBND_PATH)
name_dbs = {1: E.read_fmg_names(msg, 'GoodsName.fmg'), 2: E.read_fmg_names(msg, 'WeaponName.fmg'),
            3: E.read_fmg_names(msg, 'ProtectorName.fmg'), 4: E.read_fmg_names(msg, 'AccessoryName.fmg'),
            5: E.read_fmg_names(msg, 'GemName.fmg')}


def lot_items(lot, table):
    row = table.get(lot)
    if not row: return None
    out = []
    for s in range(1, 9):
        iid, cat, num = row.get(f'lotItemId0{s}', 0), row.get(f'lotItemCategory0{s}', 0), row.get(f'lotItemNum0{s}', 0)
        if iid > 0 and cat > 0:
            out.append(f"{name_dbs.get(cat, {}).get(iid, '?')}({'gwagm'[cat-1] if 1<=cat<=5 else cat}{iid}x{num})")
    return out if out else ['(empty/zero-item lot)']


print(f"\n[{len(sel)} residual rows" + (f" in {sorted(cats_arg)}" if cats_arg else "") + f", from {len(rows)} total]\n")
from collections import Counter
tally = Counter()
for lot, r in sorted(sel.items(), key=lambda kv: (kv[1]['cat'], kv[1]['src'], kv[0])):
    in_m, in_e = lot in ilm, lot in ile
    primary = ile if (r['lt'] == 2 and in_e) else (ilm if in_m else (ile if in_e else {}))
    items = lot_items(lot, primary) or ['(lot ROW ABSENT from both tables — stale)']
    where = ('map' if in_m else '') + ('+enemy' if in_e else '') or 'NONE'
    npcref = 'NpcParam-REF' if lot in npc_lotrefs else 'no-NpcParam'
    tally[(r['cat'], r['src'])] += 1
    print(f"  {r['cat']:<26} src={r['src']:<8} lot={lot:<10} lt={r['lt']} {r['tile']:<11} "
          f"[{where:<9} {npcref:<12}] -> {', '.join(items)}")

print("\n=== per (category, src) tally ===")
for (cat, src), n in sorted(tally.items()):
    print(f"   {n:>3}  {cat:<26} src={src}")
