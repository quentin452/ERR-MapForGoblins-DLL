#!/usr/bin/env python3
"""Phase-3 VALIDATION oracle: prove the proposed (gType,sg)+exceptions scheme reproduces the
current bake's per-item categories EXACTLY for every placed item.

Reads:
  - data/item_icon_table.json     {encoded_key: [iconId, category_name]}  (the bake's ground truth)
  - data/goods_type_sortgroup.json {goods_id: [gType, sg]}                (from _probe_taxonomy_map)

For each baked item it computes the PROPOSED category (non-goods by key range; else curated
exception id; else (gType,sg) map; else gType fallback; else catch-all) and diffs it against the
baked category. ZERO mismatches among non-exception items => the live (gType,sg) map is faithful and
the C++ port is safe. Exception-cat items match by construction (they ARE the curated table); they're
listed so the exception table's size is explicit.

Run:  py -3.14 tools/_validate_taxonomy_map.py   (no SoulsFormats needed — pure json)
"""
import os
import sys
import json
from collections import Counter, defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, 'data')

icon_table = json.load(open(os.path.join(DATA, 'item_icon_table.json'), encoding='utf-8'))
gts = json.load(open(os.path.join(DATA, 'goods_type_sortgroup.json'), encoding='utf-8'))
GTS = {int(k): tuple(v) for k, v in gts.items()}

# ── Proposed scheme ───────────────────────────────────────────────────────
# Categories handled ONLY by a curated id-list (the splits / grab-bags ER doesn't encode).
EXCEPTION_CATS = {
    'Loot - Golden Runes (Low)',
    'Loot - Smithing Stones (Low)', 'Loot - Smithing Stones (Rare)',
    'Loot - Great Gloveworts',
    'Loot - Rune Arcs', 'Loot - Prattling Pates', 'Loot - MP-Fingers', 'Loot - Rada Fruit',
    'Key - Celestial Dew', 'Key - Imbued Sword Keys', 'Loot - Stonesword Keys',
    'Quest - Deathroot', 'Quest - Seedbed Curses',
    'Key - Whetblades', 'Key - Larval Tears', 'Key - Lost Ashes', 'Loot - Dragon Hearts',
    'Key - Scadutree Fragments', 'Key - Seeds Tears Ashes',
    'Magic - Memory Stones', 'Magic - Prayerbooks',
    'Quest - Progression',
    'Reforged - Items', 'Reforged - Fortunes', 'Reforged - Sealed Curios',
}

# (gType, sg) -> category for the BULK (mono-category cells; tails ride the default).
CELL_MAP = {
    (0, 20): 'Loot - Consumables', (0, 61): 'Loot - Consumables',
    (0, 50): 'Loot - Throwables',
    (0, 70): 'Loot - Greases',
    (0, 60): 'Loot - Reusables',
    (0, 80): 'Loot - Utilities',                 # Pates = exception
    (0, 10): 'Loot - Stat Boosts',               # Rune Arc 150 = exception
    (0, 100): 'Loot - Golden Runes', (0, 101): 'Loot - Golden Runes',  # Low = exception
    (0, 15): 'Equipment - Spirits',              # ERR renumbered spirit-ash goods
    (1, 80): 'Loot - Bell-Bearings', (1, 90): 'Loot - Bell-Bearings',
    (1, 200): 'Key - Cookbooks', (1, 205): 'Key - Cookbooks',
    (1, 100): 'Magic - Prayerbooks',             # 8867 = exception
    (10, 20): 'Key - Crystal Tears',
    (11, 30): 'Key - Pots n Perfumes',
    (14, 40): 'Loot - Gloveworts', (14, 41): 'Loot - Gloveworts',     # Great = exception
    (14, 19): 'Loot - Smithing Stones', (14, 20): 'Loot - Smithing Stones',
    (14, 30): 'Loot - Smithing Stones',          # Low/Rare = exception
}
# goodsType -> category fallback (broadest families).
GTYPE_MAP = {
    2: 'Loot - Crafting Materials',
    5: 'Magic - Sorceries', 17: 'Magic - Sorceries',
    16: 'Magic - Incantations', 18: 'Magic - Incantations',
    7: 'Equipment - Spirits', 8: 'Equipment - Spirits',
}
GTYPE_DEFAULT = {1: 'Quest - Progression'}  # key-item tail -> Quest/Key Items bucket

# Non-goods equip categories by encoded-key range (mirrors classify_item_live; no params needed).
def nongoods_cat(key):
    if key >= 500000000:
        return None  # goods
    if key >= 400000000:
        return 'Equipment - Ashes of War'
    if key >= 300000000:
        return 'Equipment - Talismans'
    if key >= 200000000:
        return 'Equipment - Armour'
    if key >= 100000000:
        return 'Loot - Ammo' if (key - 100000000) >= 50000000 else 'Equipment - Armaments'
    # Legacy raw-50M ammo key (the bake stores ammo here; the live runtime uses +100M).
    if key >= 50000000:
        return 'Loot - Ammo'
    return None


# Build the curated exception id-set from the bake (the items whose baked category is exception-only).
EXCEPTION_IDS = {}
for k, (icon, cat) in icon_table.items():
    key = int(k)
    if key >= 500000000 and cat in EXCEPTION_CATS:
        EXCEPTION_IDS[key - 500000000] = cat


def proposed_cat(key):
    ng = nongoods_cat(key)
    if ng is not None:
        return ng
    gid = key - 500000000
    if gid in EXCEPTION_IDS:
        return EXCEPTION_IDS[gid]
    cell = GTS.get(gid)
    if cell is not None:
        if cell in CELL_MAP:
            return CELL_MAP[cell]
        if cell[0] in GTYPE_MAP:
            return GTYPE_MAP[cell[0]]
        if cell[0] in GTYPE_DEFAULT:
            return GTYPE_DEFAULT[cell[0]]
    return None  # catch-all (LootCraftingMaterials)


# ── Diff proposed vs baked ────────────────────────────────────────────────
mismatches = []
exc_count = 0
ok = 0
for k, (icon, baked) in icon_table.items():
    key = int(k)
    prop = proposed_cat(key)
    if key >= 500000000 and (key - 500000000) in EXCEPTION_IDS:
        exc_count += 1
        continue  # matches by construction
    if prop == baked:
        ok += 1
    else:
        mismatches.append((key, baked, prop))

print(f'baked items: {len(icon_table)}')
print(f'  exception-table (curated id-list): {exc_count}')
print(f'  map/gType-handled & MATCH:         {ok}')
print(f'  MISMATCHES:                        {len(mismatches)}')
print()
if mismatches:
    by = defaultdict(list)
    for key, baked, prop in mismatches:
        by[(baked, prop)].append(key)
    for (baked, prop), keys in sorted(by.items(), key=lambda x: -len(x[1])):
        gids = [str(k - 500000000) if k >= 500000000 else f'(key {k})' for k in sorted(keys)]
        print(f'  baked="{baked}"  proposed="{prop}"  x{len(keys)}: {", ".join(gids[:20])}'
              + ('' if len(gids) <= 20 else f' … +{len(gids)-20}'))
else:
    print('  ✅ ZERO mismatches — the (gType,sg)+exception scheme reproduces the bake exactly.')

print()
print(f'Exception table size: {len(EXCEPTION_IDS)} ids across '
      f'{len(set(EXCEPTION_IDS.values()))} categories')
print('  per category:', dict(Counter(EXCEPTION_IDS.values()).most_common()))
