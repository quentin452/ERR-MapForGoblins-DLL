"""Precise single-pair rule to implement in C++: bank-2000 init, callee(a+4)>=1e9, entity@a+8 is a
positionable _00 MSB enemy, lot@(a+argLen-8). Count distinct lots + 12-GR capture + 0-double check."""
import os, struct, tempfile
import extract_all_items as E, config
from pathlib import Path
from collections import Counter
from System import Array, Object
from System.IO import File as SysFile
ERR=Path(config.require_err_mod_dir()); MSB=ERR/'map'/'MapStudio'
reg=E.SoulsFormats.SFUtil.DecryptERRegulation(str(ERR/'regulation.bin')); pdefs=E.load_paramdefs()
lf=set()
for i in range(1,9): lf.update([f'lotItemId0{i}',f'lotItemCategory0{i}'])
ilm=E.param_to_dict(E.read_param(reg,'ItemLotParam_map',pdefs),lf); ile=E.param_to_dict(E.read_param(reg,'ItemLotParam_enemy',pdefs),lf)
lot_ids=set(ilm)|set(ile)
npc=E.param_to_dict(E.read_param(reg,'NpcParam',pdefs),{'itemLotId_map','itemLotId_enemy'})
npc_lots={f[k] for f in npc.values() for k in ('itemLotId_map','itemLotId_enemy') if f.get(k,0)>0}
ent00=set(); treas=set()
for mp in sorted(MSB.glob('*_00.msb.dcx')):
    try: msb=E._read_from_bytes(E._msbe_read,E.SoulsFormats.DCX.Decompress(str(mp)),'.msb')
    except: continue
    for en in getattr(msb.Parts,'Enemies',[]) or []:
        try:
            e=int(en.EntityID)
            if e>0: ent00.add(e)
        except: pass
    for t in getattr(msb.Events,'Treasures',[]) or []:
        try: treas.add(int(t.ItemLotID))
        except: pass
covered=npc_lots|treas
asm=E.asm; _rd=asm.GetType('SoulsFormats.EMEVD').GetMethod('Read',E.BindingFlags.Public|E.BindingFlags.Static|E.BindingFlags.FlattenHierarchy,None,E.Array[E.SysType]([E.SysType.GetType('System.String')]),None)
new=set(); allc=set()
for p in sorted((ERR/'event').glob('*.emevd.dcx')):
    try:
        tmp=os.path.join(tempfile.gettempdir(),str(os.getpid())+'_pr.tmp'); SysFile.WriteAllBytes(tmp,E.SoulsFormats.DCX.Decompress(str(p)).ToArray()); em=_rd.Invoke(None,Array[Object]([tmp])); os.unlink(tmp)
    except: continue
    for ev in em.Events:
        for ins in ev.Instructions:
            if int(ins.Bank)!=2000: continue
            a=bytes(ins.ArgData); n=len(a)
            if n<16: continue
            callee=struct.unpack_from('<i',a,4)[0]
            if callee<1000000000: continue
            entity=struct.unpack_from('<i',a,8)[0]
            lot=struct.unpack_from('<i',a,n-8)[0]   # idx(n-2)
            if entity not in ent00 or lot<=0 or lot==entity: continue
            if lot in lot_ids:
                allc.add(lot)
                if lot not in covered: new.add(lot)
GR=[1034430200,1034471300,1034471310,1034481300,1034481310,1035480100,1038430100,1039410300,1039410310,1053560700,1053560710,1053560720]
print(f"precise rule (entity@idx2 enemy, lot@idx(n-2)): candidates {len(allc)} · covered {len(allc&covered)} · NEW {len(new)}")
print(f"  12-GR captured: {sum(1 for g in GR if g in new)}/12")
