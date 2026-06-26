#!/usr/bin/env python3
"""Predict the m60 chest-family recovery EXACTLY like the runtime rule:
callee(args[1]) >= 1e9 in an m60_{gx}_{gz} file, entity=args[2], lot=args[n-2],
keep iff lot encodes (gx,gz) AND entity is a _00 MSB part (Enemy or Asset)."""
import os, struct, tempfile
import extract_all_items as E
import config
from pathlib import Path
from System import Array, Object
from System.IO import File as SysFile

ERR = Path(config.require_err_mod_dir())
MSB_DIR = ERR / 'map' / 'MapStudio'

ent00 = {}  # _00 entity -> (kind, model, tile)
for mp in sorted(MSB_DIR.glob('*.msb.dcx')):
    stem = mp.name.replace('.msb.dcx', '')
    if not stem.endswith('_00'):
        continue
    try:
        msb = E._read_from_bytes(E._msbe_read, E.SoulsFormats.DCX.Decompress(str(mp)), '.msb')
    except Exception:
        continue
    for kind, coll in (('Enemy', msb.Parts.Enemies), ('Asset', msb.Parts.Assets)):
        for p in coll:
            if int(getattr(p, 'GameEditionDisable', 0) or 0) == 1:
                continue
            try:
                eid = int(p.EntityID)
            except Exception:
                continue
            if eid > 0 and eid not in ent00:
                ent00[eid] = (kind, str(p.ModelName), stem)

asm = E.asm
_rd = asm.GetType('SoulsFormats.EMEVD').GetMethod(
    'Read', E.BindingFlags.Public | E.BindingFlags.Static | E.BindingFlags.FlattenHierarchy,
    None, E.Array[E.SysType]([E.SysType.GetType('System.String')]), None)

recovered, no_part = [], []
for p in sorted((ERR / 'event').glob('m60_*.emevd.dcx')):
    parts = p.name.replace('.emevd.dcx', '').split('_')
    if len(parts) < 4:
        continue
    fa, fx, fz = int(parts[0][1:]), int(parts[1]), int(parts[2])
    if fa != 60:
        continue
    try:
        tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_m60.tmp')
        SysFile.WriteAllBytes(tmp, E.SoulsFormats.DCX.Decompress(str(p)).ToArray())
        emevd = _rd.Invoke(None, Array[Object]([tmp])); os.unlink(tmp)
    except Exception:
        continue
    for ev in emevd.Events:
        for ins in ev.Instructions:
            if int(ins.Bank) != 2000:
                continue
            a = bytes(ins.ArgData); n = len(a) // 4
            if n < 4:
                continue
            ints = [struct.unpack_from('<i', a, k*4)[0] for k in range(n)]
            callee = ints[1]
            if callee < 1000000000:
                continue
            entity, lot = ints[2], ints[n-2]
            if entity <= 0 or lot <= 0:
                continue
            if (lot // 1000000 % 100) != fx or (lot // 10000 % 100) != fz:
                continue
            (recovered if entity in ent00 else no_part).append((lot, entity, ent00.get(entity)))

seen = set()
print("RECOVERED (entity is a _00 part → placed + de-baked):")
for lot, ent, info in sorted(recovered):
    if lot in seen: continue
    seen.add(lot)
    print(f"  lot={lot:<11} entity={ent} {info[0]} {info[1]} @ {info[2]}")
print(f"\n{len(seen)} distinct lots recovered; {len(no_part)} candidates whose entity is NOT a _00 part:")
for lot, ent, _ in sorted(set((l,e,None) for l,e,_ in no_part)):
    print(f"  lot={lot:<11} entity={ent} (no _00 part — stays residual)")
