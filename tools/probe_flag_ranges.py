#!/usr/bin/env python3
"""Map the getItemFlagId value structure to see if resolve_loot_flag's `>= 0x40000000`
repeatable-cut can be refined to NOT drop DLC one-time loot.

resolve_loot_flag treats flag == -1 OR flag >= 0x40000000 as repeatable/temp (golden runes,
crafting) and returns 0. But DLC one-time flags (~2.0e9, 0x79-0x7A) are ALSO >= 0x40000000
yet persistent → wrongly dropped (Crucible Hammer Helm, Royal Magic Grease, Iris of
Occultation...). This buckets every ItemLotParam getItemFlagId >= 0x40000000 by its high byte
with sample item names, so we can see whether the repeatable cohort and the DLC-one-time
cohort are numerically separable.

Run: PYTHONUTF8=1 PYTHONPATH="C:/Users/iamacat/AppData/Local/Temp/mfg_aux" \
     MFG_PROFILE=err py -3.14 tools/probe_flag_ranges.py
"""
import sys, io, os
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from collections import Counter, defaultdict
from pythonnet import load
load('coreclr')
import clr
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats
import extract_all_items as E


def main():
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

    def itemname(lot):
        l = lots.get(lot, {})
        for s in range(1, 9):
            iid = l.get(f'lotItemId0{s}', 0); cat = l.get(f'lotItemCategory0{s}', 0)
            if iid > 0 and cat > 0:
                return names.get(cat, {}).get(iid) or f'cat{cat}_id{iid}'
        return '(empty)'

    # bucket flags >= 0x40000000 by high byte
    buckets = defaultdict(list)
    total_hi = 0
    minus1 = 0
    for lot, l in lots.items():
        f = int(l.get('getItemFlagId', 0)) & 0xffffffff
        if f == 0xffffffff:
            minus1 += 1
            continue
        if f >= 0x40000000:
            total_hi += 1
            buckets[f >> 24].append((f, lot))
    print(f'flags == -1 (re-droppable): {minus1}')
    print(f'flags >= 0x40000000: {total_hi}  (these are ALL dropped by resolve_loot_flag)')
    print('\nby high byte (flag >> 24) — range, count, sample items:')
    for hb in sorted(buckets):
        rows = buckets[hb]
        fmin = min(f for f, _ in rows); fmax = max(f for f, _ in rows)
        samples = []
        seen = set()
        for f, lot in sorted(rows):
            nm = itemname(lot)
            if nm in seen:
                continue
            seen.add(nm); samples.append(nm)
            if len(samples) >= 6:
                break
        print(f'  0x{hb:02x}xxxxxx  [{fmin}..{fmax}]  n={len(rows):4}  e.g. {samples}')

    # are DLC one-time vs DLC repeatable numerically separable? Sample both, sorted by flag.
    print('\nDLC one-time vs repeatable — flags side by side (sorted):')
    one_time = ['Scadutree Fragment', 'Revered Spirit Ash', 'Starlight Token', 'Crucible Hammer Helm',
                'Royal Magic Grease', 'Iris of Occultation', 'Blessed Bone Shard']
    repeatable = ['Shadow Realm Rune [2500]', 'Mushroom', 'Dewgem', 'Ghost Glovewort [4]',
                  'Deep-Purple Lily', 'Cerulean Sea']
    samples = []
    for lot, l in lots.items():
        nm = itemname(lot)
        f = int(l.get('getItemFlagId', 0)) & 0xffffffff
        if nm in one_time:
            samples.append((f, nm, 'ONE-TIME'))
        elif nm in repeatable:
            samples.append((f, nm, 'repeatable'))
    seen = set()
    for f, nm, kind in sorted(samples):
        if nm in seen: continue
        seen.add(nm)
        print(f'  0x{f:08x} {f:>12}  [{kind:10}] {nm}')


if __name__ == '__main__':
    main()
