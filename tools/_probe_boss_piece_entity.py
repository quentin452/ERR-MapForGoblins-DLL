#!/usr/bin/env python3
"""Test the Reforged-Rune-Piece residual (9) hypothesis: the OFFLINE BAKE reads bossEntity@16
directly (extract_all_items TEMPLATE_EVENTS 90005860:(16,24,28)), but the RUNTIME parses only
(defeatFlag@8, baseLot@24) and joins defeatFlag->boss via setter candidates (msbe_parser.cpp:653).
For the 8 overworld bosses where defeatFlag != EntityID the runtime join fails -> baked residual.

This probe parses every 90005860/61/80 award and, per award, checks both paths:
  • defeatFlag@8  in MSB entities?  (runtime primary path)
  • bossEntity@16 in MSB entities?  (bake path == proposed fix)
A row where @16 is positionable but @8 is NOT == cleanly recoverable by reading @16.

Usage: py _probe_boss_piece_entity.py
"""
import sys, os, struct, tempfile
import extract_all_items as E
import config
from pathlib import Path
from System import Array, Object
from System.IO import File as SysFile

ERR = Path(config.require_err_mod_dir())
MSB_DIR = ERR / 'map' / 'MapStudio'
BOSS_EVENTS = (90005860, 90005861, 90005880)

asm = E.asm
_emevd_read = asm.GetType('SoulsFormats.EMEVD').GetMethod(
    'Read', E.BindingFlags.Public | E.BindingFlags.Static | E.BindingFlags.FlattenHierarchy,
    None, E.Array[E.SysType]([E.SysType.GetType('System.String')]), None)

# 1) parse boss-reward awards: (defeatFlag@8, bossEntity@16, baseLot@24, emevd_file)
awards = []
for p in sorted((ERR / 'event').glob('*.emevd.dcx')):
    name = p.name.replace('.emevd.dcx', '')
    try:
        tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_bp.tmp')
        SysFile.WriteAllBytes(tmp, E.SoulsFormats.DCX.Decompress(str(p)).ToArray())
        emevd = _emevd_read.Invoke(None, Array[Object]([tmp])); os.unlink(tmp)
    except Exception:
        continue
    for event in emevd.Events:
        for instr in event.Instructions:
            if int(instr.Bank) != 2000:
                continue
            args = bytes(instr.ArgData)
            if len(args) < 28:
                continue
            eid = struct.unpack_from('<i', args, 4)[0]
            if eid not in BOSS_EVENTS:
                continue
            flag   = struct.unpack_from('<i', args, 8)[0]
            entity = struct.unpack_from('<i', args, 16)[0]
            lot    = struct.unpack_from('<i', args, 24)[0]
            if lot > 0:
                awards.append((flag, entity, lot, name, eid))

# 2) MSB entity -> (tile, model, name, is00)
ent_pos = {}     # entity -> (tile, model, partname, is_00)
for msb_path in sorted(MSB_DIR.glob('*.msb.dcx')):
    stem = msb_path.name.replace('.msb.dcx', '')
    is00 = stem.endswith('_00')
    try:
        msb = E._read_from_bytes(E._msbe_read, E.SoulsFormats.DCX.Decompress(str(msb_path)), '.msb')
    except Exception:
        continue
    for part in msb.Parts.Enemies:
        if int(getattr(part, 'GameEditionDisable', 0) or 0) == 1:
            continue
        try:
            eid = int(part.EntityID)
        except Exception:
            continue
        if eid and eid not in ent_pos:
            ent_pos[eid] = (stem, str(part.ModelName), str(part.Name), is00)

print(f"[{len(awards)} boss-reward (90005860/61/80) awards, {len(ent_pos)} MSB enemy entities]\n")

# ItemLotParam_map: lot -> {getItemFlagId, goods of slots} to verify the piece chain base..base+8
reg = E.SoulsFormats.SFUtil.DecryptERRegulation(str(Path(config.ERR_MOD_DIR) / 'regulation.bin'))
pdefs = E.load_paramdefs()
lot_fields = {'getItemFlagId'}
for i in range(1, 9):
    lot_fields.update([f'lotItemId0{i}', f'lotItemCategory0{i}'])
ilm = E.param_to_dict(E.read_param(reg, 'ItemLotParam_map', pdefs), lot_fields)


def piece_in_chain(base):
    """Mirror build_disk_emevd_markers: walk base..base+8, return (off, goods) of first rune/ember."""
    for off in range(0, 9):
        row = ilm.get(base + off)
        if not row:
            continue
        for s in range(1, 9):
            iid = row.get(f'lotItemId0{s}', 0)
            cat = row.get(f'lotItemCategory0{s}', 0)
            if cat == 1 and iid in (800010, 850010):  # goods Rune(800010)/Ember(850010) piece
                return (off, iid, row.get('getItemFlagId', 0))
    return None

# 3) classify each award
both = flag_only = entity_only = neither = 0
recoverable = []
for flag, entity, lot, f, eid in sorted(awards):
    fp = ent_pos.get(flag)            # defeatFlag as entity (runtime path)
    ep = ent_pos.get(entity)          # bossEntity@16 (bake path)
    # runtime is satisfied if flag is a _00 entity (its setter fallback also exists but flag-direct is the 83/92 case)
    flag_ok = bool(fp and fp[3])
    ent_ok  = bool(ep)
    if flag_ok and ent_ok:
        both += 1
        if flag != entity:
            print(f"  [both-but-DIFFER] flag={flag} entity@16={entity} "
                  f"flagpos={ent_pos[flag][0]} entpos={ent_pos[entity][0]}")
    elif flag_ok: flag_only += 1
    elif ent_ok:
        entity_only += 1
        recoverable.append((flag, entity, lot, f, eid, ep))
    else: neither += 1

print(f"both flag&entity positionable : {both}")
print(f"flag-only (runtime ok)        : {flag_only}")
print(f"ENTITY-only (runtime MISSES, @16 recovers): {entity_only}  <-- the fix target")
print(f"neither positionable          : {neither}\n")

print("=== ENTITY-only awards (recoverable by reading bossEntity@16) ===")
have_piece = 0
for flag, entity, lot, f, eid, ep in recoverable:
    tile, model, pname, is00 = ep
    pc = piece_in_chain(lot)
    if pc:
        have_piece += 1
        off, goods, gflag = pc
        piece = f"PIECE base+{off}={lot+off} {'Rune' if goods==800010 else 'Ember'} gray-flag={gflag}"
    else:
        piece = "*** NO rune/ember piece in base..base+8 (would emit nothing) ***"
    print(f"  ev{eid} flag={flag:<11} entity@16={entity:<11} baseLot={lot:<9} "
          f"[{f}] entity in {tile} ({'_00' if is00 else 'LOD'}) {model} -> {piece}")
print(f"\n{have_piece}/{len(recoverable)} recoverable awards have a real Rune/Ember piece in the chain "
      f"(== markers the fix will emit)")
