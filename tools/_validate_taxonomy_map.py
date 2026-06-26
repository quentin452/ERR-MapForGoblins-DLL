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
import json
from collections import Counter, defaultdict

import taxonomy_classifier as TC  # the shared offline mirror of the DLL classifier

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, 'data')

icon_table = json.load(open(os.path.join(DATA, 'item_icon_table.json'), encoding='utf-8'))
gts = json.load(open(os.path.join(DATA, 'goods_type_sortgroup.json'), encoding='utf-8'))
GTS = {int(k): tuple(v) for k, v in gts.items()}
EXCEPTION_IDS = TC.load_exception_ids(icon_table)


def proposed_cat(key):
    return TC.classify(key, GTS, EXCEPTION_IDS)[0]


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
