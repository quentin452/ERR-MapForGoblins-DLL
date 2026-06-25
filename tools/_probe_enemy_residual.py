#!/usr/bin/env python3
"""DECISIVE offset-bug-vs-drift probe for the 35 uncovered baked-Enemy lots.

Paramdef-authoritative (field NAMES, not hardcoded 0x30/0x34): scan the DEPLOYED mod's
NpcParam for any row referencing each of the 35 lots via itemLotId_enemy / itemLotId_map,
and confirm they exist as ItemLotParam_enemy rows. Also compares the bake-source regulation
(config.ERR_MOD_DIR) vs the deployed one to expose drift.
"""
import sys
import extract_all_items as E  # reuses its SoulsFormats bootstrap + helpers (sets utf-8 stdout)
import config
from pathlib import Path

DEPLOYED = Path(r"D:\DOWNLOAD\ERRv2.2.9.6-541-2-2-9-6-1780861369\ERRv2.2.9.6\mod\regulation.bin")

LOTS = [333062021,333065001,532000701,435300706,391000704,113601,508000701,337000805,
        111001,317000708,336000707,463000705,435110106,420110001,420126011,420141041,
        104512,430030413,430030403,430033423,430052443,430052433,581000703,214000991,
        402056031,402056001,402056021,402056011,402030021,402030001,402040001,402022011,
        402022001,402020001,402031001]


def scan(reg_path, label):
    print(f"\n=== {label}: {reg_path} ===")
    if not reg_path.exists():
        print("  MISSING"); return None
    bnd = E.SoulsFormats.SFUtil.DecryptERRegulation(str(reg_path))
    pdefs = E.load_paramdefs()
    npc = E.read_param(bnd, 'NpcParam', pdefs)
    npc_lots = E.param_to_dict(npc, {'itemLotId_map', 'itemLotId_enemy'})
    enemy30 = {}   # lot -> [npc ids] via itemLotId_enemy
    map34 = {}     # lot -> [npc ids] via itemLotId_map
    for nid, f in npc_lots.items():
        le = f.get('itemLotId_enemy', 0); lm = f.get('itemLotId_map', 0)
        if le and le > 0: enemy30.setdefault(le, []).append(nid)
        if lm and lm > 0: map34.setdefault(lm, []).append(nid)
    print(f"  NpcParam rows={len(npc_lots)}  distinct itemLotId_enemy={len(enemy30)}  "
          f"distinct itemLotId_map={len(map34)}")
    # ItemLotParam_enemy row presence (do the lot rows exist at all?)
    ile = E.read_param(bnd, 'ItemLotParam_enemy', pdefs)
    ile_ids = set(int(r.ID) for r in ile.Rows)
    ilm = E.read_param(bnd, 'ItemLotParam_map', pdefs)
    ilm_ids = set(int(r.ID) for r in ilm.Rows)

    in30 = in34 = in_enemy_table = in_map_table = nowhere = 0
    for L in LOTS:
        a = L in enemy30; b = L in map34
        if a: in30 += 1
        if b: in34 += 1
        if L in ile_ids: in_enemy_table += 1
        if L in ilm_ids: in_map_table += 1
        if not a and not b: nowhere += 1
    print(f"  Of the {len(LOTS)} baked-Enemy lots (paramdef-authoritative):")
    print(f"    {in30} referenced by some NpcParam.itemLotId_enemy")
    print(f"    {in34} referenced by some NpcParam.itemLotId_map")
    print(f"    {nowhere} referenced by NEITHER NpcParam field")
    print(f"    {in_enemy_table} exist as ItemLotParam_enemy rows | {in_map_table} as ItemLotParam_map rows")
    return dict(enemy30=enemy30, map34=map34, ile=ile_ids, ilm=ilm_ids)


print("config.ERR_MOD_DIR (bake source) =", config.ERR_MOD_DIR)
src = None
try:
    if config.ERR_MOD_DIR:
        src = scan(Path(config.ERR_MOD_DIR) / 'regulation.bin', "BAKE-SOURCE regulation")
except Exception as ex:
    print("  bake-source scan failed:", ex)
dep = scan(DEPLOYED, "DEPLOYED regulation")
van = None
try:
    if config.GAME_DIR:
        van = scan(Path(config.GAME_DIR) / 'regulation.bin', "VANILLA regulation")
        if van:
            in30 = sum(1 for L in LOTS if L in van['enemy30'])
            in34 = sum(1 for L in LOTS if L in van['map34'])
            print(f"\n  >>> VANILLA: {in30}/{len(LOTS)} referenced via itemLotId_enemy, "
                  f"{in34}/{len(LOTS)} via itemLotId_map <<<")
            print("  per-lot (vanilla): lot  enemy30(npcIds)  map34(npcIds)")
            for L in LOTS:
                e = van['enemy30'].get(L); m = van['map34'].get(L)
                if e or m:
                    print(f"    {L:>10}  enemy30={e}  map34={m}")
except Exception as ex:
    import traceback; traceback.print_exc()

# Resolve the ITEM NAME of each baked lot (runtime-captured key + tile) for a mapgenie/Vanilla cross-check.
# key encoding (classify_item_live): >=500M goods, >=400M gem, >=300M accessory, >=200M protector, >=100M weapon.
ROWS = [  # (lot, runtime key, baked tile) — from the [ENEMY-MARKERS] log
 (333062021,500008185,'m12_1_0'),(333065001,500008185,'m12_2_0'),(532000701,502010100,'m20_1_0'),
 (435300706,134030000,'m11_0_0'),(391000704,123000000,'m60_35_54'),(113601,133200000,'m60_39_39'),
 (508000701,112530000,'m61_45_41'),(337000805,200301000,'m12_2_0'),(111001,200837000,'m60_34_50'),
 (317000708,203010300,'m61_49_49'),(336000707,300001171,'m12_1_0'),(463000705,300001191,'m12_3_0'),
 (435110106,400060300,'m60_43_39'),(420110001,500002902,'m31_17_0'),(420126011,500002905,'m39_20_0'),
 (420141041,500002906,'m60_51_39'),(104512,500002913,'m60_39_44'),(430030413,500002909,'m60_39_53'),
 (430030403,500002909,'m60_40_54'),(430033423,500002911,'m60_43_52'),(430052443,500002912,'m60_49_55'),
 (430052433,500002912,'m60_49_55'),(581000703,500001290,'m61_47_39'),(214000991,500003010,'m35_0_0'),
 (402056031,500010918,'m15_0_0'),(402056001,500010918,'m15_0_0'),(402056021,500010918,'m15_0_0'),
 (402056011,500010918,'m15_0_0'),(402030021,500010913,'m30_8_0'),(402030001,500010913,'m30_8_0'),
 (402040001,500010913,'m31_11_0'),(402022011,500010911,'m60_35_41'),(402022001,500010911,'m60_35_41'),
 (402020001,500010911,'m60_37_42'),(402031001,500010913,'m60_39_54')]

def _read_keep(read_method, data, suffix):  # like E._read_from_bytes but tolerate the temp-unlink lock
    import os, tempfile
    from System import Array, Object
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + f'_keep{suffix}')
    from System.IO import File as SysFile
    SysFile.WriteAllBytes(tmp, data.ToArray() if hasattr(data,'ToArray') else data)
    return read_method.Invoke(None, Array[Object]([tmp]))

try:
    msgbnd = _read_keep(E._bnd4_read, E.SoulsFormats.DCX.Decompress(str(E.MSGBND_PATH)), '.bnd')
    fmg = {'weapon':E.read_fmg_names(msgbnd,'WeaponName.fmg'),
           'protector':E.read_fmg_names(msgbnd,'ProtectorName.fmg'),
           'accessory':E.read_fmg_names(msgbnd,'AccessoryName.fmg'),
           'gem':E.read_fmg_names(msgbnd,'GemName.fmg'),
           'goods':E.read_fmg_names(msgbnd,'GoodsName.fmg')}
    def name_of(key):
        if key>=500000000: return 'goods', key-500000000, fmg['goods']
        if key>=400000000: return 'gem', key-400000000, fmg['gem']
        if key>=300000000: return 'accessory', key-300000000, fmg['accessory']
        if key>=200000000: return 'protector', key-200000000, fmg['protector']
        if key>=100000000: return 'weapon', key-100000000, fmg['weapon']
        return '?', key, {}
    print("\n  === ITEM NAMES of the 35 stale baked-Enemy lots (for mapgenie / Vanilla cross-check) ===")
    print(f"  {'lot':>10} {'tile':>10} {'category':>10} {'itemId':>9}  name")
    for lot,key,tile in ROWS:
        cat,iid,tbl = name_of(key)
        nm = tbl.get(iid, '(no FMG name)')
        print(f"  {lot:>10} {tile:>10} {cat:>10} {iid:>9}  {nm}")
except Exception as ex:
    import traceback; traceback.print_exc()
