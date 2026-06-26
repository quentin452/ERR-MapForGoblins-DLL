"""For each Golden Rune residual lot: find its EMEVD bank-2000 init, then for EVERY other arg that
looks like an entity, check if it's a positionable MSB part (Asset/Enemy/Dummy with that EntityID).
If a positionable entity sits in the init → the position IS on disk → recoverable. Else accepted."""
import os, struct, tempfile
import extract_all_items as E, config
from pathlib import Path
from System import Array, Object
from System.IO import File as SysFile
ERR=Path(config.require_err_mod_dir()); MSB=ERR/'map'/'MapStudio'
LOTS=[12050510,35000580,1034430200,1034471300,1034471310,1034481300,1034481310,1035480100,
1038430100,1039410300,1039410310,1053560700,1053560710,1053560720,
420110001,420126011,420141041,104512,430030413,430030403,430033423,430052433,430052443]
targets=set(LOTS)
# 1) emevd inits referencing each lot (any bank-2000), collect co-args
asm=E.asm; _rd=asm.GetType('SoulsFormats.EMEVD').GetMethod('Read',E.BindingFlags.Public|E.BindingFlags.Static|E.BindingFlags.FlattenHierarchy,None,E.Array[E.SysType]([E.SysType.GetType('System.String')]),None)
lot_args={}
for p in sorted((ERR/'event').glob('*.emevd.dcx')):
    try:
        tmp=os.path.join(tempfile.gettempdir(),str(os.getpid())+'_gr.tmp'); SysFile.WriteAllBytes(tmp,E.SoulsFormats.DCX.Decompress(str(p)).ToArray()); em=_rd.Invoke(None,Array[Object]([tmp])); os.unlink(tmp)
    except: continue
    for ev in em.Events:
        for ins in ev.Instructions:
            if int(ins.Bank)!=2000: continue
            a=bytes(ins.ArgData); ints=[struct.unpack_from('<i',a,k*4)[0] for k in range(len(a)//4)]
            for v in ints:
                if v in targets and v not in lot_args:
                    lot_args[v]=(p.name.replace('.emevd.dcx',''),ints)
# 2) collect all candidate entity ids (10-digit map entities), build a positional MSB index for them
cands=set()
for lot,(tile,ints) in lot_args.items():
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
                    P=pp.Position; pos[eid]=(st,typ,str(pp.ModelName),P.X,P.Y,P.Z)
# 3) report
rec=acc=0
for lot in LOTS:
    if lot not in lot_args:
        print(f"  lot={lot:<11} *** no emevd bank-2000 init (fallback/orphan) ***"); acc+=1; continue
    tile,ints=lot_args[lot]
    placed=[(v,pos[v]) for v in ints if v in pos]
    if placed:
        rec+=1; v,(st,typ,mdl,x,y,z)=placed[0]
        print(f"  lot={lot:<11} {tile:14s} REC entity={v} -> {st} {typ} {mdl} ({x:.0f},{y:.0f},{z:.0f})")
    else:
        acc+=1
        ents=[v for v in ints if 1000000000<=v<2000000000 and v not in targets]
        print(f"  lot={lot:<11} {tile:14s} emevd but NO positionable entity (cands={ents})")
print(f"\n=== {rec} recoverable (positionable entity) · {acc} accepted (no position) of {len(LOTS)} ===")
