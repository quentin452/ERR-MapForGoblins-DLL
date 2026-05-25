#!/usr/bin/env python3
"""Quick check: which kindling-spirit radius coroutines are alive in
ER's RAM right now. Run before and after picking up a spirit to verify
the (entity_id || 2.0f) discriminator is sound.
"""
import struct
import sys
import time
import ctypes
from ctypes import wintypes
import pymem


KINDLING = {
    1: 1045373501,
    2: 1045373502,
    3: 1045373503,
    4: 1045373504,
    5: 1045373505,
}

PATTERN_2F = b"\x00\x00\x00\x40"  # float 2.0f LE


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
    label = sys.argv[1] if len(sys.argv) > 1 else "scan"
    pm = pymem.Pymem("eldenring.exe")

    kernel32 = ctypes.windll.kernel32
    VirtualQueryEx = kernel32.VirtualQueryEx
    VirtualQueryEx.argtypes = [
        wintypes.HANDLE, ctypes.c_void_p,
        ctypes.POINTER(MBI), ctypes.c_size_t,
    ]
    VirtualQueryEx.restype = ctypes.c_size_t

    needles = {slot: struct.pack("<I", eid) + PATTERN_2F for slot, eid in KINDLING.items()}
    raw_hits = {slot: [] for slot in KINDLING}     # all (entity_id+2.0f) matches
    live_hits = {slot: [] for slot in KINDLING}    # filtered: vftable@-0x10 looks valid

    addr = 0x10000
    regions_scanned = 0
    bytes_scanned = 0
    t0 = time.time()
    while addr < 0x7FFFFFFFFFFF:
        mbi = MBI()
        if not VirtualQueryEx(pm.process_handle, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            break
        rsize = mbi.RegionSize
        if (mbi.State == 0x1000
                and (mbi.Protect & 0xFF) in (0x04, 0x08)
                and not (mbi.Protect & 0x100)):
            try:
                data = pm.read_bytes(addr, min(rsize, 64 * 1024 * 1024))
                regions_scanned += 1
                bytes_scanned += len(data)
                for slot, needle in needles.items():
                    start = 0
                    while True:
                        idx = data.find(needle, start)
                        if idx < 0:
                            break
                        hit_addr = addr + idx
                        raw_hits[slot].append(hit_addr)
                        # Liveness: the 8 bytes at hit_addr-0x10 should be a
                        # valid pointer (>= 0x10000). Freed heap chunks have
                        # the vftable overwritten by the allocator's free-list
                        # marker (low values like 0xFF00).
                        if idx >= 0x10:
                            vftable = struct.unpack_from("<Q", data, idx - 0x10)[0]
                            if vftable >= 0x10000:
                                live_hits[slot].append(hit_addr)
                        start = idx + 1
            except Exception:
                pass
        addr += rsize
    t1 = time.time()

    print(f"=== {label} ({t1 - t0:.1f}s, {regions_scanned} regions, {bytes_scanned/1e6:.0f} MB) ===")
    for slot in sorted(KINDLING):
        eid = KINDLING[slot]
        n_raw = len(raw_hits[slot])
        n_live = len(live_hits[slot])
        status = "ALIVE" if n_live > 0 else "DEAD "
        addrs = " ".join(f"0x{a:X}" for a in live_hits[slot][:3])
        zombie = n_raw - n_live
        zombie_marker = f"  (+{zombie} zombie)" if zombie else ""
        print(f"  spirit {slot} (eid {eid}): {status}  {n_live} live{zombie_marker}  {addrs}")
    alive_slots = [s for s in sorted(KINDLING) if live_hits[s]]
    print(f"\nAlive: {alive_slots}  (count {len(alive_slots)}/5)")


if __name__ == "__main__":
    main()
