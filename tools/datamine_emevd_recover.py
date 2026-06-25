#!/usr/bin/env python3
"""Datamine whether the 529 baked Emevd loot markers are RECOVERABLE from disk (no bake).

Their positions all come from an MSB enemy part (datamined: 521 c-prefix ChrIns, 8
cross-tile). The lot->entity link lives in EMEVD: a template event instruction carries
(entity_id, lot_id). So the runtime chain would be:
  EMEVD instr (template event) -> (entity_id, lot_id)
    -> MSB enemy part with that EntityID (we already read EntityID @ part+0x60)
    -> position.
This reproduces the pipeline's EMEVD extraction and measures coverage of the 529 baked
Emevd lots, to decide if a runtime EMEVD parser is worth it.

Run: PYTHONUTF8=1 PYTHONPATH="C:/Users/iamacat/AppData/Local/Temp/mfg_aux" \
     MFG_PROFILE=err py -3.14 tools/datamine_emevd_recover.py
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
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid())+'_er.tmp')
    SysFile.WriteAllBytes(tmp, data.ToArray() if hasattr(data,'ToArray') else data)
    r = meth.Invoke(None, Array[Object]([tmp])); os.unlink(tmp); return r

# template event -> (entity_off, lot_off, min_len)  [from extract_all_items]
TEMPLATE_EVENTS = {
    90005300:(8,16,20), 90005301:(8,16,20),
    90005860:(16,24,28), 90005861:(16,24,28), 90005880:(16,24,28),
    90005750:(8,16,20), 90005753:(8,16,20),
    90005774:(8,12,16), 90005792:(20,24,28),
    90005632:(8,16,20), 90005110:(8,20,24), 90005390:(8,28,32), 90005555:(8,12,16),
}

def main():
    moddir = config.require_err_mod_dir()
    # 1) parse EMEVD -> (entity, lot)
    emevd_dir = moddir/'event'
    calls = []  # (entity, lot, map, eventId)
    n_events=n_instr=0
    for p in sorted(emevd_dir.glob('*.emevd.dcx')):
        mapn = p.name.replace('.emevd.dcx','')
        try: em = _read(_emevd_read, SoulsFormats.DCX.Decompress(str(p)))
        except Exception: continue
        for ev in em.Events:
            n_events+=1
            for ins in ev.Instructions:
                n_instr+=1
                if int(ins.Bank)!=2000: continue
                args=bytes(ins.ArgData)
                if len(args)<8: continue
                eid_ev=struct.unpack_from('<i',args,4)[0]
                t=TEMPLATE_EVENTS.get(eid_ev)
                if not t: continue
                eo,lo,ml=t
                if len(args)<ml: continue
                ent=struct.unpack_from('<i',args,eo)[0]
                lot=struct.unpack_from('<i',args,lo)[0]
                if lot>0 and ent>0: calls.append((ent,lot,mapn,eid_ev))
    print(f"EMEVD: {n_events} events, {n_instr} instrs; {len(calls)} template award calls")
    lot_to_ent = {}
    for ent,lot,mapn,ev in calls:
        lot_to_ent.setdefault(lot, (ent,ev))
    by_template = Counter(ev for _,_,_,ev in calls)
    print("  award calls by template:", dict(by_template))

    # 2) MSB enemy parts: EntityID -> pos
    want_ent = {e for e,_,_,_ in calls}
    ent_to_pos = {}
    for mp in sorted((moddir/'map'/'MapStudio').glob('*.msb.dcx')):
        nm=mp.name.replace('.msb.dcx','')
        if nm.endswith('_99'): continue
        try: msb=_read(_msbe_read, SoulsFormats.DCX.Decompress(str(mp)))
        except Exception: continue
        for q in (getattr(msb.Parts,'Enemies',[]) or []):
            if int(getattr(q,'GameEditionDisable',0) or 0)==1: continue
            eid=int(getattr(q,'EntityID',0) or 0)
            if eid in want_ent and eid not in ent_to_pos:
                ent_to_pos[eid]={'name':str(q.Name),'map':nm}
    print(f"  EMEVD entities found as MSB enemy parts: {len(ent_to_pos)}/{len(want_ent)}")

    # 3) coverage of the 529 baked Emevd lots
    entries=json.load(open('.scratch/entries.json'))
    baked=[e for e in entries if e['src']=='Emevd']
    baked_lots=set(e['lotId'] for e in baked)
    rec=ent_missing=lot_missing=0
    for lot in baked_lots:
        if lot not in lot_to_ent: lot_missing+=1; continue
        ent,ev=lot_to_ent[lot]
        if ent in ent_to_pos: rec+=1
        else: ent_missing+=1
    print(f"\n529 baked Emevd lots recoverability:")
    print(f"  lot found in EMEVD template calls: {len(baked_lots)-lot_missing}/{len(baked_lots)}")
    print(f"  ...and its entity is an MSB enemy part (=> position): {rec}")
    print(f"  lot NOT in any EMEVD template call: {lot_missing}")
    print(f"  lot in EMEVD but entity not an MSB enemy part: {ent_missing}")

    # which templates carry the baked lots?
    tmpl=Counter()
    for lot in baked_lots:
        if lot in lot_to_ent: tmpl[lot_to_ent[lot][1]]+=1
    print("  baked Emevd lots by template event:", dict(tmpl))

    # sample the not-recoverable ones
    nr=[lot for lot in baked_lots if lot not in lot_to_ent]
    db=json.load(open('data/items_database.json'))
    ubl=defaultdict(list)
    for r in db:
        if r.get('source') in ('emevd','emevd_treasure'): ubl[r['itemLotId']].append(r)
    print(f"\n  sample lots NOT in EMEVD template calls ({len(nr)}):")
    for lot in sorted(nr)[:15]:
        rows=ubl.get(lot,[])
        nm=rows[0]['items'][0]['name'] if rows and rows[0].get('items') else '?'
        pn=rows[0].get('partName','?') if rows else '?'
        print(f"    lot {lot} part={pn} item={nm!r}")

if __name__=='__main__':
    main()
