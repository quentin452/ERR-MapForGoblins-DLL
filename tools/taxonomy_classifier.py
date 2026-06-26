#!/usr/bin/env python3
"""Shared OFFLINE mirror of the DLL's Phase-3 item classifier (goblin_inject.cpp
item_marker_category + category_from_taxonomy + classify_item_live).

Single source for the offline tools (_validate_taxonomy_map.py = equivalence oracle,
unplaced_items.py = known-but-unplaced report) so they never drift from each other.
Keep this in lock-step with goblin_inject.cpp when the C++ map changes.

Categories are the LOOT_CATEGORIES names (the bake's ground truth); map None to the
catch-all. Build the exception id->category map once from item_icon_table.json.
"""
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, 'data')

# Categories handled ONLY by a curated id-list (the splits/grab-bags ER's taxonomy can't express).
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
CATCH_ALL = 'Loot - Crafting Materials'


def nongoods_cat(key):
    """Non-goods equip category by encoded-key range (mirrors classify_item_live)."""
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
    if key >= 50000000:
        return 'Loot - Ammo'  # legacy raw-50M ammo key (old table)
    return None


def load_exception_ids(icon_table=None):
    """Build {goods_id: category_name} for the curated exceptions, from item_icon_table.json."""
    if icon_table is None:
        with open(os.path.join(DATA, 'item_icon_table.json'), encoding='utf-8') as f:
            icon_table = json.load(f)
    out = {}
    for k, v in icon_table.items():
        key = int(k)
        if key >= 500000000 and v[1] in EXCEPTION_CATS:
            out[key - 500000000] = v[1]
    return out


def classify(key, gts, exception_ids):
    """Encoded item key -> (category_name, confident). category None never returned;
    confident=False marks the default/catch-all tail (gType1->Quest, or LootCraftingMaterials).
    `gts` = {goods_id(int): (gType, sg)}; `exception_ids` = load_exception_ids()."""
    ng = nongoods_cat(key)
    if ng is not None:
        return ng, True
    gid = key - 500000000
    if gid in exception_ids:
        return exception_ids[gid], True
    if 300000 <= gid <= 399999:
        return 'Equipment - Spirits', True   # ERR renumbered spirit-ash goods (id range)
    cell = gts.get(gid)
    if cell is not None:
        if tuple(cell) in CELL_MAP:
            return CELL_MAP[tuple(cell)], True
        gt = cell[0]
        if gt in GTYPE_MAP:
            return GTYPE_MAP[gt], True
        if gt in GTYPE_DEFAULT:
            return GTYPE_DEFAULT[gt], False     # gType1 key-item tail
    return CATCH_ALL, False                     # catch-all (uncertain surface)
