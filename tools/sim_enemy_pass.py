#!/usr/bin/env python3
"""Reproduce the RUNTIME enemy-drop pass offline to explain why it emits ~99 (not the
~349 the items_database datamine predicted). Mirrors build_disk_enemy_markers exactly:
parse each _00 MSB's Parts.Enemies, resolve npc_loot_lot (itemLotId_map pref / _enemy),
count placements per resolved lot over the DISK parse, filter (placements==1 &&
getItemFlagId!=0). Tests the GameEditionDisable hypothesis (the offline pipeline skips
GED==1 enemies; the runtime pass does NOT → extra placements inflate the swarm count).

Run: PYTHONUTF8=1 PYTHONPATH="C:/Users/iamacat/AppData/Local/Temp/mfg_aux" \
     MFG_PROFILE=err py -3.14 tools/sim_enemy_pass.py
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
_str = SysType.GetType('System.String')
_param_read = asm.GetType('SoulsFormats.PARAM').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str]), None)
_msbe_read = asm.GetType('SoulsFormats.MSBE').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str]), None)

def load_paramdefs():
    defs = {}
    for xml_path in config.PARAMDEF_DIR.glob('*.xml'):
        try:
            pdef = SoulsFormats.PARAMDEF.XmlDeserialize(str(xml_path), False)
            if pdef and pdef.ParamType: defs[str(pdef.ParamType)] = pdef
        except Exception: pass
    return defs

def _read_param(data):
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_se.param')
    SysFile.WriteAllBytes(tmp, data.ToArray() if hasattr(data,'ToArray') else data)
    r = _param_read.Invoke(None, Array[Object]([tmp])); os.unlink(tmp); return r

def read_param(bnd, name, paramdefs, exclude=None):
    for f in bnd.Files:
        fn = str(f.Name)
        if name in fn and (exclude is None or exclude not in fn):
            p = _read_param(f.Bytes)
            pt = str(p.ParamType) if p.ParamType else ''
            if pt in paramdefs: p.ApplyParamdef(paramdefs[pt])
            return p
    return None

def cells(param, fields):
    out = {}
    for row in param.Rows:
        e = {}
        for c in row.Cells:
            n = str(c.Def.InternalName)
            if n in fields:
                try: e[n] = int(str(c.Value))
                except Exception: e[n] = 0
        out[int(row.ID)] = e
    return out

def read_msb(path):
    data = bytes(SoulsFormats.DCX.Decompress(str(path)).ToArray())
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_se.msb')
    SysFile.WriteAllBytes(tmp, data)
    msb = _msbe_read.Invoke(None, Array[Object]([tmp])); os.unlink(tmp); return msb

def main():
    paramdefs = load_paramdefs()
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(config.require_err_mod_dir()/'regulation.bin'))
    npc = cells(read_param(bnd,'NpcParam',paramdefs,exclude='NpcThink'), {'itemLotId_map','itemLotId_enemy'})
    fe = cells(read_param(bnd,'ItemLotParam_enemy',paramdefs), {'getItemFlagId'})
    fm = cells(read_param(bnd,'ItemLotParam_map',paramdefs), {'getItemFlagId'})
    PREFER_ENEMY = os.environ.get('PREFER','enemy') == 'enemy'
    def npc_lot(nid):
        r = npc.get(nid)
        if not r: return 0,0
        m = r.get('itemLotId_map',0)
        e = r.get('itemLotId_enemy',0)
        if PREFER_ENEMY:
            if e>0: return e,2
            if m>0: return m,1
        else:
            if m>0: return m,1
            if e>0: return e,2
        return 0,0
    print(f"(lot preference: {'ENEMY-first' if PREFER_ENEMY else 'MAP-first'})")
    def raw_flag(lot):
        if lot in fe: return fe[lot].get('getItemFlagId',0)
        if lot in fm: return fm[lot].get('getItemFlagId',0)
        return 0
    def flag(lot):
        # mirror runtime resolve_loot_flag: repeatable/temp flags (-1 or >=0x40000000)
        # read as not-a-tracked-collectible → 0.
        f = raw_flag(lot)
        if f == 0xFFFFFFFF or f == -1 or f >= 0x40000000: return 0
        return f

    msb_dir = config.require_err_mod_dir()/'map'/'MapStudio'
    # collect placements: (lot, lt, ged) per enemy, with/without GED
    plc_all = []   # runtime: no GED skip
    plc_ged = []   # offline: skip GED==1
    for p in sorted(msb_dir.glob('*_00.msb.dcx')):
        try: msb = read_msb(p)
        except Exception: continue
        for e in (getattr(msb.Parts,'Enemies',[]) or []):
            nid = int(getattr(e,'NPCParamID',0) or 0)
            ged = int(getattr(e,'GameEditionDisable',0) or 0)
            lot,lt = npc_lot(nid)
            if lot==0: continue
            plc_all.append((lot,lt))
            if ged!=1: plc_ged.append((lot,lt))

    def simulate(plc, label):
        lot_count = Counter(l for l,_ in plc)
        emit = set()
        swarm=noflag=0
        for lot,lt in plc:
            if lot_count[lot]!=1: swarm+=1; continue
            if flag(lot)==0: noflag+=1; continue
            emit.add(lot)
        print(f"  [{label}] placements={len(plc)} -> emit lots={len(emit)} "
              f"(swarm-placements={swarm}, no-flag-placements={noflag})")
        return emit

    print("Enemy pass simulation:")
    e_all = simulate(plc_all, "runtime (no GED skip)")
    e_ged = simulate(plc_ged, "offline (skip GED==1)")

    # compare to baked 119
    entries = json.load(open('.scratch/entries.json'))
    baked = set(e['lotId'] for e in entries if e['src']=='Enemy')
    print(f"\nbaked-119 distinct lots: {len(baked)}")
    print(f"  covered by runtime(no-GED): {len(baked & e_all)}  missing: {len(baked - e_all)}")
    print(f"  covered by offline(GED):    {len(baked & e_ged)}  missing: {len(baked - e_ged)}")
    miss = sorted(baked - e_all)
    lc_all = Counter(l for l,_ in plc_all)
    print(f"\n  baked lots missing under runtime ({len(miss)}): reasons —")
    r_swarm=r_flag=r_notondisk=0
    for l in miss:
        if lc_all.get(l,0)==0: r_notondisk+=1
        elif lc_all.get(l,0)>1: r_swarm+=1
        elif flag(l)==0: r_flag+=1
    print(f"    not-on-_00-disk(npc resolves elsewhere): {r_notondisk}, "
          f"swarm(>1 placement): {r_swarm}, persistent-flag-but-repeatable: {r_flag}")

    # list the repeatable-flag baked cases (on disk, lc==1, but runtime flag()==0)
    db0 = json.load(open('data/items_database.json'))
    ubl = defaultdict(list)
    for r in db0:
        if r.get('source')=='enemy': ubl[r['itemLotId']].append(r)
    print("  >>> repeatable-flag baked cases (excluded by runtime flag semantics):")
    for l in miss:
        if lc_all.get(l,0)==1 and flag(l)==0:
            rows = ubl.get(l, [])
            nm = rows[0]['items'][0]['name'] if rows and rows[0].get('items') else '?'
            mp = rows[0]['map'] if rows else '?'
            print(f"      lot {l} {mp} item={nm!r} rawflag={raw_flag(l)} (>=0x40000000={raw_flag(l)>=0x40000000})")

    # Alternative filter B: persistent-flag only, DEDUP by lot (ignore placement count).
    def sim_flagonly(plc, label):
        emit = set(l for l,_ in plc if flag(l)!=0)
        print(f"  [{label}] emit lots={len(emit)}  covers baked={len(baked & emit)}/119")
        return emit
    # DUPLICATE measure: of the baked enemy lots NOT lot-matched, how many have their
    # npc's resolved lot EMITTED by filter B (disk marker at the same enemy position)?
    fb_emit = set(l for l,_ in plc_all if flag(l)!=0)
    db1 = json.load(open('data/items_database.json'))
    ubl2 = defaultdict(list)
    for r in db1:
        if r.get('source')=='enemy': ubl2[r['itemLotId']].append(r)
    dup_baked = 0
    for l in miss:
        rows = ubl2.get(l, [])
        if not rows: continue
        rl,_ = npc_lot(rows[0].get('npcParamId',0))
        if rl in fb_emit: dup_baked += 1
    print(f"\nDUPLICATE estimate (filter B + lot-exact guard): {dup_baked} of {len(miss)} "
          f"uncovered baked rows sit at a position the disk ALSO emits (sibling lot)")

    print("\nFilter variants (runtime flag semantics: repeatable>=0x40000000 -> 0):")
    fa = simulate(plc_all, "A: placement==1 && persistent-flag")
    print(f"      covers baked={len(baked & fa)}/119, extra={len(fa-baked)}")
    fb = sim_flagonly(plc_all, "B: persistent-flag only (dedup by lot)")
    print(f"      extra over baked={len(fb-baked)}")

    # Diagnose the not-on-disk baked lots: what map/item/npc, what does the npc resolve to?
    db = json.load(open('data/items_database.json'))
    uni_by_lot = defaultdict(list)
    for r in db:
        if r.get('source')=='enemy': uni_by_lot[r['itemLotId']].append(r)
    notdisk = [l for l in miss if lc_all.get(l,0)==0]
    print(f"\n  {len(notdisk)} baked lots NOT produced by the _00 disk parse — sample:")
    for l in sorted(notdisk)[:20]:
        rows = uni_by_lot.get(l, [])
        if rows:
            r = rows[0]
            nid = r.get('npcParamId',0)
            rl,rt = npc_lot(nid)
            nm = r['items'][0]['name'] if r.get('items') else '?'
            print(f"    lot {l} {r['map']} npc={nid} item={nm!r} | npc_lot->{rl}(lt{rt}) rawflag={raw_flag(l)}")
        else:
            print(f"    lot {l} (not in items_database enemy rows)")

if __name__ == '__main__':
    main()
