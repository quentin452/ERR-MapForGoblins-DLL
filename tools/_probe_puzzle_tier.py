#!/usr/bin/env python3
"""Are the extra-puzzle assets (AEG099_047 Sellia chalices, AEG110_029 Siofra lanterns,
AEG237_055 Snow Town statues) in _00 tiles (runtime-visible) or only in LOD supertiles?"""
import extract_all_items as E, config
from pathlib import Path
MSB_DIR = Path(config.require_err_mod_dir())/'map'/'MapStudio'
MODELS = {'AEG099_047':'Sellia chalice','AEG110_029':'Siofra lantern','AEG237_055':'Snow Town statue'}
from collections import Counter
res = {m:{'_00':0,'lod':0,'_00_ent':0,'lod_ent':0,'tiles':Counter()} for m in MODELS}
for mp in sorted(MSB_DIR.glob('*.msb.dcx')):
    stem = mp.name.replace('.msb.dcx','')
    is00 = stem.endswith('_00')
    try: msb = E._read_from_bytes(E._msbe_read, E.SoulsFormats.DCX.Decompress(str(mp)), '.msb')
    except: continue
    for a in msb.Parts.Assets:
        mdl = str(a.ModelName)
        if mdl not in MODELS: continue
        try: eid=int(a.EntityID)
        except: eid=0
        r=res[mdl]
        r['_00' if is00 else 'lod']+=1
        if eid>0: r['_00_ent' if is00 else 'lod_ent']+=1
        if eid>0: r['tiles'][stem]+=1
for m,lbl in MODELS.items():
    r=res[m]
    print(f"{m} ({lbl}): _00={r['_00']} ( {r['_00_ent']} w/EntityID )  LOD/non-_00={r['lod']} ( {r['lod_ent']} w/EntityID )")
    for t,n in r['tiles'].most_common(8): print(f"    {t}: {n} w/entity")
