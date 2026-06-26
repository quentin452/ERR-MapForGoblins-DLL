#!/usr/bin/env python3
"""Snow Town seal-release statues (AEG110_029, event 1048572370) — confirm the LOD-asset
recovery: (1) parse event 1048572370 -> (entity->flag), (2) find the AEG110_029 assets in
non-_00 tiles, (3) print their part name (cross-tile prefix = the marker's true fine tile),
raw block-local pos, entityId, and whether each entity got a flag. This is the ground truth
for the runtime non-_00 asset scan (mirror of load_lod_award_entities, asset side)."""
import os, struct, tempfile
import extract_all_items as E, config
from pathlib import Path
from System import Array, Object
from System.IO import File as SysFile

ERR = Path(config.require_err_mod_dir()); MSB = ERR/'map'/'MapStudio'
EVENT_ID = 1048572370
ENT_OFF, FLAG_OFF = 8, 16  # entity@idx0 (+8), flag@idx2 (+16) — from _EXTRA_PUZZLES (0,2)
MODEL = 'AEG110_029'

asm = E.asm
_rd = asm.GetType('SoulsFormats.EMEVD').GetMethod(
    'Read', E.BindingFlags.Public|E.BindingFlags.Static|E.BindingFlags.FlattenHierarchy,
    None, E.Array[E.SysType]([E.SysType.GetType('System.String')]), None)

flags = {}
for p in sorted((ERR/'event').glob('*.emevd.dcx')):
    try:
        tmp = os.path.join(tempfile.gettempdir(), str(os.getpid())+'_ss.tmp')
        SysFile.WriteAllBytes(tmp, E.SoulsFormats.DCX.Decompress(str(p)).ToArray())
        em = _rd.Invoke(None, Array[Object]([tmp])); os.unlink(tmp)
    except Exception:
        continue
    for ev in em.Events:
        for ins in ev.Instructions:
            if int(ins.Bank) != 2000: continue
            a = bytes(ins.ArgData) if ins.ArgData else b''
            if len(a) < max(ENT_OFF, FLAG_OFF)+4: continue
            if struct.unpack_from('<I', a, 4)[0] != EVENT_ID: continue
            ent = struct.unpack_from('<I', a, ENT_OFF)[0]
            fl  = struct.unpack_from('<I', a, FLAG_OFF)[0]
            print(f"  init event={p.name} entity={ent} flag={fl} (arglen={len(a)})")
            if ent and fl: flags[ent] = fl
print(f"{len(flags)} (entity->flag) from event {EVENT_ID}: {flags}")

print(f"\n{MODEL} assets across ALL tiles (tier + part name prefix + raw pos):")
found = 0; flagged = 0
for mp in sorted(MSB.glob('*.msb.dcx')):
    stem = mp.name.replace('.msb.dcx','')
    try: msb = E._read_from_bytes(E._msbe_read, E.SoulsFormats.DCX.Decompress(str(mp)), '.msb')
    except Exception: continue
    for a in msb.Parts.Assets:
        if str(a.ModelName) != MODEL: continue
        try: eid = int(a.EntityID)
        except Exception: eid = 0
        nm = str(a.Name)
        pos = (float(a.Position.X), float(a.Position.Y), float(a.Position.Z))
        prefix = nm.split('-')[0] if '-' in nm else '(none)'
        has = 'FLAG '+str(flags[eid]) if eid in flags else 'no-flag'
        print(f"  file={stem:18s} name={nm:32s} prefix={prefix:14s} eid={eid} pos=({pos[0]:.1f},{pos[1]:.1f},{pos[2]:.1f}) {has}")
        found += 1
        if eid in flags: flagged += 1
print(f"\ntotal {MODEL} placements = {found}; with a {EVENT_ID} flag = {flagged} "
      f"(these recover; the marker tile = the part-name prefix, pos = the raw block-local pos)")
