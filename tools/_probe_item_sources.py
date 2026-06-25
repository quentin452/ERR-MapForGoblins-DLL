#!/usr/bin/env python3
"""For each of the 7 enemy-mislabel items: is the SAME item granted by any OTHER ItemLotParam lot
(besides its phantom enemy-lot)? If yes → the item has another potential map source (plausibly
covered). If ONLY the phantom lot grants it → it's a unique source → dropping the phantom would
leave that item with no loot marker."""
import sys
import extract_all_items as E
import config
from pathlib import Path

# (target itemId, type, the phantom enemy-lot that the bake mis-tagged)
TARGETS = [
    (33200000, 'weapon', 113601,    'Academy Glintstone Staff'),
    (34030000, 'weapon', 435300706, 'Gravel Stone Seal'),
    (837000,   'protector', 111001, "Witch's Glintstone Crown (Broken)"),
    (23000000, 'weapon', 391000704, "Prelate's Inferno Crozier"),
    (12530000, 'weapon', 508000701, "Bloodfiend's Arm"),
    (3010300,  'protector', 317000708, "Gaius's Greaves"),
    (2010100,  'goods', 532000701,  'Revered Spirit Ash'),
]
TARGET_IDS = {t[0] for t in TARGETS}

reg = E.SoulsFormats.SFUtil.DecryptERRegulation(str(Path(config.ERR_MOD_DIR) / 'regulation.bin'))
pdefs = E.load_paramdefs()
slots = set()
for i in range(1, 9):
    slots.add(f'lotItemId0{i}'); slots.add(f'lotItemCategory0{i}')

# itemId -> list of (table, lotId) that grant it
grants = {tid: [] for tid in TARGET_IDS}
for tbl in ('ItemLotParam_map', 'ItemLotParam_enemy'):
    rows = E.param_to_dict(E.read_param(reg, tbl, pdefs), slots)
    for lotId, f in rows.items():
        for i in range(1, 9):
            iid = f.get(f'lotItemId0{i}', 0)
            if iid in TARGET_IDS and iid > 0:
                grants[iid].append((tbl.replace('ItemLotParam_', ''), lotId))

print(f"{'item':<34} {'phantom lot':>11}  other-granting-lots (besides the phantom)")
for tid, typ, phantom, name in TARGETS:
    others = [(t, l) for (t, l) in grants[tid] if l != phantom]
    tag = "UNIQUE → only the phantom" if not others else f"{len(others)} OTHER lot(s)"
    print(f"{name:<34} {phantom:>11}  {tag}")
    for t, l in others[:6]:
        print(f"{'':<34} {'':>11}    - {t}:{l}")
