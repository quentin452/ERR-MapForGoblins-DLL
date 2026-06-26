"""Second-path search for the 11 'not found' Golden Rune residual lots (9 orphan-enemy + 2 fallback).
For each: scan ALL EMEVD banks (not just 2000) for the lot id, and for every co-arg that is a
positionable MSB part across ALL tiles (incl non-_00 LOD), report it. Also flag enemy lots whose
ItemLotParam_enemy id matches a c-model 'common' drop pattern. Decides recoverable vs truly orphan."""
import os, struct, tempfile
import extract_all_items as E, config
from pathlib import Path
from System import Array, Object
from System.IO import File as SysFile
ERR=Path(config.require_err_mod_dir()); MSB=ERR/'map'/'MapStudio'
LOTS=[420110001,420126011,420141041,104512,430030413,430030403,430033423,430052433,430052443,12050510,35000580]
targets=set(LOTS)
# 1) ALL emevd banks: instructions referencing a target lot, collect co-args + bank/id
asm=E.asm; _rd=asm.GetType('SoulsFormats.EMEVD').GetMethod('Read',E.BindingFlags.Public|E.BindingFlags.Static|E.BindingFlags.FlattenHierarchy,None,E.Array[E.SysType]([E.SysType.GetType('System.String')]),None)
refs={l:[] for l in targets}
for p in sorted((ERR/'event').glob('*.emevd.dcx')):
    try:
        tmp=os.path.join(tempfile.gettempdir(),str(os.getpid())+'_p2.tmp'); SysFile.WriteAllBytes(tmp,E.SoulsFormats.DCX.Decompress(str(p)).ToArray()); em=_rd.Invoke(None,Array[Object]([tmp])); os.unlink(tmp)
    except: continue
    for ev in em.Events:
        for ins in ev.Instructions:
            a=bytes(ins.ArgData)
            if len(a)<4: continue
            ints=[struct.unpack_from('<i',a,k*4)[0] for k in range(len(a)//4)]
            for l in targets:
                if l in ints: refs[l].append((p.name.replace('.emevd.dcx',''),int(ins.Bank),int(ins.ID),ints))
# 2) collect candidate entity ids, index positions across ALL tiles
cands=set()
for l,rs in refs.items():
    for _,_,_,ints in rs:
        for v in ints:
            if 1000000000<=v<2000000000 and v not in targets: cands.add(v)
pos={}
if cands:
    for mp in sorted(MSB.glob('*.msb.dcx')):
        try: msb=E._read_from_bytes(E._msbe_read,E.SoulsFormats.DCX.Decompress(str(mp)),'.msb')
        except: continue
        st=mp.name.replace('.msb.dcx','')
        for typ in ('Assets','DummyAssets','Enemies','DummyEnemies'):
            for pp in getattr(msb.Parts,typ,[]) or []:
                try: eid=int(pp.EntityID)
                except: eid=0
                if eid in cands and eid not in pos:
                    P=pp.Position; pos[eid]=(st,typ,str(pp.ModelName))
for l in LOTS:
    rs=refs[l]
    if not rs: print(f"  lot={l:<11} NO emevd ref at all (any bank)"); continue
    placed=[(v,pos[v]) for _,_,_,ints in rs for v in ints if v in pos]
    banks=sorted(set((r[1],r[2]) for r in rs))
    if placed:
        v,(st,typ,mdl)=placed[0]
        print(f"  lot={l:<11} REC entity={v} {st} {typ} {mdl}  (banks={banks})")
    else:
        print(f"  lot={l:<11} emevd refs banks={banks} but NO positionable entity (any tile)")
