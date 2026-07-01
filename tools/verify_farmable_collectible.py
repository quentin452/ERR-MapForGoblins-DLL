#!/usr/bin/env python3
"""Part A(b) verification — the WorldFarmableCollectible flag on ItemLotParam.

Reads ItemLotParam_map + ItemLotParam_enemy off regulation.bin (disk == live) and
answers the plan's open questions:
  - Is it "any of getItemFlagId01/02/03 == 0", or a single canonical field?
  - Polarity: 0 = farmable (no persistent flag) vs nonzero = one-time.
The paramdef shows 8 per-slot flags (01..08, "0 = use shared") PLUS a single master
`getItemFlagId` ("0 = flag disabled"). This script measures which one actually carries
the signal and confirms polarity against real item names.

Usage: py verify_farmable_collectible.py            # active profile (default ERR)
       MFG_PROFILE=vanilla py verify_farmable_collectible.py
"""
import sys, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pythonnet import load; load('coreclr')
import clr; clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats
import extract_all_items as E

def main():
    moddir = config.require_err_mod_dir()
    print(f'# profile={config.PROFILE}  source={moddir}\n')
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(moddir / 'regulation.bin'))
    pdefs = E.load_paramdefs()

    fields = {'getItemFlagId'}
    for i in range(1, 9):
        fields.add(f'getItemFlagId0{i}')
        fields.add(f'lotItemId0{i}')
        fields.add(f'lotItemCategory0{i}')

    # item-name resolution (reuse extract_all_items' merged-FMG reader)
    msgbnd = E._read_from_bytes(E._bnd4_read, SoulsFormats.DCX.Decompress(str(E.MSGBND_PATH)), '.bnd')
    names = {1: E.read_fmg_names(msgbnd, 'GoodsName.fmg'),
             2: E.read_fmg_names(msgbnd, 'WeaponName.fmg'),
             3: E.read_fmg_names(msgbnd, 'ProtectorName.fmg'),
             4: E.read_fmg_names(msgbnd, 'AccessoryName.fmg'),
             5: E.read_fmg_names(msgbnd, 'GemName.fmg')}
    def itemname(l):
        for s in range(1, 9):
            iid = int(l.get(f'lotItemId0{s}', 0) or 0); cat = int(l.get(f'lotItemCategory0{s}', 0) or 0)
            if iid > 0 and cat > 0:
                return names.get(cat, {}).get(iid) or f'cat{cat}_id{iid}'
        return '(empty)'

    for tname in ('ItemLotParam_map', 'ItemLotParam_enemy'):
        d = E.param_to_dict(E.read_param(bnd, tname, pdefs), fields)
        n = len(d)
        master0 = master_nz = 0
        perslot_any_nz = 0                 # lots where at least one 01..08 != 0
        perslot_nz_master0 = 0             # per-slot set but master 0 (true override)
        perslot_nz_master_nz = 0           # per-slot set AND master set
        for l in d.values():
            m = int(l.get('getItemFlagId', 0) or 0) & 0xffffffff
            m = 0 if m in (0, 0xffffffff) else m
            slots = [int(l.get(f'getItemFlagId0{i}', 0) or 0) & 0xffffffff for i in range(1, 9)]
            slots = [0 if s in (0, 0xffffffff) else s for s in slots]
            any_slot = any(slots)
            if m: master_nz += 1
            else: master0 += 1
            if any_slot:
                perslot_any_nz += 1
                if m: perslot_nz_master_nz += 1
                else: perslot_nz_master0 += 1

        print(f'== {tname} ==   rows={n}')
        print(f'  master getItemFlagId:  ==0 (farmable)={master0}   !=0 (one-time)={master_nz}')
        print(f'  per-slot 01..08 ever nonzero:  {perslot_any_nz} lots'
              f'   (of which master==0: {perslot_nz_master0}, master!=0: {perslot_nz_master_nz})')

        # polarity spot-check: sample named lots each side of the master flag
        def sample(pred, title, k=8):
            print(f'  -- {title} --')
            c = 0
            for lot, l in sorted(d.items()):
                m = int(l.get('getItemFlagId', 0) or 0) & 0xffffffff
                m = 0 if m in (0, 0xffffffff) else m
                if not pred(m): continue
                nm = itemname(l)
                if nm == '(empty)': continue
                print(f'     lot {lot:>11}  flag={m:<11}  {nm}')
                c += 1
                if c >= k: break
        sample(lambda m: m == 0, 'master==0 (expect farmable: materials/consumables)')
        sample(lambda m: m != 0, 'master!=0 (expect one-time: uniques/key items)')
        print()

if __name__ == '__main__':
    main()
