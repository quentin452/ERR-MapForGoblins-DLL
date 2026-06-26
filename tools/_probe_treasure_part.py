"""For each treasure-src residual lot, find its MSB Events.Treasure and report the PART it
hangs on (Asset / DummyAsset / other), EntityID, and GameEditionDisable — to tell whether the
runtime treasure pass drops it (inert DummyAsset, no entity) or genuinely never sees it."""
import sys
import extract_all_items as E, config
from pathlib import Path
MSB = Path(config.require_err_mod_dir())/'map'/'MapStudio'
TARGETS = set(int(x) for x in sys.argv[1:])
# part name -> (type, entityId, ged) per tile, built lazily
def part_index(msb):
    idx = {}
    for typ in ('Assets','DummyAssets','Enemies','DummyEnemies','Players','Collisions','ConnectCollisions'):
        for p in getattr(msb.Parts, typ, []) or []:
            try: eid=int(p.EntityID)
            except: eid=0
            ged=None
            for a in ('GameEditionDisable','GameEditonDisable'):
                if hasattr(p,a): ged=getattr(p,a)
            idx[str(p.Name)] = (typ, eid, ged, str(getattr(p,'ModelName','')))
    return idx
found=set()
for mp in sorted(MSB.glob('*_00.msb.dcx')):
    try: msb=E._read_from_bytes(E._msbe_read,E.SoulsFormats.DCX.Decompress(str(mp)),'.msb')
    except: continue
    treas = getattr(msb.Events,'Treasures',None) or getattr(msb.Events,'Treasure',None) or []
    pidx=None
    for t in treas:
        lot=None
        for a in ('ItemLotID','ItemLotParamId','ItemLotId'):
            if hasattr(t,a): lot=int(getattr(t,a)); break
        if lot not in TARGETS: continue
        if pidx is None: pidx=part_index(msb)
        pn = None
        for a in ('TreasurePartName','PartName','TreasurePart'):
            if hasattr(t,a): pn=str(getattr(t,a)); break
        info = pidx.get(pn, ('(part not found)',0,None,''))
        found.add(lot)
        print(f"  lot={lot:<11} {mp.name.replace('.msb.dcx',''):14s} part={pn!s:28s} type={info[0]:12s} eid={info[1]} ged={info[2]} model={info[3]}")
print(f"\nfound MSB Treasure event for {len(found)}/{len(TARGETS)} lots; missing: {sorted(TARGETS-found)}")
