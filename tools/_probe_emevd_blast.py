"""Blast-radius of a generic 'emevd bank-2000 init with a positionable _00-MSB enemy entity + a real
ItemLotParam lot' rule. Counts DISTINCT award lots, split covered (MSB Treasure / NpcParam) vs NEW
(would become markers). 'New' is an UPPER BOUND (ignores kEmevd-template + treasure-sibling coverage)."""
import os, struct, tempfile
import extract_all_items as E, config
from pathlib import Path
from collections import Counter
from System import Array, Object
from System.IO import File as SysFile
ERR=Path(config.require_err_mod_dir()); MSB=ERR/'map'/'MapStudio'
reg=E.SoulsFormats.SFUtil.DecryptERRegulation(str(ERR/'regulation.bin')); pdefs=E.load_paramdefs()
# real lots + first item
lf=set()
for i in range(1,9): lf.update([f'lotItemId0{i}',f'lotItemCategory0{i}'])
ilm=E.param_to_dict(E.read_param(reg,'ItemLotParam_map',pdefs),lf)
ile=E.param_to_dict(E.read_param(reg,'ItemLotParam_enemy',pdefs),lf)
lot_ids=set(ilm)|set(ile)
# covered: NpcParam-referenced
npc=E.param_to_dict(E.read_param(reg,'NpcParam',pdefs),{'itemLotId_map','itemLotId_enemy'})
npc_lots=set()
for f in npc.values():
    for k in ('itemLotId_map','itemLotId_enemy'):
        if f.get(k,0)>0: npc_lots.add(f[k])
# _00 MSB enemies (entity set, matches runtime scope) + MSB treasure lots (covered)
ent00=set(); treas_lots=set()
import re
for mp in sorted(MSB.glob('*_00.msb.dcx')):
    try: msb=E._read_from_bytes(E._msbe_read,E.SoulsFormats.DCX.Decompress(str(mp)),'.msb')
    except: continue
    for en in getattr(msb.Parts,'Enemies',[]) or []:
        try:
            e=int(en.EntityID)
            if e>0: ent00.add(e)
        except: pass
    for t in getattr(msb.Events,'Treasures',[]) or []:
        try: treas_lots.add(int(t.ItemLotID))
        except: pass
covered=npc_lots|treas_lots
# emevd scan
asm=E.asm; _rd=asm.GetType('SoulsFormats.EMEVD').GetMethod('Read',E.BindingFlags.Public|E.BindingFlags.Static|E.BindingFlags.FlattenHierarchy,None,E.Array[E.SysType]([E.SysType.GetType('System.String')]),None)
cand_lots=set()
for p in sorted((ERR/'event').glob('*.emevd.dcx')):
    try:
        tmp=os.path.join(tempfile.gettempdir(),str(os.getpid())+'_bl.tmp'); SysFile.WriteAllBytes(tmp,E.SoulsFormats.DCX.Decompress(str(p)).ToArray()); em=_rd.Invoke(None,Array[Object]([tmp])); os.unlink(tmp)
    except: continue
    for ev in em.Events:
        for ins in ev.Instructions:
            if int(ins.Bank)!=2000: continue
            a=bytes(ins.ArgData); ints=[struct.unpack_from('<i',a,k*4)[0] for k in range(len(a)//4)]
            has_ent=any(v in ent00 for v in ints)
            if not has_ent: continue
            for v in ints:
                if v in lot_ids and v not in ent00: cand_lots.add(v)
new=cand_lots-covered
print(f"candidate award lots (emevd init w/ positionable _00 enemy + real lot): {len(cand_lots)}")
print(f"  covered (MSB Treasure / NpcParam): {len(cand_lots & covered)}")
print(f"  NEW (upper bound on added markers): {len(new)}")
# category breakdown of NEW
def cat_of(lot):
    r=ilm.get(lot) or ile.get(lot)
    if not r: return None
    for s in range(1,9):
        if r.get(f'lotItemId0{s}',0)>0: return r.get(f'lotItemCategory0{s}',0)
    return None
bc=Counter()
gn={1:E.read_fmg_names(E._read_from_bytes(E._bnd4_read,E.SoulsFormats.DCX.Decompress(str(E.MSGBND_PATH)),'.bnd'),'GoodsName.fmg')}
for lot in new:
    c=cat_of(lot); bc[c]+=1
print("  NEW by ItemLotParam category (1=goods,2=weapon,3=armor,4=talisman,5=gem):", dict(bc))
