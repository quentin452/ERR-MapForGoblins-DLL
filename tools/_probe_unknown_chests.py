#!/usr/bin/env python3
"""Trace the placement mechanism of the residual UNKNOWN-source map lots.

For each target lot, checks TWO sources:
  (1) MSB Asset pickUpItemLotParamId — a placed Parts.Asset whose AEG model's
      AssetEnvironmentGeometryParam.pickUpItemLotParamId == lot (chest / ground pickup).
  (2) EMEVD reference — any bank-2000 event init whose ArgData contains the lot (any
      template id), reported with the template + the arg layout.

Tells us whether the 29 unknown lots are chest-assets (extend the collectible/treasure pass)
or scripted (extend kEmevdTemplates).

Usage: python _probe_unknown_chests.py [lot ...]   (defaults = the 29 residual unknown lots)
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

# (1) AssetEnvironmentGeometryParam row -> pickUpItemLotParamId
reg = E.SoulsFormats.SFUtil.DecryptERRegulation(str(ERR / 'regulation.bin'))
pdefs = E.load_paramdefs()
aeg = E.param_to_dict(E.read_param(reg, 'AssetEnvironmentGeometryParam', pdefs), {'pickUpItemLotParamId'})
row_to_lot = {rid: f.get('pickUpItemLotParamId', 0) for rid, f in aeg.items()}
lot_to_rows = {}
for rid, lot in row_to_lot.items():
    if lot and lot in targets:
        lot_to_rows.setdefault(lot, []).append(rid)

# scan placed assets in _00 tiles for those aeg rows
asset_hits = {l: [] for l in targets}
for msb_path in sorted(MSB_DIR.glob('*.msb.dcx')):
    stem = msb_path.name.replace('.msb.dcx', '')
    if not stem.endswith('_00'):
        continue
    try:
        msb = E._read_from_bytes(E._msbe_read, E.SoulsFormats.DCX.Decompress(str(msb_path)), '.msb')
    except Exception:
        continue
    for p in msb.Parts.Assets:
        model = str(p.ModelName)  # AEG099_001
        if not model.startswith('AEG'):
            continue
        try:
            a, b = model[3:].split('_')
            aegRow = int(a) * 1000 + int(b)
        except Exception:
            continue
        lot = row_to_lot.get(aegRow, 0)
        if lot in targets:
            asset_hits[lot].append((stem, model, str(p.Name)))

# (2) EMEVD references (any template)
asm = E.asm
_emevd_read = asm.GetType('SoulsFormats.EMEVD').GetMethod(
    'Read', E.BindingFlags.Public | E.BindingFlags.Static | E.BindingFlags.FlattenHierarchy,
    None, E.Array[E.SysType]([E.SysType.GetType('System.String')]), None)
emevd_hits = {l: [] for l in targets}
for p in sorted((ERR / 'event').glob('*.emevd.dcx')):
    name = p.name.replace('.emevd.dcx', '')
    try:
        tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_puc.tmp')
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
            for k, v in enumerate(ints):
                if v in targets:
                    tmpl = ints[1] if n >= 2 else None
                    emevd_hits[v].append((name, tmpl, k, ints))

KNOWN_TMPL = {90005300,90005301,90005860,90005861,90005880,90005750,90005753,
              90005774,90005792,90005632,90005110,90005390,90005555}
asset_n = emevd_n = both_n = neither_n = 0
for lot in sorted(targets):
    ah, eh = asset_hits[lot], emevd_hits[lot]
    tag = []
    if ah: tag.append("ASSET-pickup")
    if eh:
        tmpls = sorted(set(h[1] for h in eh))
        known = any(t in KNOWN_TMPL for t in tmpls)
        tag.append(f"EMEVD tmpl={tmpls}" + (" [KNOWN]" if known else " [non-template]"))
    if ah and eh: both_n += 1
    elif ah: asset_n += 1
    elif eh: emevd_n += 1
    else: neither_n += 1
    print(f"LOT {lot:<11} {' + '.join(tag) if tag else '*** neither asset nor emevd ***'}")
    for s, m, nm in ah[:3]:
        print(f"      asset: {s:<14} {m:<14} {nm}")
    for nm, tmpl, k, ints in eh[:2]:
        print(f"      emevd: {nm:<14} tmpl={tmpl} lot@idx{k}  args={ints}")

print(f"\n=== {asset_n} asset-only · {emevd_n} emevd-only · {both_n} both · {neither_n} neither "
      f"(of {len(targets)}) ===")
