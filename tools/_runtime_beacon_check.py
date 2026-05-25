"""Locate the live beacon array in game memory and report exact layout.

Save file slot 2 currently has 5 beacons (type 0x0101) and 1 stamp.
We have their map coords. Search for the EXACT 16-byte slot bytes
(idx, x, z, type, pad) of beacon 0 in the running game memory and
report all hits + 256-byte context, so we can see what the real
in-memory layout looks like (slot count, type values, alignment).
"""
import sys, io, ctypes, struct, os
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
from ctypes import wintypes
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import config

PID = config.require_eldenring_pid()
print(f'Attached to PID {PID}', flush=True)

# Beacons from save file (extract_markers.py output for slot 2):
BEACONS = [
    (0, 4320.4, 4716.3, 0x0101),  # B1
    (1, 4233.8, 5315.4, 0x0101),  # B2
    (2, 5138.5, 5853.8, 0x0101),  # B3
    (3, 4747.3, 5074.8, 0x0101),  # B4
    (4, 5382.8, 5345.4, 0x0101),  # B5
]

# We don't know the exact idx the game stored, so search by (x, z) float
# pair only (8 bytes); idx is then read from the preceding 4 bytes.
import pymem
pm = pymem.Pymem()
pm.open_process_from_id(PID)

MEM_COMMIT = 0x1000
VALID_PROT = {0x02, 0x04, 0x08, 0x20, 0x40}

class MBI(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_ulonglong), ("AllocationBase", ctypes.c_ulonglong),
        ("AllocationProtect", wintypes.DWORD), ("__a1", wintypes.DWORD),
        ("RegionSize", ctypes.c_ulonglong), ("State", wintypes.DWORD),
        ("Protect", wintypes.DWORD), ("Type", wintypes.DWORD), ("__a2", wintypes.DWORD),
    ]
k32 = ctypes.windll.kernel32
VQ = k32.VirtualQueryEx
VQ.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
VQ.restype = ctypes.c_size_t
h = pm.process_handle


def regions(max_size=1024 * 1024 * 1024):
    addr = 0
    mbi = MBI()
    while addr < 0x7FFFFFFFFFFF:
        if VQ(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)) == 0:
            break
        if mbi.State == MEM_COMMIT and mbi.Protect in VALID_PROT and mbi.RegionSize <= max_size:
            yield mbi.BaseAddress, mbi.RegionSize
        addr = mbi.BaseAddress + mbi.RegionSize


# Search pattern: (x, z) for each beacon = 8 bytes
patterns = []
for bi, x, z, typ in BEACONS:
    pat = struct.pack('<ff', x, z)
    patterns.append((bi, pat, x, z, typ))

print('Searching for beacon (x,z) byte patterns...', flush=True)
hits_per_beacon = {bi: [] for bi, _, _, _, _ in patterns}
total_regions = 0
total_mb = 0
for base, size in regions():
    total_regions += 1
    total_mb += size / (1024 * 1024)
    try:
        data = pm.read_bytes(base, size)
    except Exception:
        continue
    for bi, pat, x, z, typ in patterns:
        off = 0
        while True:
            i = data.find(pat, off)
            if i < 0:
                break
            if i % 4 == 0:  # float-aligned
                hits_per_beacon[bi].append((base + i, data[max(0, i - 16):i + 32]))
            off = i + 1

print(f'Scanned {total_regions} regions ({total_mb:.0f} MB)\n', flush=True)
for bi, _, x, z, typ in patterns:
    hits = hits_per_beacon[bi]
    print(f'B{bi+1} map=({x:.1f}, {z:.1f}) type=0x{typ:04X}: {len(hits)} hits', flush=True)

# Show context for B1 hits — 16 bytes BEFORE (the idx field) + 32 bytes after (xz + type/pad + next slot)
b0_hits = hits_per_beacon[0]
print(f'\n=== B1 context (first {min(len(b0_hits), 5)} hits) ===\n', flush=True)
for addr, ctx in b0_hits[:5]:
    print(f'@ 0x{addr:016X}  (xz starts at offset 16 in ctx)', flush=True)
    # Decode the slot at offset 16-4 = 12 (idx is 4 bytes before x)
    slot_off = 12  # ctx is 16-byte pre + 32-byte post; xz starts at 16
    if len(ctx) >= slot_off + 16:
        idx, fx, fz, ftyp, fpad = struct.unpack_from('<iffHH', ctx, slot_off)
        print(f'  slot:  idx={idx} x={fx:.2f} z={fz:.2f} type=0x{ftyp:04X} pad=0x{fpad:04X}', flush=True)
    # Print hex
    for k in range(0, len(ctx), 16):
        hex_part = ' '.join(f'{b:02x}' for b in ctx[k:k + 16])
        print(f'  {k:+04d}  {hex_part}', flush=True)
    print('', flush=True)

# Now look for the FULL 5-beacon array. If we find B1, B2, B3, B4, B5 within
# 80 bytes of each other, that's the array.
print('\n=== Looking for clustered beacon arrays ===\n', flush=True)
all_hits = []
for bi, _, x, z, typ in patterns:
    for addr, _ in hits_per_beacon[bi]:
        all_hits.append((addr, bi))
all_hits.sort()

# Cluster hits within 256 bytes of each other
clusters = []
current = []
for addr, bi in all_hits:
    if current and addr - current[-1][0] > 256:
        if len(current) >= 2:
            clusters.append(current)
        current = []
    current.append((addr, bi))
if len(current) >= 2:
    clusters.append(current)

print(f'Found {len(clusters)} clusters with >=2 distinct beacon hits within 256 bytes:', flush=True)
for cluster in clusters[:10]:
    distinct = set(bi for _, bi in cluster)
    base_addr = cluster[0][0]
    print(f'\nCluster @ 0x{base_addr:016X}  ({len(cluster)} hits, {len(distinct)} distinct beacons)', flush=True)
    for addr, bi in cluster:
        print(f'  +0x{addr - base_addr:04x}  B{bi+1}  @ 0x{addr:016X}', flush=True)
