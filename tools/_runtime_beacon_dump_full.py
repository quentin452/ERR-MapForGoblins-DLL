"""Hardcoded test: dump 256 bytes around the known beacon array address
0x000001782004A198 (slot start of B1 in slot 2). Shows 16 slots × 16 bytes
to see EXACT memory layout: how many beacon slots, what comes after them,
where stamps start.
"""
import sys, io, ctypes, struct
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
from ctypes import wintypes
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import config
import pymem

PID = config.require_eldenring_pid()
pm = pymem.Pymem()
pm.open_process_from_id(PID)
print(f'Attached to PID {PID}', flush=True)

# Beacon B1 xz-match was at 0x...A19C, slot starts at xz-4 = 0x...A198.
# Let's grab that base address and dump 320 bytes (20 slots) around it.
BASE = 0x000001782004A198

try:
    data = pm.read_bytes(BASE - 32, 32 + 320)
except Exception as e:
    print(f'Read failed: {e}', flush=True)
    # Try re-locating the array — the addr is from a previous scan run, it can shift
    # Re-search for the B1 raw bytes
    B1_RAW = bytes.fromhex('01000000e00287453762934501010000')
    print('Re-searching B1 raw bytes...', flush=True)
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
    addr = 0
    mbi = MBI()
    found_addr = None
    while addr < 0x7FFFFFFFFFFF:
        if VQ(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)) == 0:
            break
        if mbi.State == MEM_COMMIT and mbi.Protect in VALID_PROT and mbi.RegionSize <= 1024 * 1024 * 1024:
            try:
                d = pm.read_bytes(mbi.BaseAddress, mbi.RegionSize)
                j = d.find(B1_RAW)
                if j >= 0:
                    found_addr = mbi.BaseAddress + j
                    break
            except Exception:
                pass
        addr = mbi.BaseAddress + mbi.RegionSize
    if found_addr is None:
        print('Could not relocate B1 in memory', flush=True)
        sys.exit(1)
    BASE = found_addr
    print(f'Re-found at 0x{BASE:016X}', flush=True)
    data = pm.read_bytes(BASE - 32, 32 + 320)

print(f'\nDumping 320 bytes starting at 0x{BASE:016X} (B1 slot start), -32..+320', flush=True)
print('=' * 100, flush=True)

# Decode each 16-byte chunk as a slot (only the 16-aligned ones starting at BASE)
print('\nSlot decode (starting at BASE = B1 slot):', flush=True)
for s in range(20):
    off = 32 + s * 16  # data is read starting at BASE-32
    if off + 16 > len(data):
        break
    chunk = data[off:off + 16]
    idx, x, z, typ, pad = struct.unpack('<iffHH', chunk)
    hex_str = ' '.join(f'{b:02x}' for b in chunk)
    addr = BASE + s * 16
    label = ''
    if idx == -1:
        label = 'EMPTY'
    elif (typ >> 8) & 0xFF == 0x01:
        label = 'BEACON'
    elif (typ >> 8) & 0xFF in (0x06, 0x08, 0x09):
        label = 'STAMP'
    elif typ == 0xFFFF:
        label = 'TERMINATOR'
    else:
        label = f'OTHER (hi=0x{(typ>>8)&0xFF:02x})'
    print(f'  slot{s:2d} @ 0x{addr:016X}  {hex_str}  | idx={idx:11d} x={x:11.2f} z={z:11.2f} type=0x{typ:04X} pad=0x{pad:04X}  {label}', flush=True)

print('\nRaw 16-byte rows around BASE:', flush=True)
for off in range(0, len(data), 16):
    addr = BASE - 32 + off
    chunk = data[off:off + 16]
    hex_str = ' '.join(f'{b:02x}' for b in chunk)
    rel = addr - BASE
    print(f'  {rel:+05d}  0x{addr:016X}  {hex_str}', flush=True)
