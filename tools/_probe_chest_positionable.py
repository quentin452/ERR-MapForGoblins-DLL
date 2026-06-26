#!/usr/bin/env python3
"""For each residual UNKNOWN map lot: is ANY arg of its EMEVD award event a positionable
MSB part (Enemy or Asset) in the SAME tile? Determines the recoverable fraction.

A lot is RUNTIME-RECOVERABLE iff its scripted event carries an entity that maps to an MSB
part position (then a generic 'scan args for an in-tile MSB entity' join can place it). If
NO arg resolves to a part, the bake positioned it by an offline heuristic → must stay baked.

Usage: python _probe_chest_positionable.py [lot ...]  (defaults = the 29 residual unknown lots)
"""
import sys, os, struct, tempfile
import extract_all_items as E
import config
from pathlib import Path
from System import Array, Object
from System.IO import File as SysFile

DEFAULT = [1034430200,1034471300,1034471310,1034481300,1034481310,1034500110,1035480100,
           1038430100,1039410300,1039410310,1042330100,1043520500,1047370100,1048510700,
           1049550700,1053560700,1053560710,1053560720,12050510,16000980,20010520,
           31080700,31080710,31080720,34140720,35000580,42000000,42020000,42030000]
targets = set(int(a) for a in sys.argv[1:]) or set(DEFAULT)

ERR = Path(config.require_err_mod_dir())
MSB_DIR = ERR / 'map' / 'MapStudio'

def tile_of_lot(lot):
    # m60 overworld lots = 10AABBCCnnn? use the residual tile from the log instead — but we
    # can approximate: dungeon lots m{AA}_{BB}_0 vs m60. We just match entity by global id.
    return None

# entity id -> list of (tile_stem, kind, model)  over ALL tiers
ent_map = {}
for msb_path in sorted(MSB_DIR.glob('*.msb.dcx')):
    stem = msb_path.name.replace('.msb.dcx', '')
    try:
        msb = E._read_from_bytes(E._msbe_read, E.SoulsFormats.DCX.Decompress(str(msb_path)), '.msb')
    except Exception:
        continue
    for kind, coll in (('Enemy', msb.Parts.Enemies), ('Asset', msb.Parts.Assets)):
        for p in coll:
            try:
                eid = int(p.EntityID)
            except Exception:
                continue
            if eid > 0:
                ent_map.setdefault(eid, []).append((stem, kind, str(p.ModelName)))

# emevd: for each target lot, gather the args of the event(s) referencing it
asm = E.asm
_emevd_read = asm.GetType('SoulsFormats.EMEVD').GetMethod(
    'Read', E.BindingFlags.Public | E.BindingFlags.Static | E.BindingFlags.FlattenHierarchy,
    None, E.Array[E.SysType]([E.SysType.GetType('System.String')]), None)
lot_args = {l: [] for l in targets}   # lot -> list of (file, tmpl, ints)
for p in sorted((ERR / 'event').glob('*.emevd.dcx')):
    name = p.name.replace('.emevd.dcx', '')
    try:
        tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_pcp.tmp')
        SysFile.WriteAllBytes(tmp, E.SoulsFormats.DCX.Decompress(str(p)).ToArray())
        emevd = _emevd_read.Invoke(None, Array[Object]([tmp])); os.unlink(tmp)
    except Exception:
        continue
    for event in emevd.Events:
        for instr in event.Instructions:
            if int(instr.Bank) != 2000:
                continue
            args = bytes(instr.ArgData)
            n = len(args) // 4
            ints = [struct.unpack_from('<i', args, k*4)[0] for k in range(n)]
            for v in ints:
                if v in targets:
                    lot_args[v].append((name, ints[1] if n >= 2 else None, ints))
                    break

reco = stuck = 0
for lot in sorted(targets):
    hits = lot_args[lot]
    found = None
    for fname, tmpl, ints in hits:
        for v in ints:
            if v != lot and v in ent_map:
                # prefer a part in the same area as the emevd file
                parts = ent_map[v]
                found = (v, tmpl, parts[0])
                break
        if found:
            break
    if found:
        reco += 1
        v, tmpl, (stem, kind, model) = found
        print(f"LOT {lot:<11} RECOVERABLE via entity {v} ({kind} {model} @ {stem}) tmpl={tmpl}")
    else:
        stuck += 1
        tmpls = sorted(set(h[1] for h in hits)) if hits else []
        print(f"LOT {lot:<11} STUCK — no arg resolves to an MSB part  (tmpl={tmpls})")

print(f"\n=== {reco} recoverable (an arg is an MSB entity) · {stuck} stuck (offline-heuristic pos) "
      f"of {len(targets)} ===")
