#!/usr/bin/env python3
"""Datamine a RUNTIME-readable notability signal that separates the 119 curated baked
enemy-drop markers from the ~25608 raw enemy-drop universe.

Hypotheses tested (cheapest first):
  H1  getItemFlagId != 0 on the lot (one-time persistent drop vs respawning clutter).
  H2  placements-per-lot (unique enemy vs swarm/trash spawning the same lot).
  H3  lotSource map vs enemy (NpcParam.itemLotId_map = persistent vs itemLotId_enemy).
  H4  item broad_category (notable equip/key vs consumable/crafting clutter).

If any cleanly separates baked-119 from the rest, the disk enemy pass can filter LIVE
(e.g. resolve_loot_flag(lot)!=0) instead of porting the pipeline's LOOT_CATEGORIES.

Run: PYTHONUTF8=1 PYTHONPATH="C:/Users/iamacat/AppData/Local/Temp/mfg_aux" \
     MFG_PROFILE=err py -3.14 tools/datamine_enemy_notability.py
"""
import sys, io, os, json, tempfile
from collections import Counter, defaultdict
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pathlib import Path
from pythonnet import load
load('coreclr')
import clr
clr.AddReference(str(config.SOULSFORMATS_DLL))
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
import SoulsFormats

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
_str_type = SysType.GetType('System.String')
_param_read = asm.GetType('SoulsFormats.PARAM').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)
PARAMDEF_DIR = config.PARAMDEF_DIR

def load_paramdefs():
    defs = {}
    for xml_path in PARAMDEF_DIR.glob('*.xml'):
        try:
            pdef = SoulsFormats.PARAMDEF.XmlDeserialize(str(xml_path), False)
            if pdef and pdef.ParamType:
                defs[str(pdef.ParamType)] = pdef
        except Exception:
            pass
    return defs

def _read_param_bytes(data):
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_dm.param')
    SysFile.WriteAllBytes(tmp, data.ToArray() if hasattr(data, 'ToArray') else data)
    r = _param_read.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    return r

def read_param(bnd, name, paramdefs, exclude=None):
    for f in bnd.Files:
        fn = str(f.Name)
        if name in fn and (exclude is None or exclude not in fn):
            param = _read_param_bytes(f.Bytes)
            pt = str(param.ParamType) if param.ParamType else ''
            if pt in paramdefs:
                param.ApplyParamdef(paramdefs[pt])
            return param
    return None

def lot_flag_map(param, field='getItemFlagId'):
    out = {}
    for row in param.Rows:
        fv = 0
        for cell in row.Cells:
            if str(cell.Def.InternalName) == field:
                try: fv = int(str(cell.Value))
                except Exception: fv = 0
                break
        out[int(row.ID)] = fv
    return out

def main():
    paramdefs = load_paramdefs()
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(config.require_err_mod_dir() / 'regulation.bin'))
    ilp_enemy = read_param(bnd, 'ItemLotParam_enemy', paramdefs)
    ilp_map = read_param(bnd, 'ItemLotParam_map', paramdefs)
    flag_enemy = lot_flag_map(ilp_enemy)
    flag_map = lot_flag_map(ilp_map)
    print(f"ItemLotParam_enemy rows: {len(flag_enemy)}  map rows: {len(flag_map)}")

    def flag_of(lot):
        # a lot id may live in either table; prefer enemy (lotType 2)
        if lot in flag_enemy: return flag_enemy[lot]
        if lot in flag_map: return flag_map[lot]
        return None

    # baked 119 enemy lots
    entries = json.load(open('.scratch/entries.json'))
    baked = [e for e in entries if e['src'] == 'Enemy']
    baked_lots = [e['lotId'] for e in baked]
    print(f"\nbaked enemy markers: {len(baked_lots)} ({len(set(baked_lots))} distinct lots)")

    # universe: items_database source=='enemy'
    db = json.load(open('data/items_database.json'))
    uni = [r for r in db if r.get('source') == 'enemy']
    uni_lots = [r['itemLotId'] for r in uni]
    placements_per_lot = Counter(uni_lots)
    print(f"universe enemy placements: {len(uni)} ({len(set(uni_lots))} distinct lots)")

    # ── H1: getItemFlagId != 0 ──
    def flag_stats(lots, label):
        n = len(lots)
        nz = sum(1 for l in lots if (flag_of(l) or 0) != 0)
        miss = sum(1 for l in lots if flag_of(l) is None)
        print(f"  {label:22} {n:6}  getItemFlagId!=0: {nz:6} ({100*nz/max(n,1):.1f}%)  "
              f"lot-not-in-tables: {miss}")
    print("\n=== H1: getItemFlagId (one-time persistent vs respawn) ===")
    flag_stats(baked_lots, "baked-119 (per marker)")
    flag_stats(list(set(baked_lots)), "baked-119 (distinct)")
    flag_stats(uni_lots, "universe (per placement)")
    flag_stats(list(set(uni_lots)), "universe (distinct lots)")

    # ── H2: placements-per-lot ──
    print("\n=== H2: placements-per-lot (unique enemy vs swarm) ===")
    baked_lotset = set(baked_lots)
    bk_pp = [placements_per_lot.get(l, 0) for l in baked_lotset]
    import statistics
    def dist(vals, label):
        vals = [v for v in vals if v > 0]
        if not vals: print(f"  {label}: (none in universe)"); return
        vals.sort()
        print(f"  {label}: n={len(vals)} min={vals[0]} median={statistics.median(vals):.0f} "
              f"max={vals[-1]} mean={statistics.mean(vals):.1f}")
    dist(bk_pp, "baked-119 lots' placement count")
    dist(list(placements_per_lot.values()), "ALL universe lots' placement count")
    # how many universe lots have placement count <= the baked max?
    bk_max = max(bk_pp) if bk_pp else 0
    le = sum(1 for v in placements_per_lot.values() if v <= bk_max)
    print(f"  baked max placements/lot = {bk_max}; universe lots with <= that: {le}/{len(placements_per_lot)}")

    # ── H3: lotSource (which NpcParam field) — baked all enemy-lot? ──
    print("\n=== H3: lot table membership (baked-119 distinct lots) ===")
    in_enemy = sum(1 for l in baked_lotset if l in flag_enemy)
    in_map = sum(1 for l in baked_lotset if l in flag_map and l not in flag_enemy)
    print(f"  in ItemLotParam_enemy: {in_enemy}  only in _map: {in_map}")

    # ── H4: broad_category of baked vs universe ──
    print("\n=== H4: broad_category split ===")
    def bc_counter(rows):
        c = Counter()
        for r in rows:
            for i in r.get('items', [])[:1]: c[i.get('broad_category')] += 1
        return c
    # baked rows need item info — join universe by lot
    uni_by_lot = defaultdict(list)
    for r in uni: uni_by_lot[r['itemLotId']].append(r)
    baked_rows = [uni_by_lot[l][0] for l in baked_lots if l in uni_by_lot]
    print("  baked-119:", dict(bc_counter(baked_rows)))
    print("  universe :", dict(bc_counter(uni)))

    # ── combined: does (flag!=0) alone reduce universe near 119? ──
    print("\n=== COMBINED: universe lots with getItemFlagId!=0 ===")
    uni_flag_lots = [l for l in set(uni_lots) if (flag_of(l) or 0) != 0]
    uni_flag_placements = sum(placements_per_lot[l] for l in uni_flag_lots)
    print(f"  distinct universe lots w/ flag!=0: {len(uni_flag_lots)} "
          f"(placements: {uni_flag_placements})")
    covered = sum(1 for l in baked_lotset if (flag_of(l) or 0) != 0)
    print(f"  of baked-119 distinct lots, flag!=0: {covered}/{len(baked_lotset)}")

    # ── REFINED runtime filters (both live/disk-derivable) ──
    print("\n=== REFINED FILTERS (universe lots; baked-119 must be a subset) ===")
    def show(pred, label):
        lots = [l for l in set(uni_lots) if pred(l)]
        plc = sum(placements_per_lot[l] for l in lots)
        miss = [l for l in baked_lotset if not pred(l)]
        print(f"  {label:38} lots={len(lots):5} placements={plc:6} "
              f"baked-119 NOT matched: {len(miss)}")
        return set(lots)
    f_flag = show(lambda l: (flag_of(l) or 0) != 0, "flag!=0")
    f_p1   = show(lambda l: placements_per_lot.get(l,0) == 1, "placements==1")
    f_both = show(lambda l: (flag_of(l) or 0) != 0 and placements_per_lot.get(l,0) == 1,
                  "flag!=0 AND placements==1")
    # what are the extra lots beyond the baked 119 under the combined filter?
    extra = sorted(f_both - baked_lotset)
    print(f"\n  combined filter yields {len(f_both)} lots; baked-119 ⊂ it: "
          f"{baked_lotset.issubset(f_both)}; extra (not in bake): {len(extra)}")
    uni_by_lot = defaultdict(list)
    for r in uni: uni_by_lot[r['itemLotId']].append(r)
    print("  sample extra lots (notable by signal, NOT baked — bake under-coverage?):")
    for l in extra[:25]:
        rows = uni_by_lot.get(l, [])
        nm = rows[0]['items'][0]['name'] if rows and rows[0].get('items') else '?'
        mp = rows[0]['map'] if rows else '?'
        print(f"    lot {l} {mp} flag={flag_of(l)} item={nm}")

if __name__ == '__main__':
    main()
