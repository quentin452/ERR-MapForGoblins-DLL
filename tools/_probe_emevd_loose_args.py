#!/usr/bin/env python3
"""EMEVD-LOOSE arg-layout dump — for each RECOVER-EMEVD-LOOSE? residual lot, find the
bank-2000 init(s) that carry it and print the FULL annotated ArgData int array, marking
which index holds the lot and which indices are positionable MSB entities.

The recovery question (per [[residual-irreducible-strategy]]): a residual where the oracle
found the lot + a positionable entity in the SAME init blob but NOT at a known template's
fixed (entity,lot) offset. Either (a) the call uses a documented template but with a
different arg layout / a sub-lot, or (b) an undocumented / per-tile template. This dump
shows the exact (template, lotIdx, entityIdx) per lot so we can tell whether the 7 share a
clean structure (→ extend the template list precisely) or are heterogeneous (→ curate).

Usage: py _probe_emevd_loose_args.py   (lots are hardcoded from the latest oracle run)
"""
import os, struct, tempfile
import extract_all_items as E
import config
from pathlib import Path
from collections import defaultdict
from System import Array, Object
from System.IO import File as SysFile

# the 7 RECOVER-EMEVD-LOOSE? lots from the oracle (+ the 1 EMEVD-BOUND-ASSET for context)
TARGETS = {
    42030000:   "Taylew the Golem Smith (Spirit)  T90005555",
    1042330100: "Radagon's Scarseal (Talisman)    T90005880",
    1043520500: "Viridian Amber Medallion+1 (Tal) T90005300",
    1047370100: "Larval Tear m60_47_37            t1047372200",
    1049550700: "Larval Tear m60_49_55            t1049552400",
    12050510:   "Golden Rune[200] m12_05          t90005500/501",
    20010520:   "Somber Smithing Scadushard m20   t90005500/501",
    1034500110: "Blaidd Half-Wolf (Spirit)        T90005750 [ASSET-BOUND]",
}
ENT_MIN = 10000

ERR = Path(config.require_err_mod_dir())
MSB_DIR = ERR / 'map' / 'MapStudio'
print("=== building positionable EntityID map (enemy + asset, _00 + LOD) ===")
ent_pos = {}  # eid -> (tile, kind, model, name)
for msb_path in sorted(MSB_DIR.glob('*.msb.dcx')):
    stem = msb_path.name.replace('.msb.dcx', '')
    if stem.endswith('_99'):
        continue
    try:
        msb = E._read_from_bytes(E._msbe_read, E.SoulsFormats.DCX.Decompress(str(msb_path)), '.msb')
    except Exception:
        continue
    for p in msb.Parts.Enemies:
        if int(getattr(p, 'GameEditionDisable', 0) or 0) == 1:
            continue
        try: eid = int(p.EntityID)
        except Exception: continue
        if eid and eid not in ent_pos:
            ent_pos[eid] = (stem, 'enemy', str(p.ModelName), str(p.Name))
    for p in msb.Parts.Assets:
        try: eid = int(p.EntityID)
        except Exception: eid = 0
        if eid and eid not in ent_pos:
            ent_pos[eid] = (stem, 'asset', str(p.ModelName), str(p.Name))

print("=== scanning EMEVD bank-2000 inits for the target lots ===")
asm = E.asm
_emevd_read = asm.GetType('SoulsFormats.EMEVD').GetMethod(
    'Read', E.BindingFlags.Public | E.BindingFlags.Static | E.BindingFlags.FlattenHierarchy,
    None, E.Array[E.SysType]([E.SysType.GetType('System.String')]), None)

found = defaultdict(list)
for p in sorted((ERR / 'event').glob('*.emevd.dcx')):
    name = p.name.replace('.emevd.dcx', '')
    try:
        tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_la.tmp')
        SysFile.WriteAllBytes(tmp, E.SoulsFormats.DCX.Decompress(str(p)).ToArray())
        emevd = _emevd_read.Invoke(None, Array[Object]([tmp])); os.unlink(tmp)
    except Exception:
        continue
    for event in emevd.Events:
        for instr in event.Instructions:
            if int(instr.Bank) != 2000:
                continue
            ab = bytes(instr.ArgData)
            n = len(ab) // 4
            if n < 2:
                continue
            ints = [struct.unpack_from('<i', ab, k * 4)[0] for k in range(n)]
            hit = [v for v in ints if v in TARGETS]
            if hit:
                for lot in hit:
                    found[lot].append((name, ints))

print()
for lot, desc in TARGETS.items():
    print(f"\n{'='*100}\nLOT {lot}  —  {desc}")
    if lot not in found:
        print("  (no bank-2000 init carries this lot)")
        continue
    for fname, ints in found[lot]:
        tmpl = ints[1]
        # annotate each index
        cells = []
        for i, v in enumerate(ints):
            tag = ''
            if v == lot: tag = '<<LOT'
            elif i == 1: tag = '(tmpl)'
            elif v >= ENT_MIN and v in ent_pos:
                t, kind, model, _ = ent_pos[v]
                tag = f'<{kind}:{model}@{t}'
            byteoff = i * 4
            cells.append(f"[{i:>2}|b{byteoff:>2}]={v}{(' '+tag) if tag else ''}")
        print(f"  {fname}  tmpl={tmpl}  n={len(ints)}")
        for c in cells:
            print(f"        {c}")
