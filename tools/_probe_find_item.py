"""Where does the DISK place item X? Given (cat,id), find every ItemLotParam_map/_enemy lot that
grants it, then for each lot report its placement: MSB Treasure event / Asset pickUpItemLotParamId /
NpcParam enemy-drop / EMEVD bank-2000 init. Answers "is the item really on the disk, and where?" vs
the bake's (0,0,0) fallback marker.  Usage: py _probe_find_item.py <cat> <id>   (cat 2=weapon,1=goods)"""
import sys, os, struct, tempfile
import extract_all_items as E, config
from pathlib import Path
from System import Array, Object
from System.IO import File as SysFile
CAT, IID = int(sys.argv[1]), int(sys.argv[2])
ERR = Path(config.require_err_mod_dir()); MSB=ERR/'map'/'MapStudio'
reg = E.SoulsFormats.SFUtil.DecryptERRegulation(str(ERR/'regulation.bin')); pdefs=E.load_paramdefs()
lf=set()
for i in range(1,9): lf.update([f'lotItemId0{i}',f'lotItemCategory0{i}'])
ilm=E.param_to_dict(E.read_param(reg,'ItemLotParam_map',pdefs),lf)
ile=E.param_to_dict(E.read_param(reg,'ItemLotParam_enemy',pdefs),lf)
def grants(row):
    for s in range(1,9):
        if row.get(f'lotItemId0{s}',0)==IID and row.get(f'lotItemCategory0{s}',0)==CAT: return True
    return False
map_lots={l for l,r in ilm.items() if grants(r)}
enemy_lots={l for l,r in ile.items() if grants(r)}
print(f"item cat={CAT} id={IID}: {len(map_lots)} map-lot(s) {sorted(map_lots)} | {len(enemy_lots)} enemy-lot(s) {sorted(enemy_lots)}")
targets=map_lots|enemy_lots
# NpcParam enemy refs
npc=E.param_to_dict(E.read_param(reg,'NpcParam',pdefs),{'itemLotId_map','itemLotId_enemy'})
for nid,f in npc.items():
    for k in ('itemLotId_map','itemLotId_enemy'):
        if f.get(k,0) in targets: print(f"  NpcParam {nid}.{k} -> lot {f[k]}")
# AEG asset pickups
aeg=E.param_to_dict(E.read_param(reg,'AssetEnvironmentGeometryParam',pdefs),{'pickUpItemLotParamId'})
r2l={rid:f.get('pickUpItemLotParamId',0) for rid,f in aeg.items()}
hit_rows={rid for rid,lot in r2l.items() if lot in targets}
# MSB scan: Treasure events + asset placements of hit_rows
for mp in sorted(MSB.glob('*_00.msb.dcx')):
    try: msb=E._read_from_bytes(E._msbe_read,E.SoulsFormats.DCX.Decompress(str(mp)),'.msb')
    except: continue
    stem=mp.name.replace('.msb.dcx','')
    for t in getattr(msb.Events,'Treasures',[]) or []:
        if int(t.ItemLotID) in targets:
            print(f"  MSB TREASURE {stem} lot={int(t.ItemLotID)} part={t.TreasurePartName}")
    for p in msb.Parts.Assets:
        m=str(p.ModelName)
        if not m.startswith('AEG'): continue
        try: a,b=m[3:].split('_'); rid=int(a)*1000+int(b)
        except: continue
        if rid in hit_rows: print(f"  MSB ASSET-PICKUP {stem} model={m} lot={r2l[rid]} part={p.Name}")
# EMEVD bank-2000
asm=E.asm; _rd=asm.GetType('SoulsFormats.EMEVD').GetMethod('Read',E.BindingFlags.Public|E.BindingFlags.Static|E.BindingFlags.FlattenHierarchy,None,E.Array[E.SysType]([E.SysType.GetType('System.String')]),None)
for p in sorted((ERR/'event').glob('*.emevd.dcx')):
    try:
        tmp=os.path.join(tempfile.gettempdir(),str(os.getpid())+'_fi.tmp'); SysFile.WriteAllBytes(tmp,E.SoulsFormats.DCX.Decompress(str(p)).ToArray()); em=_rd.Invoke(None,Array[Object]([tmp])); os.unlink(tmp)
    except: continue
    for ev in em.Events:
        for ins in ev.Instructions:
            if int(ins.Bank)!=2000: continue
            a=bytes(ins.ArgData); ints=[struct.unpack_from('<i',a,k*4)[0] for k in range(len(a)//4)]
            for k,v in enumerate(ints):
                if v in targets: print(f"  EMEVD {p.name.replace('.emevd.dcx','')} lot={v}@idx{k} tmpl={ints[1] if len(ints)>1 else '?'} args={ints}")
