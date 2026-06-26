#!/usr/bin/env python3
"""Are the 2 baked WorldInteractables (Rune Piece + Godrick's Great Rune, both m10_00 Stormveil)
unique markers or duplicates of items already shown elsewhere?

  row 2000000: "Rune Piece"  pos (-90.463,-22.141,24.771)  flag 1042617188
  row 9100000: "Godrick's Great Rune" pos (-235.646,73.756,348.293) flag 10000800

Check: (a) does m10_00 have an AEG099_821/822 reforged-piece gather node (or boss-reward piece)
near the Rune Piece pos → covered by the Reforged disk pass; (b) is the Godrick interactable pos ==
Godrick's boss position (→ same spot as the live Great Rune marker) or distinct; (c) decode the flags.
"""
import extract_all_items as E, config
from pathlib import Path
import math

ERR = Path(config.require_err_mod_dir())
MSB = ERR / 'map' / 'MapStudio' / 'm10_00_00_00.msb.dcx'
PIECE_POS = (-90.463, -22.141, 24.771)
GODRICK_POS = (-235.646, 73.756, 348.293)

def d3(a, b): return math.sqrt(sum((a[i]-b[i])**2 for i in range(3)))

msb = E._read_from_bytes(E._msbe_read, E.SoulsFormats.DCX.Decompress(str(MSB)), '.msb')

print("=== m10_00 Assets AEG099_82x (reforged-piece gather nodes) + nearest to the baked Rune Piece ===")
best = (1e9, None)
aeg_821 = []
for p in msb.Parts.Assets:
    model = str(p.ModelName)
    if model.startswith('AEG099_82'):
        pos = (float(p.Position.X), float(p.Position.Y), float(p.Position.Z))
        dist = d3(pos, PIECE_POS)
        aeg_821.append((model, str(p.Name), pos, dist))
        if dist < best[0]: best = (dist, (model, str(p.Name), pos))
print(f"  {len(aeg_821)} AEG099_82x assets in m10_00")
for m, nm, pos, dist in sorted(aeg_821, key=lambda x: x[3])[:8]:
    print(f"    {m} {nm} @ ({pos[0]:.1f},{pos[1]:.1f},{pos[2]:.1f})  dist-to-baked-piece={dist:.1f}")
print(f"  → nearest reforged gather node is {best[0]:.1f}u from the baked Rune Piece pos")

print("\n=== ALL assets within 8u of the baked Rune Piece pos (what is actually there?) ===")
for p in msb.Parts.Assets:
    pos = (float(p.Position.X), float(p.Position.Y), float(p.Position.Z))
    if d3(pos, PIECE_POS) < 8:
        print(f"    {str(p.ModelName)} {str(p.Name)} @ ({pos[0]:.1f},{pos[1]:.1f},{pos[2]:.1f}) d={d3(pos,PIECE_POS):.1f}")

print("\n=== Godrick boss enemy (c0000-ish / EntityID) + nearest enemy to the Godrick interactable pos ===")
best = (1e9, None)
for p in msb.Parts.Enemies:
    pos = (float(p.Position.X), float(p.Position.Y), float(p.Position.Z))
    dist = d3(pos, GODRICK_POS)
    if dist < best[0]:
        try: eid = int(p.EntityID)
        except Exception: eid = 0
        best = (dist, (str(p.ModelName), str(p.Name), eid, pos))
if best[1]:
    m, nm, eid, pos = best[1]
    print(f"  nearest enemy: {m} {nm} eid={eid} @ ({pos[0]:.1f},{pos[1]:.1f},{pos[2]:.1f})  dist={best[0]:.1f}")
print("\n=== what is at the Godrick interactable pos (<8u)? ===")
for p in msb.Parts.Assets:
    pos = (float(p.Position.X), float(p.Position.Y), float(p.Position.Z))
    if d3(pos, GODRICK_POS) < 8:
        print(f"    ASSET {str(p.ModelName)} {str(p.Name)} @ ({pos[0]:.1f},{pos[1]:.1f},{pos[2]:.1f}) d={d3(pos,GODRICK_POS):.1f}")

print("\n=== decode flags via ItemLotParam getItemFlagId (does a lot grant a Rune Piece at flag 1042617188?) ===")
reg = E.SoulsFormats.SFUtil.DecryptERRegulation(str(ERR / 'regulation.bin'))
pdefs = E.load_paramdefs()
lf = {'getItemFlagId'}
for i in range(1, 9): lf.update([f'lotItemId0{i}', f'lotItemCategory0{i}'])
ilm = E.param_to_dict(E.read_param(reg, 'ItemLotParam_map', pdefs), lf)
ile = E.param_to_dict(E.read_param(reg, 'ItemLotParam_enemy', pdefs), lf)
for tag, flag in (('Rune Piece', 1042617188), ('Godrick GR', 10000800)):
    hits = []
    for tname, tbl in (('map', ilm), ('enemy', ile)):
        for lot, r in tbl.items():
            if r.get('getItemFlagId', 0) == flag:
                items = [(r.get(f'lotItemCategory0{s}', 0), r.get(f'lotItemId0{s}', 0)) for s in range(1, 9) if r.get(f'lotItemId0{s}', 0) > 0]
                hits.append((tname, lot, items))
    print(f"  flag {flag} ({tag}): {hits[:6]}")
