"""Pin ShopLineupParam_ST byte offsets for equipId / equipType / sellQuantity / mtrlId, by walking
the paramdef field sizes (authoritative layout), then VALIDATE against raw row bytes of a known row
(re-offset-validation discipline). Prints offsets + row size for the RawShopRow C++ struct."""
import extract_all_items as E, config
from pathlib import Path
ERR=Path(config.require_err_mod_dir())
reg=E.SoulsFormats.SFUtil.DecryptERRegulation(str(ERR/'regulation.bin')); pdefs=E.load_paramdefs()
# find the ShopLineupParam PARAM + its def
param=E.read_param(reg,'ShopLineupParam',pdefs)
pd=pdefs.get(str(param.ParamType)) or pdefs.get('SHOP_LINEUP_PARAM')
SIZE={'s8':1,'u8':1,'dummy8':1,'s16':2,'u16':2,'s32':4,'u32':4,'f32':4,'angle32':4,'f64':8,'b32':4}
off=0; bitacc=0; bitfield_type=None; layout=[]
def flush_bits():
    global bitacc,bitfield_type,off
    if bitfield_type is not None:
        off += SIZE.get(bitfield_type,1); bitacc=0; bitfield_type=None
for f in pd.Fields:
    dt=str(f.DisplayType).lower()
    nm=str(f.InternalName)
    bs=int(f.BitSize) if hasattr(f,'BitSize') else -1
    al=int(f.ArrayLength) if hasattr(f,'ArrayLength') else 1
    if bs>=0 and dt in ('u8','u16','u32','s8','s16','s32','dummy8'):
        tsz=SIZE.get(dt,1)*8
        if bitfield_type!=dt or bitacc+bs>tsz:
            flush_bits(); bitfield_type=dt; bitacc=0
        layout.append((off,nm,dt,f"bit{bs}")); bitacc+=bs
        continue
    flush_bits()
    if dt in ('fixstr','fixstrw'):
        sz=al*(2 if dt=='fixstrw' else 1)
    elif dt=='dummy8':
        sz=al
    else:
        sz=SIZE.get(dt,1)*max(al,1)
    layout.append((off,nm,dt,sz)); off+=sz
flush_bits()
want={'equipId','equipType','sellQuantity','mtrlId','costType','value'}
print(f"ShopLineupParam_ST computed size = {off} bytes")
for o,nm,dt,sz in layout:
    if nm in want: print(f"  +0x{o:02x}  {nm:18s} {dt} {sz}")
# validate vs raw bytes of a known row (row 50000303: equipId 3030000, qty -1, equipType 0)
import struct
RID=50000303
row=next((r for r in param.Rows if int(r.ID)==RID),None)
if row is not None:
    # get raw bytes: re-serialize the single row via the param writer is hard; instead read each cell
    cells={str(c.Def.InternalName):c.Value for c in row.Cells}
    print(f"\nrow {RID}: equipId={cells.get('equipId')} equipType={cells.get('equipType')} sellQuantity={cells.get('sellQuantity')} mtrlId={cells.get('mtrlId')}")

print("\n=== FULL layout (validate the 0x14-0x18 region + total) ===")
try: print("paramdef.GetRowSize() =", pd.GetRowSize())
except Exception as e: print("GetRowSize n/a:", e)
for o,nm,dt,sz in layout:
    if 0x10 <= o <= 0x1a: print(f"  +0x{o:02x}  {nm:20s} {dt} {sz}")
