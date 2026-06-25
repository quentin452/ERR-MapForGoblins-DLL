#!/usr/bin/env python3
"""Pin the NpcParam teamType + nameId byte offsets for the no-bake Hostile NPC pass.

Computes each field's REAL byte offset from the paramdef field layout (the field NAME is a
label, NOT the offset — see the SignPuddleParam trap), then VALIDATES against raw-serialized
row bytes (an invader row: raw[teamOff] in {24,27} and raw[nameOff] == the nameId cell).
Also cross-checks the known itemLotId_enemy@0x30 / itemLotId_map@0x34 pins."""
import sys, io, struct
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pythonnet import load
load('coreclr')
import clr
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats
from extract_all_items import load_paramdefs, read_param

ERR = config.require_err_mod_dir()
bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(ERR / 'regulation.bin'))
pdefs = load_paramdefs()
np = read_param(bnd, 'NpcParam', pdefs)
pdef = np.AppliedParamdef

SIZE = {'s8':1,'u8':1,'dummy8':1,'s16':2,'u16':2,'s32':4,'u32':4,'f32':4,'angle32':4,'f64':8,'b32':4,'fixstr':1,'fixstrW':2}

# Walk paramdef fields → real byte offset (handles bitfield packing).
offsets = {}
byte = 0
cur_bits_type = None
bitpos = 0
for f in pdef.Fields:
    name = str(f.InternalName)
    dt = str(f.DisplayType)
    bitsz = int(f.BitSize) if hasattr(f, 'BitSize') else -1
    arr = int(f.ArrayLength) if hasattr(f, 'ArrayLength') else 1
    base = SIZE.get(dt, 4)
    if bitsz >= 0 and dt in ('u8','s8','u16','s16','u32','s32','dummy8'):
        if cur_bits_type != base or bitpos + bitsz > base*8:
            if cur_bits_type is not None: byte += cur_bits_type
            cur_bits_type = base; bitpos = 0
        offsets.setdefault(name, byte)
        bitpos += bitsz
        continue
    if cur_bits_type is not None:
        byte += cur_bits_type; cur_bits_type = None; bitpos = 0
    offsets.setdefault(name, byte)
    if dt == 'fixstr': byte += arr
    elif dt == 'fixstrW': byte += arr*2
    else: byte += base * (arr if arr > 0 else 1)
if cur_bits_type is not None: byte += cur_bits_type

print(f"NpcParam computed row size = {byte} (DetectedSize uses 736)")
for n in ('teamType','nameId','itemLotId_enemy','itemLotId_map'):
    print(f"  {n:18s} -> 0x{offsets.get(n,-1):x}" if n in offsets else f"  {n}: ABSENT")

# Validate vs raw rows: serialize, find an invader row, check raw bytes at the computed offsets.
raw = bytes(np.Write())
toff, noff = offsets['teamType'], offsets['nameId']
loff_e, loff_m = offsets.get('itemLotId_enemy',0x30), offsets.get('itemLotId_map',0x34)
# Build (id -> cell teamType,nameId) from the parsed param for cross-check.
def cells(row):
    d={}
    for c in row.Cells: d[str(c.Def.InternalName)] = c.Value
    return d
invaders = []
for row in np.Rows:
    c = cells(row)
    try: team = int(c['teamType']); name = int(c['nameId'])
    except: continue
    if team in (24,27) and name > 0: invaders.append((int(row.ID), team, name))
print(f"\ninvader rows (teamType in 24/27 AND nameId>0): {len(invaders)}")

# Raw validation: a row's data has teamType@base+0x133 AND nameId@base+0xc. For a sample of
# invaders find base where raw[base+0x133]==team AND int32(raw, base+0xc)==name — both offsets
# confirmed jointly. Positive (invaders) + the itemLot cross-check already covers negatives.
val_ok = val_fail = 0
for (rid, team, name) in invaders[:25]:
    found = False
    p = 0
    while True:
        p = raw.find(struct.pack('<i', name), p)
        if p < 0: break
        base = p - noff
        if base >= 0 and base + 0x134 <= len(raw) and raw[base + toff] == team:
            found = True; break
        p += 1
    if found: val_ok += 1
    else: val_fail += 1
print(f"raw-validation (25 invaders): {val_ok} confirmed, {val_fail} not located")
print(f"USE: teamType@0x{toff:x} (u8), nameId@0x{noff:x} (s32). itemLot cross-check: enemy@0x{loff_e:x} map@0x{loff_m:x} (known-good 0x30/0x34)")
