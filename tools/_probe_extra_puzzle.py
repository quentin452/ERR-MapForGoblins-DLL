#!/usr/bin/env python3
"""Predict the extra-puzzle recovery: parse the 5 bespoke events -> (entity->flag), then
count _00 AEG099_047/AEG237_055 assets whose EntityID is in that set (= will be emitted)."""
import os, struct, tempfile
import extract_all_items as E, config
from pathlib import Path
from System import Array, Object
from System.IO import File as SysFile
ERR = Path(config.require_err_mod_dir()); MSB=ERR/'map'/'MapStudio'
EVENTS = {1049392302:(8,12),1049392303:(8,12),1050392303:(8,12),12022601:(12,8),12022621:(12,8)}
asm=E.asm; _rd=asm.GetType('SoulsFormats.EMEVD').GetMethod('Read',E.BindingFlags.Public|E.BindingFlags.Static|E.BindingFlags.FlattenHierarchy,None,E.Array[E.SysType]([E.SysType.GetType('System.String')]),None)
flags={}
for p in sorted((ERR/'event').glob('*.emevd.dcx')):
    try:
        tmp=os.path.join(tempfile.gettempdir(),str(os.getpid())+'_xp.tmp'); SysFile.WriteAllBytes(tmp,E.SoulsFormats.DCX.Decompress(str(p)).ToArray()); em=_rd.Invoke(None,Array[Object]([tmp])); os.unlink(tmp)
    except: continue
    for ev in em.Events:
        for ins in ev.Instructions:
            if int(ins.Bank)!=2000: continue
            a=bytes(ins.ArgData)
            if len(a)<8: continue
            tid=struct.unpack_from('<I',a,4)[0]
            if tid not in EVENTS: continue
            eo,fo=EVENTS[tid]
            if len(a)<max(eo,fo)+4: continue
            ent=struct.unpack_from('<I',a,eo)[0]; fl=struct.unpack_from('<I',a,fo)[0]
            if ent and fl: flags[ent]=fl
print(f"{len(flags)} (entity->flag) from the 5 events")
hit={'AEG099_047':0,'AEG237_055':0}
for mp in sorted(MSB.glob('*_00.msb.dcx')):
    try: msb=E._read_from_bytes(E._msbe_read,E.SoulsFormats.DCX.Decompress(str(mp)),'.msb')
    except: continue
    for a in msb.Parts.Assets:
        m=str(a.ModelName)
        if m not in hit: continue
        try: eid=int(a.EntityID)
        except: eid=0
        if eid in flags: hit[m]+=1
print(f"_00 assets that will EMIT: AEG099_047 (chalices)={hit['AEG099_047']}, AEG237_055 (lanterns)={hit['AEG237_055']}, total={sum(hit.values())}")
