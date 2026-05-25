#!/usr/bin/env python3
"""
Find MSB Parts with specific EntityIDs referenced in m32_00_00_00 EMEVD.
Target EntityIDs: 32001510, 32001511, 32001512, 32001840, 32002840, 32002841
"""
import sys, io, os, tempfile
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

from pathlib import Path
import config
from pythonnet import load
load('coreclr')
import clr
clr.AddReference(str(config.SOULSFORMATS_DLL))
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
import SoulsFormats

GAME_DIR = config.require_game_dir()
MSB_PATH = GAME_DIR / 'map' / 'MapStudio' / 'm32_00_00_00.msb.dcx'

TARGET_ENTITY_IDS = {32001510, 32001511, 32001512, 32001840, 32002840, 32002841}

if not MSB_PATH.exists():
    print(f"ERROR: File not found at {MSB_PATH}")
    sys.exit(1)

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
_str_type = SysType.GetType('System.String')
_msbe_read = asm.GetType('SoulsFormats.MSBE').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)

def load_msb(path):
    data = SoulsFormats.DCX.Decompress(str(path)).ToArray()
    tmp = os.path.join(tempfile.gettempdir(), '_find_entity_ids_tmp.msb')
    SysFile.WriteAllBytes(tmp, data)
    m = _msbe_read.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    return m

print(f"Reading {MSB_PATH.name}...")
msb = load_msb(MSB_PATH)

results = []
all_found_ids = set()

# Check all Part types
part_types = [
    ('Assets', msb.Parts.Assets),
    ('Enemies', msb.Parts.Enemies),
    ('DummyAssets', msb.Parts.DummyAssets),
    ('DummyEnemies', msb.Parts.DummyEnemies),
    ('MapPieces', msb.Parts.MapPieces),
]

for type_name, part_list in part_types:
    if part_list.Count == 0:
        continue
    
    for part in part_list:
        entity_id = int(getattr(part, 'EntityID', 0))
        all_found_ids.add(entity_id)
        
        if entity_id in TARGET_ENTITY_IDS:
            name = str(part.Name) if part.Name else '?'
            model = str(part.ModelName) if part.ModelName else '?'
            pos = part.Position if hasattr(part, 'Position') else part.Translate
            pos_x = float(pos.X) if pos else 0
            pos_y = float(pos.Y) if pos else 0
            pos_z = float(pos.Z) if pos else 0
            
            results.append({
                'entity_id': entity_id,
                'type': type_name,
                'name': name,
                'model': model,
                'pos': (pos_x, pos_y, pos_z)
            })

# Sort by entity ID
results.sort(key=lambda r: r['entity_id'])

print(f"\nFound {len(results)} matching parts:\n")
print("EntityID   | Type         | Name                      | ModelName            | Position")
print("-" * 110)
for r in results:
    pos_str = f"({r['pos'][0]:.2f}, {r['pos'][1]:.2f}, {r['pos'][2]:.2f})"
    print(f"{r['entity_id']:10d} | {r['type']:12s} | {r['name']:25s} | {r['model']:20s} | {pos_str}")

# Report missing
missing = TARGET_ENTITY_IDS - set(r['entity_id'] for r in results)
if missing:
    print(f"\nMissing EntityIDs in m32_00_00_00.msb.dcx: {sorted(missing)}")
    print(f"(Total EntityIDs in MSB: {len(all_found_ids)})")

# Check for the specific slot patterns mentioned
print("\n\nChecking for AEG099_860/870 slots...")
aeg_matches = []
for r in results:
    if 'AEG099_860' in r['model'] or 'AEG099_870' in r['model']:
        aeg_matches.append(r)
        print(f"  EntityID {r['entity_id']}: {r['model']} (name: {r['name']})")

if not aeg_matches:
    print("  None of the target EntityIDs match AEG099_860 or AEG099_870 models")
