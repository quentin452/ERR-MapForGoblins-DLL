#!/usr/bin/env python3
"""Identify the 1 baked-only World - Maps residual: real EMEVD-granted map, or a phantom?

The disk pass (build_disk_maps_markers) emits map fragments (EquipParamGoods.sortGroupId ∈ {190,191})
placed as MSB Events.Treasures. baked=1/disk=23 → one map goods has NO MSB Treasure. The runtime
comment claims it's the Altus Plateau map, granted by EMEVD. Verify: enumerate every map goods, find
its ItemLotParam_map lot, check MSB-Treasure placement + EMEVD-award presence → name the residual and
classify it (real recoverable vs bake phantom).
"""
import os, struct, tempfile
import extract_all_items as E
import config
from pathlib import Path
from System import Array, Object
from System.IO import File as SysFile

ERR = Path(config.require_err_mod_dir())
MSB_DIR = ERR / 'map' / 'MapStudio'
print("=== regulation: map goods + lots ===")
reg = E.SoulsFormats.SFUtil.DecryptERRegulation(str(ERR / 'regulation.bin'))
pdefs = E.load_paramdefs()
goods = E.param_to_dict(E.read_param(reg, 'EquipParamGoods', pdefs), {'sortGroupId'})
map_goods = {gid for gid, f in goods.items() if f.get('sortGroupId', -1) in (190, 191)}
print(f"  {len(map_goods)} map goods (sortGroupId 190/191)")

lf = {'getItemFlagId'}
for i in range(1, 9): lf.update([f'lotItemId0{i}', f'lotItemCategory0{i}'])
ilm = E.param_to_dict(E.read_param(reg, 'ItemLotParam_map', pdefs), lf)
# map goods id -> list of ItemLotParam_map lots that grant it (cat 1 goods)
goods_to_lots = {g: [] for g in map_goods}
for lot, r in ilm.items():
    for s in range(1, 9):
        if r.get(f'lotItemCategory0{s}', 0) == 1 and r.get(f'lotItemId0{s}', 0) in map_goods:
            goods_to_lots[r[f'lotItemId0{s}']].append(lot)

# FMG names
try:
    tmp = os.path.join(tempfile.gettempdir(), f"mfg_msg_{os.getpid()}.bnd")
    SysFile.WriteAllBytes(tmp, E.SoulsFormats.DCX.Decompress(str(E.MSGBND_PATH)).ToArray())
    msg = E._bnd4_read.Invoke(None, Array[Object]([tmp])); os.unlink(tmp)
    gname = E.read_fmg_names(msg, 'GoodsName.fmg')
except Exception:
    gname = {}

print("=== MSB treasure lots ===")
treasure_lots = set()
for mp in sorted(MSB_DIR.glob('*.msb.dcx')):
    if mp.name.endswith('_99.msb.dcx'): continue
    try: msb = E._read_from_bytes(E._msbe_read, E.SoulsFormats.DCX.Decompress(str(mp)), '.msb')
    except Exception: continue
    for ev in msb.Events.Treasures:
        try: treasure_lots.add(int(ev.ItemLotID))
        except Exception: pass

all_map_lots = {l for lots in goods_to_lots.values() for l in lots}
print("=== EMEVD: all bank-2000 init lots (any arg) ===")
asm = E.asm
_er = asm.GetType('SoulsFormats.EMEVD').GetMethod(
    'Read', E.BindingFlags.Public | E.BindingFlags.Static | E.BindingFlags.FlattenHierarchy,
    None, E.Array[E.SysType]([E.SysType.GetType('System.String')]), None)
emevd_lots = {}  # lot -> (file, tmpl)
for p in sorted((ERR / 'event').glob('*.emevd.dcx')):
    name = p.name.replace('.emevd.dcx', '')
    try:
        tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_m.tmp')
        SysFile.WriteAllBytes(tmp, E.SoulsFormats.DCX.Decompress(str(p)).ToArray())
        emevd = _er.Invoke(None, Array[Object]([tmp])); os.unlink(tmp)
    except Exception: continue
    for ev in emevd.Events:
        for ins in ev.Instructions:
            if int(ins.Bank) != 2000: continue
            ab = bytes(ins.ArgData); n = len(ab)//4
            if n < 2: continue
            ints = [struct.unpack_from('<i', ab, k*4)[0] for k in range(n)]
            tmpl = ints[1]
            for v in ints:
                if v in all_map_lots and v not in emevd_lots:
                    emevd_lots[v] = (name, tmpl)

print(f"\n{'GOODS':<8} {'NAME':<34} TREASURE? EMEVD?   lots")
on_treasure_goods = set()
for g in sorted(map_goods):
    lots = goods_to_lots[g]
    nm = gname.get(g, f'?{g}')
    tr = [l for l in lots if l in treasure_lots]
    em = [(l, emevd_lots[l]) for l in lots if l in emevd_lots]
    if tr: on_treasure_goods.add(g)
    tflag = 'YES' if tr else 'no '
    eflag = ('T%d@%s' % (em[0][1][1], em[0][1][0])) if em else '—'
    print(f"{g:<8} {nm:<34} {tflag:<9} {eflag:<9} lots={lots[:4]}")

print("\n=== BAKED-ONLY map(s) (no MSB Treasure → the disk pass can't place them) ===")
baked_only = [g for g in sorted(map_goods) if g not in on_treasure_goods and goods_to_lots[g]]
no_lot = [g for g in sorted(map_goods) if not goods_to_lots[g]]
for g in baked_only:
    lots = goods_to_lots[g]
    em = [(l, emevd_lots[l]) for l in lots if l in emevd_lots]
    verdict = ('REAL — EMEVD-granted (%s)' % em[0][1][0]) if em else 'PHANTOM? no treasure AND no EMEVD award'
    print(f"  {g} '{gname.get(g, g)}' lots={lots} -> {verdict}")
if no_lot:
    print(f"\n  map goods with NO ItemLotParam_map lot at all: {[(g, gname.get(g,g)) for g in no_lot]}")
print(f"\nsummary: {len(map_goods)} map goods · {len(on_treasure_goods)} on MSB treasure (disk) · "
      f"{len(baked_only)} baked-only · {len(no_lot)} no-lot")

