"""For ALL 80 residual rows: resolve item -> is it sold infinite-stock (qty=-1) in ShopLineupParam?
Tells the exact count the 'drop shop-inf phantoms' runtime rule would remove, per category."""
import re
import extract_all_items as E, config
from pathlib import Path
LOG=Path(r"D:\DOWNLOAD\ERRv2.2.9.6-541-2-2-9-6-1780861369\ERRv2.2.9.6\dll\offline\logs\MapForGoblins.log")
ROW=re.compile(r'\[RESIDUAL-ROW\] cat="(?P<cat>[^"]+)" src=(?P<src>\w+) lot=(?P<lot>\d+) lt=(?P<lt>\d+) m\d+_\d+_\d+ key=(?P<key>-?\d+)')
rows={}
for ln in open(LOG,encoding='utf-8',errors='replace'):
    m=ROW.search(ln)
    if m: rows[int(m.group('lot'))]=(m.group('cat'),m.group('src'),int(m.group('lt')))
ERR=Path(config.require_err_mod_dir())
reg=E.SoulsFormats.SFUtil.DecryptERRegulation(str(ERR/'regulation.bin')); pdefs=E.load_paramdefs()
lf=set()
for i in range(1,9): lf.update([f'lotItemId0{i}',f'lotItemCategory0{i}'])
ilm=E.param_to_dict(E.read_param(reg,'ItemLotParam_map',pdefs),lf)
ile=E.param_to_dict(E.read_param(reg,'ItemLotParam_enemy',pdefs),lf)
shop=E.param_to_dict(E.read_param(reg,'ShopLineupParam',pdefs),{'equipId','equipType','sellQuantity'})
ET={2:0,3:1,4:2,1:3}
sold_inf=set(); sold_any=set()
for f in shop.values():
    k=(f.get('equipType',-99),f.get('equipId',0))
    sold_any.add(k)
    if f.get('sellQuantity',0)==-1: sold_inf.add(k)
def first_item(lot,lt):
    tbl=ile if lt==2 else ilm
    r=tbl.get(lot) or ilm.get(lot) or ile.get(lot)
    if not r: return None
    for s in range(1,9):
        iid=r.get(f'lotItemId0{s}',0); cat=r.get(f'lotItemCategory0{s}',0)
        if iid>0 and cat>0: return (cat,iid)
    return None
from collections import Counter
inf=Counter(); anyc=Counter(); none=Counter()
for lot,(cat,src,lt) in rows.items():
    it=first_item(lot,lt)
    if not it: none[cat]+=1; continue
    key=(ET.get(it[0]),it[1])
    if key in sold_inf: inf[cat]+=1
    elif key in sold_any: anyc[cat]+=1
    else: none[cat]+=1
print("=== residual markers sold INFINITE-stock in shop (drop candidates) ===")
for c,n in sorted(inf.items(),key=lambda x:-x[1]): print(f"  {n:>3}  {c}")
print(f"  TOTAL shop-inf = {sum(inf.values())} / {len(rows)}")
print("\n=== sold FINITE in shop (NOT auto-drop) ===")
for c,n in sorted(anyc.items(),key=lambda x:-x[1]): print(f"  {n:>3}  {c}")
print(f"  total finite-shop = {sum(anyc.values())}")
print(f"\n=== NOT in shop at all = {sum(none.values())} ===")
for c,n in sorted(none.items(),key=lambda x:-x[1]): print(f"  {n:>3}  {c}")
