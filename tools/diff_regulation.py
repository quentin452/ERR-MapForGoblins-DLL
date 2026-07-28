#!/usr/bin/env python3
"""diff_regulation — what actually CHANGED between two regulation.bin files.

Sibling of `flagdiff.py` (which diffs event flags between two SAVES): this one diffs the
param tables between two regulations, row by row and field by field.

WHY IT EXISTS. A byte comparison of two regulation.bin is worthless — the container is
encrypted + compressed, so identical content can hash differently and a one-row change is
indistinguishable from a rewrite. And for a randomized install the spoiler log is NOT a
substitute: measured 2026-07-28, a roll rewrites ~67% of `ItemLotParam_enemy` rows, but the
log only reports UNIQUE drops (bosses, named NPCs, individually-described enemies) — generic
respawning mob drop tables never appear in it. This tool is the only way to see those.

USAGE
    python tools/diff_regulation.py A.bin B.bin
    python tools/diff_regulation.py A.bin B.bin --param ItemLotParam_enemy
    python tools/diff_regulation.py A.bin B.bin --param ItemLotParam_enemy --details --limit 20
    python tools/diff_regulation.py A.bin B.bin --row 100000                # find a row anywhere

SPOILER HYGIENE (same rule as tools/audit_markers_vs_spoiler.py): the default output is
AGGREGATE — param names and counts, never a row id or a field value. `--details` opts into
per-row output and is the only mode that can spoil a run.
"""
import argparse
import os
import sys
from pathlib import Path

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE))

import extract_all_items as E  # noqa: E402  (.NET / SoulsFormats bootstrap)
from extract_all_items import (SoulsFormats, load_paramdefs,  # noqa: E402
                               _read_from_bytes, _param_read)


def load_params(path, paramdefs, only=None):
    """{param name: {row id: (field name, value) tuple}} for every PARAM in a regulation."""
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(path))
    out = {}
    for f in bnd.Files:
        name = os.path.basename(str(f.Name))
        if not name.lower().endswith('.param'):
            continue
        stem = name[:-len('.param')]
        if only and only != stem:
            continue
        param = _read_from_bytes(_param_read, f.Bytes, '.param')
        pt = str(param.ParamType) if param.ParamType else ''
        if pt in paramdefs:
            param.ApplyParamdef(paramdefs[pt])
        rows = {}
        for row in param.Rows:
            # Field NAMES are kept, not just values: without them a diff can only say "row
            # changed", which is exactly the useless answer this tool exists to avoid.
            try:
                rows[int(row.ID)] = tuple((str(c.Def.InternalName), str(c.Value)) for c in row.Cells)
            except Exception:
                rows[int(row.ID)] = ()
        out[stem] = rows
    return out


def diff_rows(a, b):
    """(added ids, removed ids, {id: [(field, old, new)]}) between two row dicts."""
    added = sorted(b.keys() - a.keys())
    removed = sorted(a.keys() - b.keys())
    changed = {}
    for rid in sorted(a.keys() & b.keys()):
        ra, rb = a[rid], b[rid]
        if ra == rb:
            continue
        if len(ra) != len(rb):          # paramdef mismatch — report wholesale
            changed[rid] = [('<row shape>', f'{len(ra)} fields', f'{len(rb)} fields')]
            continue
        deltas = [(fa, va, vb) for (fa, va), (_, vb) in zip(ra, rb) if va != vb]
        if deltas:
            changed[rid] = deltas
    return added, removed, changed


def main():
    ap = argparse.ArgumentParser(description='Diff two regulation.bin at the param-row level.')
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--param', help='only this param (e.g. ItemLotParam_enemy)')
    ap.add_argument('--row', type=int, help='only this row id, across every param')
    ap.add_argument('--details', action='store_true',
                    help='print per-row field changes (SPOILS a run — off by default)')
    ap.add_argument('--limit', type=int, default=10, help='rows shown per param with --details')
    args = ap.parse_args()

    for p in (args.a, args.b):
        if not Path(p).is_file():
            sys.exit(f'not a file: {p}')

    paramdefs = load_paramdefs()
    A = load_params(args.a, paramdefs, args.param)
    B = load_params(args.b, paramdefs, args.param)

    print(f'A = {args.a}')
    print(f'B = {args.b}')
    only_a = sorted(A.keys() - B.keys())
    only_b = sorted(B.keys() - A.keys())
    if only_a:
        print(f'params only in A: {len(only_a)}  {only_a[:5]}')
    if only_b:
        print(f'params only in B: {len(only_b)}  {only_b[:5]}')

    total_changed = 0
    print(f'\n{"param":34s} {"rows":>7s} {"+":>6s} {"-":>6s} {"changed":>8s}  {"%":>6s}')
    print('-' * 74)
    for name in sorted(A.keys() & B.keys()):
        added, removed, changed = diff_rows(A[name], B[name])
        if args.row is not None:
            changed = {k: v for k, v in changed.items() if k == args.row}
            added = [i for i in added if i == args.row]
            removed = [i for i in removed if i == args.row]
        if not (added or removed or changed):
            continue
        total_changed += 1
        common = len(A[name].keys() & B[name].keys())
        pct = 100.0 * len(changed) / common if common else 0.0
        print(f'{name:34s} {len(A[name]):7d} {len(added):6d} {len(removed):6d} '
              f'{len(changed):8d}  {pct:5.1f}%')
        if args.details:
            for rid in list(changed)[:args.limit]:
                fields = ', '.join(f'{f}: {o} -> {n}' for f, o, n in changed[rid][:6])
                more = '' if len(changed[rid]) <= 6 else f' (+{len(changed[rid]) - 6} more)'
                print(f'    row {rid:>10d}  {fields}{more}')
            if len(changed) > args.limit:
                print(f'    … {len(changed) - args.limit} more changed rows')

    if total_changed == 0:
        print('(no param differs — the two regulations are equivalent)')
    else:
        print(f'\n{total_changed} param(s) differ.')


if __name__ == '__main__':
    main()
