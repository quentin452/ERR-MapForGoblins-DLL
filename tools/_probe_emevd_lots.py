#!/usr/bin/env python3
"""Trace EXACTLY how a given ItemLotParam lot is placed by the mod's EMEVD.

For each target lot id, scans every event/*.emevd.dcx instruction's ArgData for the lot
value (int32 at any 4-byte window) and reports the file, event, instruction bank:id, the
template eventId (args[1] when bank 2000), the lot's byte offset, and the full int32 args.
This tells us whether the lot is awarded by a known kEmevdTemplate event, by some OTHER
template the runtime pass misses, or never referenced in EMEVD at all (→ it's a chest/MSB
asset, not EMEVD).

Usage:  python _probe_emevd_lots.py [lot1 lot2 ...]   (defaults = the 15 residual emevd lots)
"""
import sys, os, struct, tempfile
import extract_all_items as E   # SoulsFormats bootstrap
import config
from pathlib import Path

DEFAULT_LOTS = [
    40524, 30510,
    1036490010, 1036490011, 1036490012, 1036490013, 1036490014,
    2045460500, 2045460501, 2048400020, 2048400021,
    2050460500, 2050460501, 2050460510, 2050460511,
    # plus the m31 chest + Bloodfiend's Arm boss reward, for comparison
    31080700, 31080710, 31080720, 508000701,
]

targets = set(int(a) for a in sys.argv[1:]) or set(DEFAULT_LOTS)

asm = E.asm
_emevd_read = asm.GetType('SoulsFormats.EMEVD').GetMethod(
    'Read', E.BindingFlags.Public | E.BindingFlags.Static | E.BindingFlags.FlattenHierarchy,
    None, E.Array[E.SysType]([E.SysType.GetType('System.String')]), None)

emevd_dir = Path(config.require_err_mod_dir()) / 'event'
from System import Array, Object
from System.IO import File as SysFile

hits = {t: [] for t in targets}
nfiles = 0
for p in sorted(emevd_dir.glob('*.emevd.dcx')):
    name = p.name.replace('.emevd.dcx', '')
    try:
        tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_probe_emevd.tmp')
        SysFile.WriteAllBytes(tmp, E.SoulsFormats.DCX.Decompress(str(p)).ToArray())
        emevd = _emevd_read.Invoke(None, Array[Object]([tmp]))
        os.unlink(tmp)
    except Exception as ex:
        continue
    nfiles += 1
    for event in emevd.Events:
        for instr in event.Instructions:
            args = bytes(instr.ArgData)
            n = len(args) // 4
            ints = [struct.unpack_from('<i', args, k * 4)[0] for k in range(n)]
            for k, v in enumerate(ints):
                if v in targets:
                    bank = int(instr.Bank); iid = int(instr.ID)
                    tmpl = ints[1] if (bank == 2000 and n >= 2) else None
                    hits[v].append(dict(file=name, eventId=int(event.ID), bank=bank, iid=iid,
                                        tmpl=tmpl, off=k * 4, idx=k, ints=ints))

print(f"[scanned {nfiles} emevd files for {len(targets)} lots]\n")
KNOWN_TMPL = {90005300,90005301,90005860,90005861,90005880,90005750,90005753,
              90005774,90005792,90005632,90005110,90005390,90005555}
for lot in sorted(targets):
    hs = hits[lot]
    if not hs:
        print(f"LOT {lot}: *** NO EMEVD REFERENCE *** → not script-placed (chest/MSB asset?)")
        continue
    print(f"LOT {lot}: {len(hs)} ref(s)")
    for h in hs:
        tn = ''
        if h['tmpl'] is not None:
            tn = f" tmpl={h['tmpl']}" + (" [KNOWN]" if h['tmpl'] in KNOWN_TMPL else " [** NOT in kEmevdTemplates **]")
        print(f"   {h['file']:<14} ev={h['eventId']:<12} {h['bank']:>4}:{h['iid']:<3}"
              f" lot@idx{h['idx']}(byte{h['off']}){tn}")
        print(f"      args={h['ints']}")
    print()
