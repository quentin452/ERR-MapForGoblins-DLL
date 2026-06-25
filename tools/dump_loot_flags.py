#!/usr/bin/env python3
"""Dump every distinct nonzero getItemFlagId from ItemLotParam_map/_enemy to a flat
text file (flag\tname), for the live RPM group-persistence sweep (flag_group_persistent.py).
Run from tools/ (cwd needs oo2core):
  PYTHONUTF8=1 PYTHONPATH=...mfg_aux MFG_PROFILE=err py -3.14 dump_loot_flags.py
"""
import sys, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pythonnet import load
load('coreclr')
import clr
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats
import extract_all_items as E

moddir = config.require_err_mod_dir()
bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(moddir / 'regulation.bin'))
pdefs = E.load_paramdefs()
lf = set()
for i in range(1, 9):
    lf.update([f'lotItemId0{i}', f'lotItemCategory0{i}'])
lf.add('getItemFlagId')
lots = {}
for tname in ('ItemLotParam_map', 'ItemLotParam_enemy'):
    d = E.param_to_dict(E.read_param(bnd, tname, pdefs), lf)
    for k, v in d.items():
        lots.setdefault(k, v)
msgbnd = E._read_from_bytes(E._bnd4_read, SoulsFormats.DCX.Decompress(str(E.MSGBND_PATH)), '.bnd')
names = {1: E.read_fmg_names(msgbnd, 'GoodsName.fmg'),
         2: E.read_fmg_names(msgbnd, 'WeaponName.fmg'),
         3: E.read_fmg_names(msgbnd, 'ProtectorName.fmg'),
         4: E.read_fmg_names(msgbnd, 'AccessoryName.fmg'),
         5: E.read_fmg_names(msgbnd, 'GemName.fmg')}
def itemname(l):
    for s in range(1, 9):
        iid = l.get(f'lotItemId0{s}', 0); cat = l.get(f'lotItemCategory0{s}', 0)
        if iid > 0 and cat > 0:
            return names.get(cat, {}).get(iid) or f'cat{cat}_id{iid}'
    return '(empty)'

seen = {}
for lot, l in lots.items():
    f = int(l.get('getItemFlagId', 0)) & 0xffffffff
    if f == 0 or f == 0xffffffff:
        continue
    if f not in seen:
        seen[f] = itemname(l)
out = r'D:\ghidra_scripts\loot_flags_dump.txt'
with open(out, 'w', encoding='utf-8') as fh:
    for f in sorted(seen):
        fh.write(f'{f}\t{seen[f]}\n')
print(f'wrote {len(seen)} distinct nonzero flags -> {out}')
print(f'  >=0x40000000: {sum(1 for f in seen if f>=0x40000000)}   <0x40000000: {sum(1 for f in seen if f<0x40000000)}')
