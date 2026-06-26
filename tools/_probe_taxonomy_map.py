#!/usr/bin/env python3
"""Phase-3 PROBE: build the (goodsType, sortGroupId) -> Category candidate map and list conflicts.

Goal: prove whether ER's own item taxonomy (EquipParamGoods.goodsType@+0x3e + sortGroupId@+0x72)
can REPLACE the per-item ITEM_ICONS *category* column with a small live (gType,sg)->Category map.

Method (data-driven, validate BEFORE coding C++):
  1. Read EVERY EquipParamGoods row -> (id, goodsType, sortGroupId)  [the live-readable fields].
  2. For each goods id, run the SAME ordered LOOT_CATEGORIES classifier the bake uses
     (generate_loot_massedit, first match wins) on it as a singleton -> the category it lands in.
  3. Aggregate per (gType, sg) cell -> Counter(category). A cell that yields >1 category is a
     CONFLICT (sortGroupId alone can't decide it -> needs a curated id sub-rule / stays in ITEM_ICONS).
  4. Emit a markdown report: the clean (pure) cells = drift-free live-map rows; the conflicted cells
     = the curated exception surface ITEM_ICONS must keep.

This is OFFLINE analysis only. It writes:
  - data/goods_type_sortgroup.json   {id: [gType, sg]} for every goods row (reusable cache)
  - docs/taxonomy_map_probe.md        the human report (mapping + conflicts)

Run (err profile):
  PYTHONUTF8=1 PYTHONPATH="C:/Users/iamacat/AppData/Local/Temp/mfg_aux" MFG_PROFILE=err \
    py -3.14 tools/_probe_taxonomy_map.py
"""
import os
import sys
import json
import tempfile
from collections import Counter, defaultdict

import config
# extract_all_items wires up the pythonnet/SoulsFormats bridge, paramdefs, FMG reader.
import extract_all_items as E
# generate_loot_massedit owns the curated ground-truth classifier (LOOT_CATEGORIES).
import generate_loot_massedit as G
from System import Array, Object
from System.IO import File as SysFile

ROOT = config.PROJECT_DIR
OUT_JSON = config.DATA_DIR / 'goods_type_sortgroup.json'
OUT_MD = ROOT / 'docs' / 'taxonomy_map_probe.md'

ERR_ONLY_CATS = getattr(G, 'ERR_ONLY_CATS',
                        {'Reforged - Items', 'Reforged - Fortunes', 'Reforged - Sealed Curios'})


def read_goods_rows():
    """Every EquipParamGoods row -> {id: (goodsType, sortGroupId)} via SoulsFormats."""
    src = config.require_err_mod_dir()
    bnd = E.SoulsFormats.SFUtil.DecryptERRegulation(str(src / 'regulation.bin'))
    defs = E.load_paramdefs()
    goods = E.read_param(bnd, 'EquipParamGoods', defs)
    if goods is None:
        sys.exit('EquipParamGoods not found in regulation')
    rows = {}
    for r in goods.Rows:
        rid = int(r.ID)
        gtype = sortg = None
        for c in r.Cells:
            nm = str(c.Def.InternalName)
            if nm == 'goodsType':
                gtype = int(c.Value)
            elif nm == 'sortGroupId':
                sortg = int(c.Value)
        rows[rid] = (gtype if gtype is not None else -1,
                     sortg if sortg is not None else -1)
    return rows


def load_goods_names():
    """{goods_id: name} from GoodsName FMG (+ DLC variants)."""
    tmp = os.path.join(tempfile.gettempdir(), f'mfg_tax_{os.getpid()}.bnd')
    SysFile.WriteAllBytes(tmp, E.SoulsFormats.DCX.Decompress(str(E.MSGBND_PATH)).ToArray())
    msg = E._bnd4_read.Invoke(None, Array[Object]([tmp]))
    try:
        os.unlink(tmp)
    except OSError:
        pass
    return E.read_fmg_names(msg, 'GoodsName.fmg')


def classify(item_id, name):
    """Run the ordered curated classifier on a singleton goods item -> category name or None."""
    single = [{'id': item_id, 'category': 1, 'name': name or ''}]
    for cn, cc in G.LOOT_CATEGORIES.items():
        if config.PROFILE != 'err' and cn in ERR_ONLY_CATS:
            continue
        try:
            if cc['filter'](single):
                return cn
        except Exception:
            continue
    return None


def main():
    print('Reading EquipParamGoods ...')
    rows = read_goods_rows()
    print(f'  {len(rows)} goods rows')
    print('Loading GoodsName FMG ...')
    names = load_goods_names()
    print(f'  {len(names)} names')

    # cache the raw (gType,sg) per goods id
    OUT_JSON.parent.mkdir(parents=True, exist_ok=True)
    with open(OUT_JSON, 'w', encoding='utf-8') as f:
        json.dump({str(k): list(v) for k, v in sorted(rows.items())}, f)
    print(f'  wrote {OUT_JSON}')

    # cell (gType,sg) -> Counter(category) ; and per-cell per-category sample ids
    cell_cats = defaultdict(Counter)
    cell_items = defaultdict(lambda: defaultdict(list))  # cell -> cat -> [(id,name)]
    cat_cells = defaultdict(set)                          # category -> set of cells
    none_by_cell = Counter()
    for rid, (gt, sg) in rows.items():
        cat = classify(rid, names.get(rid))
        cell = (gt, sg)
        cell_cats[cell][cat] += 1
        cell_items[cell][cat].append((rid, names.get(rid, '?')))
        if cat is None:
            none_by_cell[cell] += 1
        else:
            cat_cells[cat].add(cell)

    # --- report ---
    L = []
    L.append('# Phase-3 taxonomy probe — (goodsType, sortGroupId) -> Category')
    L.append('')
    L.append('> Generated by `tools/_probe_taxonomy_map.py`. Offline analysis of whether ER\'s own '
             'item taxonomy (`EquipParamGoods.goodsType@+0x3e` + `sortGroupId@+0x72`) can replace the '
             'per-item ITEM_ICONS *category* column. Each row = one distinct `(gType, sg)` cell over '
             'ALL goods rows, with the category the curated `LOOT_CATEGORIES` classifier assigns.')
    L.append('')
    L.append(f'_{len(rows)} goods rows, {len(cell_cats)} distinct (gType,sg) cells, '
             f'{len(cat_cells)} goods categories reachable._')
    L.append('')

    pure = {c: cc for c, cc in cell_cats.items() if len([k for k in cc if k is not None]) <= 1}
    conflict = {c: cc for c, cc in cell_cats.items()
                if len([k for k in cc if k is not None]) >= 2}

    L.append(f'## Summary')
    L.append('')
    L.append(f'- **Pure cells** (<=1 non-None category): **{len(pure)}** — drift-free live-map rows.')
    L.append(f'- **Conflict cells** (>=2 categories): **{len(conflict)}** — need a curated id sub-rule '
             f'(stay in ITEM_ICONS).')
    total_none = sum(none_by_cell.values())
    L.append(f'- **Unclassified goods** (no LOOT_CATEGORIES match = the catch-all tail): **{total_none}**.')
    L.append('')

    # full cell table
    L.append('## All (gType, sg) cells')
    L.append('')
    L.append('| gType | sg | n | category(ies) [count] | status |')
    L.append('|---:|---:|---:|---|---|')
    for cell in sorted(cell_cats, key=lambda c: (c[0], c[1])):
        gt, sg = cell
        cc = cell_cats[cell]
        n = sum(cc.values())
        named = {k: v for k, v in cc.items() if k is not None}
        parts = []
        for k in sorted(named, key=lambda x: -named[x]):
            parts.append(f'{k} [{named[k]}]')
        if cc.get(None):
            parts.append(f'_(unclassified)_ [{cc[None]}]')
        if len(named) >= 2:
            status = '**CONFLICT**'
        elif len(named) == 1 and not cc.get(None):
            status = 'pure'
        elif len(named) == 1:
            status = 'pure+tail'
        else:
            status = 'tail-only'
        L.append(f'| {gt} | {sg} | {n} | {"; ".join(parts)} | {status} |')
    L.append('')

    # conflicts detail
    L.append('## Conflict cells — id-level breakdown')
    L.append('')
    L.append('These `(gType, sg)` cells map to >1 category: sortGroupId alone cannot decide them. '
             'The curated rule (id list / name match) that splits them must stay as an ITEM_ICONS '
             'exception (or become an explicit id sub-rule in the live classifier).')
    L.append('')
    if not conflict:
        L.append('_(none)_')
    for cell in sorted(conflict, key=lambda c: (c[0], c[1])):
        gt, sg = cell
        L.append(f'### gType {gt}, sg {sg}')
        L.append('')
        for cat in sorted(cell_items[cell], key=lambda x: (x is None, x)):
            items = cell_items[cell][cat]
            label = cat if cat is not None else '_(unclassified)_'
            sample = ', '.join(f'{nm}({i})' for i, nm in sorted(items)[:12])
            more = '' if len(items) <= 12 else f' … +{len(items)-12} more'
            L.append(f'- **{label}** ({len(items)}): {sample}{more}')
        L.append('')

    # per-category: is it pure-sortgroup-derivable?
    L.append('## Per-category — cell footprint')
    L.append('')
    L.append('For each goods category, the set of `(gType,sg)` cells its items occupy, and whether any '
             'of those cells is shared with another category (= not purely sortGroup-derivable).')
    L.append('')
    L.append('| category | cells | shares a cell? |')
    L.append('|---|---|---|')
    for cat in sorted(cat_cells):
        cells = sorted(cat_cells[cat])
        shared = any(len([k for k in cell_cats[c] if k is not None]) >= 2 for c in cells)
        cellstr = ', '.join(f'({g},{s})' for g, s in cells)
        L.append(f'| {cat} | {cellstr} | {"YES — curated" if shared else "no — pure"} |')
    L.append('')

    OUT_MD.parent.mkdir(parents=True, exist_ok=True)
    with open(OUT_MD, 'w', encoding='utf-8') as f:
        f.write('\n'.join(L))
    print(f'wrote {OUT_MD}  ({len(cell_cats)} cells, {len(conflict)} conflicts)')


if __name__ == '__main__':
    main()
