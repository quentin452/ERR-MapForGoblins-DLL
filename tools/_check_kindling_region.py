#!/usr/bin/env python3
"""For each kindling-spirit hit address, dump the containing region's
MEMORY_BASIC_INFORMATION so we can see why our C++ filter might be
rejecting it.
"""
import struct, ctypes
from ctypes import wintypes
import pymem

KINDLING = [1045373501, 1045373502, 1045373503, 1045373504, 1045373505]
PATTERN_2F = b"\x00\x00\x00\x40"


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


PROT_NAMES = {
    0x01: "NOACCESS", 0x02: "READONLY", 0x04: "READWRITE", 0x08: "WRITECOPY",
    0x10: "EXECUTE", 0x20: "EXECUTE_READ", 0x40: "EXECUTE_READWRITE",
    0x80: "EXECUTE_WRITECOPY",
}
TYPE_NAMES = {0x1000000: "IMAGE", 0x40000: "MAPPED", 0x20000: "PRIVATE"}


def main():
    pm = pymem.Pymem("eldenring.exe")
    kernel32 = ctypes.windll.kernel32
    VirtualQueryEx = kernel32.VirtualQueryEx
    VirtualQueryEx.argtypes = [
        wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t,
    ]
    VirtualQueryEx.restype = ctypes.c_size_t

    # First find all hits
    print("scanning for hits...")
    needles = {eid: struct.pack("<I", eid) + PATTERN_2F for eid in KINDLING}
    found = {}  # eid -> first hit addr
    addr = 0x10000
    while addr < 0x7FFFFFFFFFFF:
        mbi = MBI()
        if not VirtualQueryEx(pm.process_handle, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            break
        rsize = mbi.RegionSize
        if (mbi.State == 0x1000 and (mbi.Protect & 0xFF) in (0x04, 0x08)
                and not (mbi.Protect & 0x100)):
            try:
                data = pm.read_bytes(addr, min(rsize, 64 * 1024 * 1024))
                for eid, needle in needles.items():
                    if eid in found:
                        continue
                    if needle in data:
                        idx = data.find(needle)
                        if idx >= 0x10:
                            vftable = struct.unpack_from("<Q", data, idx - 0x10)[0]
                            if vftable >= 0x10000:
                                found[eid] = (addr + idx, addr, mbi.RegionSize, mbi.Type, mbi.Protect, mbi.AllocationProtect)
            except Exception:
                pass
        addr += rsize

    print(f"\nfound {len(found)} live spirits, region info:\n")
    for eid in KINDLING:
        if eid not in found:
            print(f"  eid {eid}: NOT FOUND")
            continue
        hit_addr, base, size, typ, prot, alloc_prot = found[eid]
        type_name = TYPE_NAMES.get(typ, f"0x{typ:X}")
        prot_name = PROT_NAMES.get(prot & 0xFF, f"0x{prot:X}")
        print(f"  eid {eid}: hit 0x{hit_addr:X}")
        print(f"    region base   = 0x{base:X}")
        print(f"    region size   = 0x{size:X} ({size/1024/1024:.2f} MB)")
        print(f"    region type   = {type_name} (0x{typ:X})")
        print(f"    region protect= {prot_name} (0x{prot:X})")
        print(f"    alloc protect = 0x{alloc_prot:X}")
        # Compute what C++ would do:
        skip_image = typ == 0x1000000
        skip_big = size > 32 * 1024 * 1024
        skip_small = size < 0x1000
        verdict = "SCAN"
        if skip_image: verdict = "SKIP (MEM_IMAGE)"
        elif skip_big: verdict = "SKIP (>32 MB)"
        elif skip_small: verdict = "SKIP (<4 KB)"
        print(f"    C++ verdict   = {verdict}")
        print()


if __name__ == "__main__":
    main()
