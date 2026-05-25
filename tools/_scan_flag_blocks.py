"""Scan ALL event flag blocks in USER_DATA002, count set flags per block.
Helps find WHICH block holds the AEG099_860 collection flags."""
import sys, struct, os
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import config

flag_map = {}
with open(Path(__file__).parent / '_eventflag_bst.txt') as f:
    for line in f:
        k, v = line.strip().split(',')
        flag_map[int(k)] = int(v)

BLOCK_SIZE = 125

os.environ.setdefault("PYTHONNET_RUNTIME", "coreclr")
from pythonnet import load; load("coreclr")
import clr
from System import Array, Object
from System import Type as SysType
from System.Reflection import Assembly, BindingFlags
dll = str(config.SOULSFORMATS_DLL)
asm = Assembly.LoadFrom(dll); clr.AddReference(dll)
_str = SysType.GetType("System.String")
_bnd4_read = asm.GetType("SoulsFormats.BND4").GetMethod("Read",
    BindingFlags.Public|BindingFlags.Static|BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str]), None)

save_path = str(config.require_active_save())
bnd = _bnd4_read.Invoke(None, Array[Object]([save_path]))

for f in bnd.Files:
    name = str(f.Name) if f.Name else '?'
    if 'USER_DATA002' not in name: continue
    data = bytes(f.Bytes.ToArray())
    # event_flags at 0x3c338 per earlier scan
    E = 0x3c338
    event_flags = data[E : E + 0x1BF99F]

    # For each block in flag_map, count set bits
    block_counts = []
    for block_id, offset_idx in flag_map.items():
        offset = offset_idx * BLOCK_SIZE
        if offset + BLOCK_SIZE > len(event_flags): continue
        block_bytes = event_flags[offset : offset + BLOCK_SIZE]
        set_count = sum(bin(b).count('1') for b in block_bytes)
        if set_count > 0:
            block_counts.append((block_id, set_count))

    block_counts.sort(key=lambda x: -x[1])
    print(f'Blocks with set flags (top 40):')
    for block_id, count in block_counts[:40]:
        # Sample range
        print(f'  block {block_id:>10} (flag range {block_id*1000}..{block_id*1000+999}): {count} set')

    # Focus on blocks around 32001 (AEG099_860 entity IDs) and surrounding
    print(f'\nSet flags in blocks 30000..40000 and 1000000..1050000 detail:')
    for block_id in sorted(flag_map.keys()):
        if (30000 <= block_id <= 40000) or (1000000 <= block_id <= 1050000):
            offset = flag_map[block_id] * BLOCK_SIZE
            if offset + BLOCK_SIZE > len(event_flags): continue
            block_bytes = event_flags[offset : offset + BLOCK_SIZE]
            set_flags = []
            for byte_idx in range(BLOCK_SIZE):
                b = block_bytes[byte_idx]
                for bit_idx in range(8):
                    if (b >> (7 - bit_idx)) & 1:
                        flag_id = block_id * 1000 + byte_idx * 8 + bit_idx
                        set_flags.append(flag_id)
            if set_flags:
                print(f'  block {block_id}: {len(set_flags)} set: {set_flags[:30]}{"..." if len(set_flags) > 30 else ""}')
