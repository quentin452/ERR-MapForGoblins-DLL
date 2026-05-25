"""Read decrypted beacon bytes from .err save (slot 2) and locate them in
running game memory. Reports every hit with full context.

Strategy: pull the BND4 file directly via SoulsFormats, find the EMPTY_BEACON
sentinel + 5 beacon slots before it, then search the EXACT 16-byte slot bytes
of every filled beacon in process memory of the live ELDEN RING.
"""
import sys, io, os, struct, re, ctypes
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
from ctypes import wintypes
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import config

# ── 1. Decrypt + parse save slot ──
os.environ.setdefault("PYTHONNET_RUNTIME", "coreclr")
from pythonnet import load
load("coreclr")
import clr
from System import Array, Object
from System import Type as SysType
from System.Reflection import Assembly, BindingFlags

dll = str(config.SOULSFORMATS_DLL)
asm = Assembly.LoadFrom(dll)
clr.AddReference(dll)
_str_type = SysType.GetType("System.String")
_bnd4_read = asm.GetType("SoulsFormats.BND4").GetMethod(
    "Read",
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None,
)

SAVE = str(config.require_active_save())
print(f'Save: {SAVE}', flush=True)

# Try slots 0-9, find one with non-empty marker area
import struct as st

EMPTY_BEACON = b"\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00"

def parse_slot(slot_data):
    """Return (beacons, stamps) where each is (offset, raw_bytes_16, decoded_dict)."""
    first_empty = slot_data.find(EMPTY_BEACON)
    if first_empty < 0:
        return [], []
    # Walk back over high-byte=0x01 slots to find array start
    beacon_start = first_empty
    while beacon_start >= 16:
        cand = beacon_start - 16
        idx = st.unpack_from('<i', slot_data, cand)[0]
        typ = st.unpack_from('<H', slot_data, cand + 12)[0]
        hi = (typ >> 8) & 0xFF
        if idx == -1 or hi == 0x01:
            beacon_start = cand
        else:
            break
    beacons = []
    off = beacon_start
    for i in range(5):
        if off + 16 > len(slot_data):
            break
        raw = slot_data[off:off + 16]
        idx, x, z, typ, pad = st.unpack_from('<iffHH', raw)
        beacons.append((off, raw, dict(idx=idx, x=x, z=z, type=typ, pad=pad)))
        off += 16
    stamps = []
    while off < len(slot_data) - 16:
        raw = slot_data[off:off + 16]
        idx, x, z, typ, pad = st.unpack_from('<iffHH', raw)
        if typ == 0xFFFF: break
        if idx != -1 and (typ >> 8) & 0xFF in (0x06, 0x08, 0x09):
            stamps.append((off, raw, dict(idx=idx, x=x, z=z, type=typ, pad=pad)))
        off += 16
        if len(stamps) > 100: break
    return beacons, stamps


bnd = _bnd4_read.Invoke(None, Array[Object]([SAVE]))
n_files = bnd.Files.Count
print(f'BND4 files: {n_files}', flush=True)

best_slot = None
best_beacons = []
for slot in range(n_files):
    try:
        slot_data = bytes(bnd.Files[slot].Bytes.ToArray())
    except Exception:
        continue
    beacons, stamps = parse_slot(slot_data)
    filled = [b for b in beacons if b[2]['idx'] != -1]
    if filled:
        print(f'Slot {slot} ({bnd.Files[slot].Name}): size={len(slot_data)}  filled_beacons={len(filled)}  stamps={len(stamps)}', flush=True)
        prev_filled = len([b for b in best_beacons if b[2]['idx'] != -1])
        if len(filled) > prev_filled:
            best_slot = slot
            best_beacons = beacons

if best_slot is None:
    print('No slot has filled beacons.', flush=True)
    sys.exit(0)

print(f'\nBest slot: {best_slot}', flush=True)
print('Beacons:', flush=True)
for off, raw, dec in best_beacons:
    print(f'  @ slot+0x{off:06X}  raw={raw.hex()}  idx={dec["idx"]} x={dec["x"]:.4f} z={dec["z"]:.4f} type=0x{dec["type"]:04X} pad=0x{dec["pad"]:04X}', flush=True)

# ── 2. Search live memory ──
import pymem
PID = config.require_eldenring_pid()
pm = pymem.Pymem()
pm.open_process_from_id(PID)
print(f'\nAttached to PID {PID}', flush=True)

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


# Search for filled beacons. Also search by xz alone (last 12 bytes, skipping idx)
filled = [(i, raw, dec) for i, (off, raw, dec) in enumerate(best_beacons) if dec['idx'] != -1]
print(f'\nSearching live memory for {len(filled)} filled beacons...', flush=True)

hits_full = {i: [] for i, _, _ in filled}        # full 16-byte match
hits_xz_only = {i: [] for i, _, _ in filled}     # x+z (8 bytes) only
total_regions = 0
total_mb = 0
for base, size in regions():
    total_regions += 1
    total_mb += size / (1024 * 1024)
    try:
        data = pm.read_bytes(base, size)
    except Exception:
        continue
    for i, raw, dec in filled:
        # 16-byte exact
        off = 0
        while True:
            j = data.find(raw, off)
            if j < 0: break
            hits_full[i].append((base + j, data[max(0, j - 16):j + 32]))
            off = j + 1
            if len(hits_full[i]) > 50: break
        # 8-byte xz only
        xz = raw[4:12]
        off = 0
        while True:
            j = data.find(xz, off)
            if j < 0: break
            if j % 4 == 0:
                hits_xz_only[i].append((base + j, data[max(0, j - 16):j + 32]))
            off = j + 1
            if len(hits_xz_only[i]) > 100: break

print(f'Scanned {total_regions} regions ({total_mb:.0f} MB)\n', flush=True)
for i, raw, dec in filled:
    print(f'B{i+1} idx={dec["idx"]} x={dec["x"]:.4f} z={dec["z"]:.4f}', flush=True)
    print(f'  full 16-byte hits: {len(hits_full[i])}', flush=True)
    print(f'  xz-only (8B) hits: {len(hits_xz_only[i])}', flush=True)

# Show first 3 xz-only hits for B1 with surrounding context
if filled:
    i0, raw0, dec0 = filled[0]
    print(f'\n=== B1 xz-only hit context (first 5) ===', flush=True)
    for addr, ctx in hits_xz_only[i0][:5]:
        print(f'\n@ 0x{addr:016X}', flush=True)
        # ctx layout: 16 bytes pre-xz, 32 bytes from xz onwards
        # Decode as if a slot starts at offset 12 in ctx (idx at 12, xz at 16, ...)
        if len(ctx) >= 28:
            idx, fx, fz, ftyp, fpad = struct.unpack_from('<iffHH', ctx, 12)
            print(f'  treating xz-4 as slot start: idx={idx} x={fx:.4f} z={fz:.4f} type=0x{ftyp:04X} pad=0x{fpad:04X}', flush=True)
        for k in range(0, len(ctx), 16):
            hex_part = ' '.join(f'{b:02x}' for b in ctx[k:k + 16])
            ascii_part = ''.join(chr(b) if 32 <= b < 127 else '.' for b in ctx[k:k + 16])
            print(f'  {k - 16:+04d}  {hex_part}  {ascii_part}', flush=True)
