#!/usr/bin/env python3
"""Replicate the RUNTIME emevd direct-award join (build_disk_emevd_markers) OFFLINE to find
why a baked emevd lot stays residual.

Mirrors the runtime EXACTLY:
  • emevd direct award = kEmevdTemplates parse over event/*.emevd.dcx  (entity, lot)
  • entity -> position  = MSB Parts.Enemies, _00 tiles ONLY, keep if (npc||entity), skip GED==1
  • treasure_lots       = MSB Treasure-event itemLotId, _00 tiles  (treasure_dup → NOT covered)
  • sibling walk        = base+1..+50 in ItemLotParam_map/_enemy

Prints, per target lot, the award + the precise reason it is/ isn't covered.

Usage: python _probe_emevd_join.py [lot ...]  (defaults = the 15 residual emevd lots)
"""
import sys, os, struct, tempfile
import extract_all_items as E
import config
from pathlib import Path
from System import Array, Object
from System.IO import File as SysFile

TARGETS = set(int(a) for a in sys.argv[1:]) or {
    40524, 30510, 1036490010, 1036490011, 1036490012, 1036490013, 1036490014,
    2045460500, 2045460501, 2048400020, 2048400021,
    2050460500, 2050460501, 2050460510, 2050460511}

KEMEVD = {  # eventId: (entityOff, lotOff, minLen) — copy of kEmevdTemplates
    90005300:(8,16,20), 90005301:(8,16,20), 90005860:(16,24,28), 90005861:(16,24,28),
    90005880:(16,24,28), 90005750:(8,16,20), 90005753:(8,16,20), 90005774:(8,12,16),
    90005792:(20,24,28), 90005632:(8,16,20), 90005110:(8,20,24), 90005390:(8,28,32),
    90005555:(8,12,16)}

ERR = Path(config.require_err_mod_dir())
MSB_DIR = ERR / 'map' / 'MapStudio'

asm = E.asm
_emevd_read = asm.GetType('SoulsFormats.EMEVD').GetMethod(
    'Read', E.BindingFlags.Public | E.BindingFlags.Static | E.BindingFlags.FlattenHierarchy,
    None, E.Array[E.SysType]([E.SysType.GetType('System.String')]), None)

# 1) emevd direct awards (entity, lot) + the file each came from
awards = []  # (entity, lot, file)
for p in sorted((ERR / 'event').glob('*.emevd.dcx')):
    name = p.name.replace('.emevd.dcx', '')
    try:
        tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_pj.tmp')
        SysFile.WriteAllBytes(tmp, E.SoulsFormats.DCX.Decompress(str(p)).ToArray())
        emevd = _emevd_read.Invoke(None, Array[Object]([tmp])); os.unlink(tmp)
    except Exception:
        continue
    for event in emevd.Events:
        for instr in event.Instructions:
            if int(instr.Bank) != 2000:
                continue
            args = bytes(instr.ArgData)
            if len(args) < 8:
                continue
            eid = struct.unpack_from('<i', args, 4)[0]
            t = KEMEVD.get(eid)
            if not t or len(args) < t[2]:
                continue
            entity = struct.unpack_from('<i', args, t[0])[0]
            lot = struct.unpack_from('<i', args, t[1])[0]
            if lot > 0 and entity > 0:
                awards.append((entity, lot, name))

award_by_lot = {}
for entity, lot, f in awards:
    award_by_lot.setdefault(lot, []).append((entity, f))

# 2) _00 enemy entity -> tile  +  3) _00 treasure lots
ent_pos = {}      # entity -> (tile, model, name)
treasure_lots = set()
for msb_path in sorted(MSB_DIR.glob('*.msb.dcx')):
    stem = msb_path.name.replace('.msb.dcx', '')
    if not stem.endswith('_00'):
        continue
    try:
        msb = E._read_from_bytes(E._msbe_read, E.SoulsFormats.DCX.Decompress(str(msb_path)), '.msb')
    except Exception:
        continue
    for p in msb.Parts.Enemies:
        if int(getattr(p, 'GameEditionDisable', 0) or 0) == 1:
            continue
        try:
            eid = int(p.EntityID)
        except Exception:
            continue
        if eid and eid not in ent_pos:
            ent_pos[eid] = (stem, str(p.ModelName), str(p.Name))
    for ev in msb.Events.Treasures:
        try:
            lid = int(ev.ItemLotID)
        except Exception:
            continue
        if lid > 0:
            treasure_lots.add(lid)

print(f"[{len(awards)} direct awards, {len(ent_pos)} _00 enemy entities, "
      f"{len(treasure_lots)} _00 treasure lots]\n")

# reverse-trace: for each base award, which contiguous sibling lots exist (any table)?
# (we don't have the param here; just report the award + join verdict.)
base_lots = set(award_by_lot.keys())
for lot in sorted(TARGETS):
    direct = award_by_lot.get(lot)
    if direct:
        for entity, f in direct:
            pos = ent_pos.get(entity)
            tdup = lot in treasure_lots
            if pos and not tdup:
                verdict = f"COVERED (entity in _00 {pos[0]} {pos[1]})"
            elif pos and tdup:
                verdict = f"NOT covered: treasure_dup (lot is also a _00 MSB Treasure) — emevd skips, no de-bake"
            elif not pos:
                verdict = "NOT covered: entity-not-an-_00-enemy (LOD/non-_00 scope)"
            print(f"LOT {lot:<11} BASE award entity={entity} file={f:<14} -> {verdict}")
    else:
        # is it a sibling of a target base?
        sib_of = [b for b in base_lots if 0 < lot - b <= 50]
        if sib_of:
            print(f"LOT {lot:<11} SIBLING of base {sib_of} (covered iff that base emits + chain walk reaches it)")
        else:
            print(f"LOT {lot:<11} *** no direct award, no nearby base *** (chest/MSB-asset, not emevd-direct)")
