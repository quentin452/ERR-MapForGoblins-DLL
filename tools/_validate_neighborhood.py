#!/usr/bin/env python3
"""Validate the 'neighborhood scan' optimization:
1. Walk WorldSfxMan*+0x40+0x28+0x10+0xA0 → MSB region table (deterministic).
2. VirtualQuery the table address → get the alloc_base.
3. Walk forward/backward from there, scanning each PRIVATE RW region for
   kindling radius signature. Count how many regions, and how many bytes
   we have to touch before finding all 5 spirits.

If "few regions, few MB" — we have a fast cold-start path. If "thousands
of regions" — not better than current brute scan.
"""
import struct, os, ctypes, time
from ctypes import wintypes
import pymem

os.environ.setdefault("PYTHONIOENCODING", "utf-8")

RVA_WORLD_SFX_MAN = 0x3D6F5F8

KINDLING_NORMAL = [1045373501, 1045373502, 1045373503, 1045373504, 1045373505]
TWO_F = b"\x00\x00\x00\x40"


class MBI(ctypes.Structure):
    _fields_ = [
        ("BaseAddress",       ctypes.c_void_p),
        ("AllocationBase",    ctypes.c_void_p),
        ("AllocationProtect", wintypes.DWORD),
        ("RegionSize",        ctypes.c_size_t),
        ("State",             wintypes.DWORD),
        ("Protect",           wintypes.DWORD),
        ("Type",              wintypes.DWORD),
    ]


def read_ptr(pm, a):
    try: return struct.unpack("<Q", pm.read_bytes(int(a), 8))[0]
    except: return None


def find_radius(data, eid):
    n = struct.pack("<I", eid) + TWO_F
    return data.count(n)


def virt_query(pm, addr):
    kernel32 = ctypes.windll.kernel32
    VQ = kernel32.VirtualQueryEx
    VQ.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
    VQ.restype = ctypes.c_size_t
    mbi = MBI()
    if not VQ(pm.process_handle, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
        return None
    return mbi


def is_scannable(mbi):
    if mbi.State != 0x1000: return False  # MEM_COMMIT
    p = mbi.Protect & 0xFF
    if p not in (0x04, 0x08): return False  # RW or WC
    if mbi.Protect & 0x100: return False    # GUARD
    if mbi.Type == 0x1000000: return False  # IMAGE
    if mbi.RegionSize > 32 * 1024 * 1024: return False
    if mbi.RegionSize < 0x1000: return False
    return True


def scan_region(pm, base, size):
    found = set()
    try:
        data = pm.read_bytes(base, size)
    except Exception:
        return found
    for eid in KINDLING_NORMAL:
        n = struct.pack("<I", eid) + TWO_F
        idx = 0
        while True:
            i = data.find(n, idx)
            if i < 0: break
            # vftable check
            if i >= 0x10:
                v = struct.unpack_from("<Q", data, i - 0x10)[0]
                if v >= 0x10000:
                    found.add(eid)
                    break
            idx = i + 1
    return found


def main():
    pm = pymem.Pymem("eldenring.exe")
    base = pm.base_address
    print(f"base: 0x{base:X}")

    # Step 1: deterministic chain to MSB region table
    sfx = read_ptr(pm, base + RVA_WORLD_SFX_MAN)
    p1 = read_ptr(pm, sfx + 0x40)
    p2 = read_ptr(pm, p1 + 0x28)
    p3 = read_ptr(pm, p2 + 0x10)
    msb_table = read_ptr(pm, p3 + 0xA0)
    print(f"MSB region table @ 0x{msb_table:X}")

    # Get the table's heap region
    mbi_table = virt_query(pm, msb_table)
    print(f"  table heap: base=0x{mbi_table.AllocationBase:X} region 0x{mbi_table.BaseAddress:X}, size 0x{mbi_table.RegionSize:X}")

    # Step 2: scan outwards from the table region (NOT AllocationBase
    # — the latter is the start of a huge reservation, while the actual
    # region is much closer to where radius coroutines live).
    print("\n=== Scanning neighborhood (region-relative) ===")
    t0 = time.time()
    found = set()
    bytes_scanned = 0
    regions_visited = 0
    regions_scanned = 0
    table_region_base = mbi_table.BaseAddress

    # Walk BACKWARD first
    addr = table_region_base - 1
    walked_bwd_mb = 0
    while addr > 0x10000 and walked_bwd_mb < 256:
        mbi = virt_query(pm, addr)
        if not mbi: break
        regions_visited += 1
        scannable = is_scannable(mbi)
        offset = mbi.BaseAddress - table_region_base
        if scannable:
            new_found = scan_region(pm, mbi.BaseAddress, mbi.RegionSize)
            regions_scanned += 1
            bytes_scanned += mbi.RegionSize
            marker = f"→ found {sorted(new_found)}" if new_found else ""
            if new_found - found:
                found |= new_found
            print(f"  BACK {offset/1024/1024:+8.2f} MB: 0x{mbi.BaseAddress:X} size 0x{mbi.RegionSize:X} prot=0x{mbi.Protect:X} type=0x{mbi.Type:X} {marker}")
        else:
            print(f"  BACK {offset/1024/1024:+8.2f} MB: 0x{mbi.BaseAddress:X} size 0x{mbi.RegionSize:X} prot=0x{mbi.Protect:X} type=0x{mbi.Type:X} state=0x{mbi.State:X} SKIPPED")
        walked_bwd_mb += mbi.RegionSize / 1024 / 1024
        if len(found) == 5:
            break
        addr = mbi.BaseAddress - 1

    # If still incomplete, walk forward
    if len(found) < 5:
        addr = table_region_base + mbi_table.RegionSize
        walked_fwd_mb = 0
        while addr < 0x7FFFFFFFFFFF and walked_fwd_mb < 256:
            mbi = virt_query(pm, addr)
            if not mbi: break
            regions_visited += 1
            if is_scannable(mbi):
                new_found = scan_region(pm, mbi.BaseAddress, mbi.RegionSize)
                offset = mbi.BaseAddress - table_region_base
                if new_found - found:
                    found |= new_found
                    regions_scanned += 1
                    bytes_scanned += mbi.RegionSize
                    print(f"  {offset/1024/1024:+8.2f} MB: region 0x{mbi.BaseAddress:X} size 0x{mbi.RegionSize:X} → found {sorted(new_found)}")
                else:
                    regions_scanned += 1
                    bytes_scanned += mbi.RegionSize
            walked_fwd_mb += mbi.RegionSize / 1024 / 1024
            if len(found) == 5:
                break
            addr = mbi.BaseAddress + mbi.RegionSize

    dt = time.time() - t0
    print(f"\n=== Result ===")
    print(f"  found {len(found)}/5 spirits: {sorted(found)}")
    print(f"  regions visited: {regions_visited}")
    print(f"  regions scanned: {regions_scanned}")
    print(f"  bytes scanned:   {bytes_scanned/1024/1024:.1f} MB")
    print(f"  total time:      {dt*1000:.0f} ms")


if __name__ == "__main__":
    main()
