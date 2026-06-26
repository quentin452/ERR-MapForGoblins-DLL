#!/usr/bin/env python3
"""Is the Kindling position on disk? Scan m60_45_37_00.msb for the 5 Kindling SFX eids
(1045373501..505) across ALL region/part collections; report collection, subtype, position."""
import extract_all_items as E, config
from pathlib import Path
MSB = Path(config.require_err_mod_dir())/'map'/'MapStudio'/'m60_45_37_00.msb.dcx'
msb = E._read_from_bytes(E._msbe_read, E.SoulsFormats.DCX.Decompress(str(MSB)), '.msb')
targets = set(range(1045373500, 1045373510))
def dump(label, coll):
    for p in coll:
        try: eid = int(getattr(p,'EntityID',-1) or -1)
        except: eid = -1
        nm = str(getattr(p,'Name',''))
        # match by entity id OR by name containing the SFX id digits
        hit = eid in targets or any(str(t) in nm for t in targets) or 'ParticleEffect' in nm or 'SFX' in nm.upper()
        if eid in targets:
            pos = getattr(p,'Position',None)
            print(f"  [{label}] eid={eid} name={nm!r} type={type(p).__name__} pos=({pos.X:.1f},{pos.Y:.1f},{pos.Z:.1f})" if pos else f"  [{label}] eid={eid} name={nm!r}")
# Regions
for attr in dir(msb.Regions):
    if attr.startswith('_'): continue
    try: coll = getattr(msb.Regions, attr)
    except: continue
    if hasattr(coll,'__iter__'):
        try: dump('Region.'+attr, coll)
        except: pass
# also print ALL region eids in a window to see what IS there
print("--- all region entity ids in [1045373000,1045374000) ---")
for attr in dir(msb.Regions):
    if attr.startswith('_'): continue
    try: coll = getattr(msb.Regions, attr)
    except: continue
    if not hasattr(coll,'__iter__'): continue
    for p in coll:
        try: eid=int(getattr(p,'EntityID',-1) or -1)
        except: continue
        if 1045373000<=eid<1045374000:
            print(f"  Region.{attr} eid={eid} name={str(getattr(p,'Name',''))!r}")
