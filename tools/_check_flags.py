"""Check AEG099_860 event flag IDs in USER_DATA002 save slot.
We know from MSB: EntityIDs 32001201, 32001205, 32001206, 32001215, 32001216, 32001219
assigned to AEG099_860 slots 9001, 9002, 9003, 9007, 9005, 9004 respectively.
Check if these IDs (also used as event flag IDs) are SET in save event_flags bitmap.
"""
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

def is_flag_set(event_flags_bytes, flag_id):
    block = flag_id // 1000
    index = flag_id % 1000
    if block not in flag_map: return None
    offset = flag_map[block] * BLOCK_SIZE
    byte_index = index // 8
    bit_index = 7 - (index % 8)
    pos = offset + byte_index
    if pos >= len(event_flags_bytes): return None
    return ((event_flags_bytes[pos] >> bit_index) & 1) == 1

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

    # Find event_flags location
    moeg_off = data.find(b'MOEG')
    print(f'MOEG at 0x{moeg_off:x}')

    # Known reference: first_step_grace (76101) should be SET
    ref_flag_id = 76101
    block = ref_flag_id // 1000
    index = ref_flag_id % 1000
    block_offset = flag_map.get(block)
    byte_in_flags = block_offset * BLOCK_SIZE + index // 8
    bit_to_check = 7 - (index % 8)
    print(f'Reference flag {ref_flag_id}: block={block}, offset_index={block_offset}, '
          f'byte_in_flags={byte_in_flags}, bit_to_check={bit_to_check}')

    # Scan for event_flags start — find offset E where
    # (data[E + byte_in_flags] >> bit_to_check) & 1 == 1 AND density around is high
    # event_flags size is 0x1BF99F
    best_candidates = []
    search_start = max(0, moeg_off - 0x1C0000)
    search_end = min(len(data) - 0x1BF9A0, moeg_off)
    for E in range(search_start, search_end):
        pos = E + byte_in_flags
        if pos + 1 >= len(data): break
        if (data[pos] >> bit_to_check) & 1 != 1: continue
        # Density check: count set bits in 256 bytes around pos
        density = sum(bin(data[pos + k]).count('1') for k in range(-128, 128) if 0 <= pos + k < len(data))
        if density > 80:
            best_candidates.append((E, density))

    if not best_candidates:
        print('No candidate event_flags location found'); continue

    best_candidates.sort(key=lambda x: -x[1])
    print(f'Top candidates: {best_candidates[:3]}')

    # Use best candidate
    E = best_candidates[0][0]
    print(f'Using event_flags_start = 0x{E:x}')

    event_flags = data[E : E + 0x1BF99F]

    # Verify: 76101 (grace) and a few other known flags
    print(f'\nVerification:')
    for test_id in [76101, 0, 12030]:
        r = is_flag_set(event_flags, test_id)
        print(f'  flag {test_id}: {r}')

    # Check our AEG099_860 entity IDs
    target_ids = {
        'AEG099_860_9001': 32001201,
        'AEG099_860_9002': 32001205,
        'AEG099_860_9003': 32001206,
        'AEG099_860_9004': 32001219,
        'AEG099_860_9005': 32001216,
        'AEG099_860_9007': 32001215,
    }
    print(f'\nAEG099_860 EntityID flag checks:')
    for name, fid in target_ids.items():
        r = is_flag_set(event_flags, fid)
        print(f'  {name} (flag {fid}): {r}')

    # Check the specific flags from EMEVD init args
    emevd_flags = [32000201, 32000205, 32000206, 32000215, 32000216, 32000219]
    print(f'\nFlags from EMEVD init args:')
    for fid in emevd_flags:
        r = is_flag_set(event_flags, fid)
        print(f'  {fid}: {r}')
    # Also check: maybe the block for flag in init is different
    # 32000201 → block 32000, index 201
    for block_id in [32000, 32001, 32002]:
        if block_id not in flag_map: continue
        offset = flag_map[block_id] * BLOCK_SIZE
        set_count = sum(bin(b).count('1') for b in event_flags[offset:offset + BLOCK_SIZE])
        print(f'  block {block_id}: {set_count} set')
        if set_count > 0 and set_count < 30:
            for bi in range(BLOCK_SIZE):
                b = event_flags[offset + bi]
                for bit in range(8):
                    if (b >> (7 - bit)) & 1:
                        print(f'    {block_id*1000 + bi*8 + bit}: SET')

    # Scan ALL set flags in m32-related blocks
    print(f'\nScanning set flags across broad ranges:')
    ranges = [
        ('m32 entities 32000000-32010000', 32000000, 32010000),
        ('m32 misc 1032000000-1032010000', 1032000000, 1032010000),
        ('lot/material nodes 9980000-9990000', 9980000, 9990000),
        ('lot 998600-998700', 998600, 998700),
        ('generic low 60000-70000', 60000, 70000),
    ]
    for name, lo, hi in ranges:
        set_flags = [fid for fid in range(lo, hi) if is_flag_set(event_flags, fid)]
        print(f'  {name}: {len(set_flags)} set')
        if 0 < len(set_flags) <= 40:
            print(f'    {set_flags}')
