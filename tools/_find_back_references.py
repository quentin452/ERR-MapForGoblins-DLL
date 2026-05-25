#!/usr/bin/env python3
"""For each known radius-coroutine address, scan process memory for
pointers to it. Each hit is a back-reference. The manager that tracks
active radius checks should have multiple such back-pointers grouped
together (one per active condition).
"""
import struct, os, ctypes, sys
from ctypes import wintypes
from collections import defaultdict
import pymem

os.environ.setdefault("PYTHONIOENCODING", "utf-8")

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


def find_coroutines(pm):
    """Find live radius coroutines via brute scan."""
    kernel32 = ctypes.windll.kernel32
    VQ = kernel32.VirtualQueryEx
    VQ.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
    VQ.restype = ctypes.c_size_t

    addrs = {}  # eid -> address of coroutine
    addr = 0x10000
    while addr < 0x7FFFFFFFFFFF:
        mbi = MBI()
        if not VQ(pm.process_handle, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            break
        rsize = mbi.RegionSize
        if (mbi.State == 0x1000
                and (mbi.Protect & 0xFF) in (0x04, 0x08)
                and not (mbi.Protect & 0x100)):
            try:
                data = pm.read_bytes(addr, min(rsize, 64*1024*1024))
                for eid in KINDLING_NORMAL:
                    if eid in addrs: continue
                    needle = struct.pack("<I", eid) + TWO_F
                    idx = data.find(needle)
                    if idx >= 0x10:
                        v = struct.unpack_from("<Q", data, idx - 0x10)[0]
                        if v >= 0x10000:
                            addrs[eid] = addr + idx
            except: pass
        addr += rsize
    return addrs


def find_back_refs_to(pm, target_addr, max_hits=200):
    """Scan all RW memory for u64 == target_addr."""
    kernel32 = ctypes.windll.kernel32
    VQ = kernel32.VirtualQueryEx
    VQ.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
    VQ.restype = ctypes.c_size_t

    needle = struct.pack("<Q", target_addr)
    hits = []
    addr = 0x10000
    while addr < 0x7FFFFFFFFFFF and len(hits) < max_hits:
        mbi = MBI()
        if not VQ(pm.process_handle, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            break
        rsize = mbi.RegionSize
        if (mbi.State == 0x1000
                and (mbi.Protect & 0xFF) in (0x04, 0x08)
                and not (mbi.Protect & 0x100)
                and rsize <= 64*1024*1024):
            try:
                data = pm.read_bytes(addr, rsize)
                start = 0
                while True:
                    i = data.find(needle, start)
                    if i < 0: break
                    if i % 8 == 0:  # only 8-byte aligned hits (real pointers)
                        hits.append(addr + i)
                    start = i + 1
            except: pass
        addr += rsize
    return hits


def main():
    pm = pymem.Pymem("eldenring.exe")
    print(f"Finding live coroutines...")
    coros = find_coroutines(pm)
    if not coros:
        print("No live coroutines found - player might not be in m60_45_37_00")
        return
    for eid, a in sorted(coros.items()):
        print(f"  coroutine[eid {eid}] @ 0x{a:X}")

    # Pick first coroutine, find back-refs to it
    print()
    target_eid = sorted(coros.keys())[0]
    target_addr = coros[target_eid]
    print(f"=== Back-refs to spirit {target_eid} @ 0x{target_addr:X} ===")
    refs = find_back_refs_to(pm, target_addr)
    print(f"Found {len(refs)} back-refs (8-byte aligned):")
    for r in refs[:30]:
        print(f"  0x{r:X}")

    # For each back-ref, check if it's part of a list/array of similar pointers
    # by looking at the surrounding 16 ptrs (128 bytes context)
    print(f"\n=== Cluster analysis: are back-refs near OTHER coroutine pointers? ===")
    other_addrs = set(coros.values()) - {target_addr}
    for r in refs[:10]:
        try:
            ctx = pm.read_bytes(r - 64, 128)
        except: continue
        ptrs = [struct.unpack_from("<Q", ctx, i)[0] for i in range(0, 128, 8)]
        nearby_coros = [p for p in ptrs if p in other_addrs or p == target_addr]
        if len(nearby_coros) >= 2:
            print(f"\n  back-ref 0x{r:X} sits near {len(nearby_coros)} coroutine ptrs:")
            for i, p in enumerate(ptrs):
                offset = (i * 8) - 64
                marker = ""
                if p in other_addrs: marker = f" <-- coroutine of eid {[e for e,a in coros.items() if a==p][0]}"
                elif p == target_addr: marker = f" <-- target eid {target_eid}"
                if marker or (0x10000 < p < 0x7FFFFFFFFFFF):
                    print(f"    +{offset:+4d}: 0x{p:X}{marker}")


if __name__ == "__main__":
    main()
