"""Tight rule: emevd bank-2000 init where callee(idx1) is a per-tile template (>=1e9) AND ints[2] is
a positionable _00 MSB enemy AND a real ItemLotParam lot appears at idx>=2. Count NEW (uncovered)
distinct lots + their item category — to see if the tight rule isolates the ~12 GR or still over-emits."""
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
ilm=E.param_to_dict(E.read_param(reg,'ItemLotParam_map',pdefs),lf)
ile=E.param_to_dict(E.read_param(reg,'ItemLotParam_enemy',pdefs),lf)
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
        tmp=os.path.join(tempfile.gettempdir(),str(os.getpid())+'_tg.tmp'); SysFile.WriteAllBytes(tmp,E.SoulsFormats.DCX.Decompress(str(p)).ToArray()); em=_rd.Invoke(None,Array[Object]([tmp])); os.unlink(tmp)
    except: continue
    for ev in em.Events:
        for ins in ev.Instructions:
            if int(ins.Bank)!=2000: continue
            a=bytes(ins.ArgData); ints=[struct.unpack_from('<i',a,k*4)[0] for k in range(len(a)//4)]
            if len(ints)<4 or ints[1]<1000000000: continue   # per-tile template callee
            if ints[2] not in ent00: continue                # entity@idx2 positionable
            for v in ints[2:]:
                if v in lot_ids and v not in ent00:
                    allc.add(v)
                    if v not in covered: new.add(v)
def cat(lot):
    r=ilm.get(lot) or ile.get(lot)
    return next((r.get(f'lotItemCategory0{s}') for s in range(1,9) if r and r.get(f'lotItemId0{s}',0)>0),None)
print(f"tight-rule candidate lots: {len(allc)} · covered {len(allc&covered)} · NEW {len(new)}")
print("  NEW by cat (1 goods/2 wep/3 arm/4 tal/5 gem):", dict(Counter(cat(l) for l in new)))
GR=[1034430200,1034471300,1034471310,1034481300,1034481310,1035480100,1038430100,1039410300,1039410310,1053560700,1053560710,1053560720]
print(f"  of the 12 target GR, captured by tight rule: {sum(1 for g in GR if g in new)}/12")

msg=E._read_from_bytes(E._bnd4_read,E.SoulsFormats.DCX.Decompress(str(E.MSGBND_PATH)),'.bnd')
nm={1:E.read_fmg_names(msg,'GoodsName.fmg'),2:E.read_fmg_names(msg,'WeaponName.fmg'),3:E.read_fmg_names(msg,'ProtectorName.fmg'),4:E.read_fmg_names(msg,'AccessoryName.fmg'),5:E.read_fmg_names(msg,'GemName.fmg')}
def nameof(lot):
    r=ilm.get(lot) or ile.get(lot)
    for s in range(1,9):
        iid=r.get(f'lotItemId0{s}',0); c=r.get(f'lotItemCategory0{s}',0)
        if iid>0 and c>0: return nm.get(c,{}).get(iid,'?')
    return '(empty)'
print("\n=== the 30 NEW recoverable lots ===")
for lot in sorted(new): print(f"  {lot:<11} {nameof(lot)}")
