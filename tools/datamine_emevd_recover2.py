#!/usr/bin/env python3
"""Extended (template-AGNOSTIC) EMEVD recoverability datamine — fixes the ROI of a
runtime EMEVD parser before writing it.

For every bank-2000 event INIT instruction, collect all int32 args. Build:
  lot -> set of co-occurring int args  (the lot's event init also carries its entity)
and a comprehensive EntityID -> position map over ALL MSB parts (enemies + assets +
dummies — the corpse residual lives on AEG assets, not enemies). A target lot is
"EMEVD-recoverable" if its init co-carries an int that is a real MSB-part EntityID.

Targets:
  - 529 baked Emevd lots (re-confirm 307 + measure the other 222)
  - the ~328 corpse Treasure residual (partBucket != live) — is it EMEVD-bound too?

Run: PYTHONUTF8=1 PYTHONPATH="C:/Users/iamacat/AppData/Local/Temp/mfg_aux" \
     MFG_PROFILE=err py -3.14 tools/datamine_emevd_recover2.py
"""
import sys, io, os, json, struct, tempfile
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
def _read_str(tn):
    return asm.GetType(tn).GetMethod('Read', BindingFlags.Public|BindingFlags.Static|
        BindingFlags.FlattenHierarchy, None, Array[SysType]([_str]), None)
_emevd_read = _read_str('SoulsFormats.EMEVD')
_msbe_read = _read_str('SoulsFormats.MSBE')
def _read(meth, data):
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid())+'_er2.tmp')
    SysFile.WriteAllBytes(tmp, data.ToArray() if hasattr(data,'ToArray') else data)
    r = meth.Invoke(None, Array[Object]([tmp])); os.unlink(tmp); return r

def main():
    moddir = config.require_err_mod_dir()

    # 1) comprehensive EntityID -> pos over ALL MSB part categories
    PART_CATS = ('MapPieces','Enemies','Players','Collisions','DummyAssets',
                 'DummyEnemies','ConnectCollisions','Assets')
    ent_pos = {}     # entityId -> (map, partName, cat)
    ent_groups = defaultdict(set)  # entityGroupId -> set of entityIds (group awards)
    for mp in sorted((moddir/'map'/'MapStudio').glob('*.msb.dcx')):
        nm = mp.name.replace('.msb.dcx','')
        if nm.endswith('_99'): continue
        try: msb=_read(_msbe_read, SoulsFormats.DCX.Decompress(str(mp)))
        except Exception: continue
        for cat in PART_CATS:
            col = getattr(msb.Parts, cat, None)
            if not col: continue
            for q in col:
                if int(getattr(q,'GameEditionDisable',0) or 0)==1: continue
                eid = int(getattr(q,'EntityID',0) or 0)
                if eid>0 and eid not in ent_pos:
                    ent_pos[eid] = (nm, str(q.Name), cat)
                g = getattr(q,'EntityGroupIDs',None)
                if g is not None:
                    try:
                        for gv in g:
                            gv=int(gv)
                            if gv>0 and eid>0: ent_groups[gv].add(eid)
                    except Exception: pass
    print(f"MSB parts with EntityID: {len(ent_pos)}; entity groups: {len(ent_groups)}")

    def entity_reachable(v):
        return v in ent_pos or v in ent_groups

    # 2) EMEVD: every bank-2000 init -> int args. Build lot -> co-args index.
    lot_coargs = defaultdict(set)   # any int -> set of co-occurring ints in same init
    n_init=0
    for p in sorted((moddir/'event').glob('*.emevd.dcx')):
        try: em=_read(_emevd_read, SoulsFormats.DCX.Decompress(str(p)))
        except Exception: continue
        for ev in em.Events:
            for ins in ev.Instructions:
                if int(ins.Bank)!=2000: continue
                a=bytes(ins.ArgData)
                if len(a)<8: continue
                ints=[struct.unpack_from('<i',a,o)[0] for o in range(0,len(a)-3,4)]
                n_init+=1
                # treat every int in the init as a candidate "lot" key -> its co-args
                s=set(ints)
                for v in s:
                    lot_coargs[v] |= s
    print(f"bank-2000 inits parsed: {n_init}")

    def recoverable(lot):
        co = lot_coargs.get(lot)
        if not co: return None
        for v in co:
            if v!=lot and v>0 and entity_reachable(v):
                return v
        return False  # lot appears in EMEVD but no reachable entity co-arg

    # 3) targets
    entries=json.load(open('.scratch/entries.json'))
    emevd_lots=set(e['lotId'] for e in entries if e['src']=='Emevd')
    db=json.load(open('data/items_database.json'))
    # corpse residual = treasure rows with partBucket != 'live'
    corpse_lots=set()
    for r in db:
        if r.get('source')=='treasure' and r.get('partBucket')!='live':
            corpse_lots.add(r['itemLotId'])
    # restrict corpse to those actually baked as Treasure
    baked_treasure=set(e['lotId'] for e in entries if e['src']=='Treasure')
    corpse_lots &= baked_treasure

    def base_recoverable(l):
        # ItemLotParam sequence rows share a base whose last digit is 0; the EMEVD
        # awards the base, the row is base+k. Try the nearest base ids.
        for b in (l - (l % 10), l - (l % 10) + 0):
            if b!=l and isinstance(recoverable(b), int): return True
        # also try base = l with last digit 0 within the same decade range
        b = l - (l % 10)
        for k in range(0, 10):
            if (b+k)!=l and isinstance(recoverable(b+k), int): return True
        return False

    def report(lots, label):
        rec=in_emevd_no_ent=not_in_emevd=via_base=0
        for l in lots:
            r=recoverable(l)
            if isinstance(r,int): rec+=1
            elif r is False: in_emevd_no_ent+=1
            else:
                if base_recoverable(l): via_base+=1
                else: not_in_emevd+=1
        print(f"\n{label}: {len(lots)} lots")
        print(f"  EMEVD-recoverable directly (co-arg = reachable MSB entity/group): {rec}")
        print(f"  recoverable via a sequence-base lot that IS EMEVD-bound: {via_base}")
        print(f"  in EMEVD but no reachable entity co-arg: {in_emevd_no_ent}")
        print(f"  NOT recoverable (no EMEVD link, no base link): {not_in_emevd}")
        print(f"  => total recoverable with EMEVD parser (+chain): {rec+via_base}")
        return rec

    report(emevd_lots, "529 baked Emevd")
    report(corpse_lots, f"corpse Treasure residual (partBucket!=live, baked)")

    # sample the not-in-emevd ones for each
    def samples(lots, label, n=12):
        ubl=defaultdict(list)
        for r in db: ubl[r['itemLotId']].append(r)
        nr=[l for l in lots if recoverable(l) is None]
        print(f"  sample NOT-in-EMEVD {label} ({len(nr)}):")
        for l in sorted(nr)[:n]:
            rows=ubl.get(l,[])
            nm=rows[0]['items'][0]['name'] if rows and rows[0].get('items') else '?'
            pn=rows[0].get('partName','?') if rows else '?'
            print(f"    lot {l} part={pn} item={nm!r}")
    samples(emevd_lots, "Emevd")
    samples(corpse_lots, "corpse")

if __name__=='__main__':
    main()
