#!/usr/bin/env python3
"""Test the vtable-based discovery approach:
1. Take known vtable RVA (resolved via eldenring.exe base)
2. Scan RW memory for u64 == vtable_addr (single 8-byte pattern, fast)
3. For each hit, check entity_id at struct+0x30
4. Time it and compare to brute scan
"""
import struct, os, ctypes, time
from ctypes import wintypes
import pymem

os.environ.setdefault("PYTHONIOENCODING", "utf-8")

# Confirmed in current session
RADIUS_COROUTINE_VTABLE_RVA = 0x2A5BB90  # eldenring.exe RVA

KINDLING_NORMAL = {1045373501, 1045373502, 1045373503, 1045373504, 1045373505}
ENTITY_ID_OFFSET_IN_STRUCT = 0x30


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


def main():
    pm = pymem.Pymem("eldenring.exe")
    base = pm.base_address
    vtable = base + RADIUS_COROUTINE_VTABLE_RVA
    print(f"eldenring base: 0x{base:X}")
    print(f"vtable address: 0x{vtable:X} (RVA 0x{RADIUS_COROUTINE_VTABLE_RVA:X})")

    needle = struct.pack("<Q", vtable)

    kernel32 = ctypes.windll.kernel32
    VQ = kernel32.VirtualQueryEx
    VQ.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
    VQ.restype = ctypes.c_size_t

    t0 = time.time()
    hits = []
    bytes_scanned = 0
    regions_scanned = 0
    addr = 0x10000
    while addr < 0x7FFFFFFFFFFF:
        mbi = MBI()
        if not VQ(pm.process_handle, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            break
        rsize = mbi.RegionSize
        # Filter same as DLL: PRIVATE RW, not IMAGE, reasonable size
        if (mbi.State == 0x1000
                and (mbi.Protect & 0xFF) in (0x04, 0x08)
                and not (mbi.Protect & 0x100)
                and mbi.Type != 0x1000000  # not MEM_IMAGE
                and 0x1000 <= rsize <= 32 * 1024 * 1024):
            try:
                data = pm.read_bytes(addr, rsize)
                bytes_scanned += rsize
                regions_scanned += 1
                # Find all aligned u64 == vtable
                start = 0
                while True:
                    i = data.find(needle, start)
                    if i < 0: break
                    if i % 8 == 0:  # natural alignment
                        struct_start = addr + i
                        # Read entity_id at +0x30
                        eid_off = i + ENTITY_ID_OFFSET_IN_STRUCT
                        if eid_off + 4 <= len(data):
                            eid = struct.unpack_from("<I", data, eid_off)[0]
                            hits.append((struct_start, eid))
                    start = i + 1
            except: pass
        addr += rsize
    dt = time.time() - t0

    print(f"\n=== Scan complete in {dt*1000:.0f} ms ===")
    print(f"  scanned {regions_scanned} regions, {bytes_scanned/1024/1024:.0f} MB")
    print(f"  total vtable hits: {len(hits)}")

    # Filter to kindling
    kindling_hits = [(addr, eid) for addr, eid in hits if eid in KINDLING_NORMAL]
    print(f"  kindling-spirit hits: {len(kindling_hits)}")
    for addr, eid in sorted(kindling_hits, key=lambda x: x[1]):
        print(f"    eid {eid} @ struct 0x{addr:X}")

    # Show distribution of eids found via vtable
    from collections import Counter
    by_eid = Counter(eid for _, eid in hits)
    print(f"\n  total entity_id distribution (top 20):")
    for eid, count in by_eid.most_common(20):
        print(f"    eid {eid}: {count} occurrences")


if __name__ == "__main__":
    main()
