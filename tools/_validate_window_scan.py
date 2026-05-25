#!/usr/bin/env python3
"""Simpler validation: get MSB region table address via the deterministic
chain, then scan a ±N MB window centered on it. Read in chunks to handle
unmapped pages gracefully."""
import struct, os, time, ctypes
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


def virt_query(pm, addr):
    kernel32 = ctypes.windll.kernel32
    VQ = kernel32.VirtualQueryEx
    VQ.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
    VQ.restype = ctypes.c_size_t
    mbi = MBI()
    if not VQ(pm.process_handle, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
        return None
    return mbi


def scan_buf(data, found):
    for eid in KINDLING_NORMAL:
        if eid in found: continue
        n = struct.pack("<I", eid) + TWO_F
        idx = 0
        while True:
            i = data.find(n, idx)
            if i < 0: break
            if i >= 0x10:
                v = struct.unpack_from("<Q", data, i - 0x10)[0]
                if v >= 0x10000:
                    found.add(eid)
                    break
            idx = i + 1


def main():
    pm = pymem.Pymem("eldenring.exe")
    base = pm.base_address

    sfx = read_ptr(pm, base + RVA_WORLD_SFX_MAN)
    p1 = read_ptr(pm, sfx + 0x40)
    p2 = read_ptr(pm, p1 + 0x28)
    p3 = read_ptr(pm, p2 + 0x10)
    msb = read_ptr(pm, p3 + 0xA0)
    print(f"MSB table @ 0x{msb:X}")

    # Try several window sizes
    for half_window_mb in (16, 32, 64, 128, 256):
        half = half_window_mb * 1024 * 1024
        start = (msb - half) & ~0xFFF  # page-align down
        end = msb + half
        print(f"\n=== Window ±{half_window_mb} MB ({start:X}..{end:X}) ===")

        t0 = time.time()
        found = set()
        regions_scanned = 0
        bytes_read = 0
        last_end = start
        addr = start
        while addr < end:
            mbi = virt_query(pm, addr)
            if mbi is None or mbi.BaseAddress is None:
                addr += 0x1000
                continue
            region_end = mbi.BaseAddress + mbi.RegionSize
            # Move to region start if we're inside one we already passed
            if region_end <= addr:
                addr = region_end
                continue
            scan_start = max(addr, mbi.BaseAddress)
            scan_size = min(region_end, end) - scan_start
            # Check if scannable
            scannable = (mbi.State == 0x1000
                         and (mbi.Protect & 0xFF) in (0x04, 0x08)
                         and not (mbi.Protect & 0x100)
                         and mbi.Type != 0x1000000
                         and 0x1000 <= mbi.RegionSize <= 32 * 1024 * 1024)
            if scannable:
                try:
                    data = pm.read_bytes(scan_start, scan_size)
                    bytes_read += scan_size
                    regions_scanned += 1
                    scan_buf(data, found)
                except Exception:
                    pass
            addr = region_end

        dt = time.time() - t0
        print(f"  found {len(found)}/5 in {dt*1000:.0f} ms, {regions_scanned} regions, {bytes_read/1024/1024:.1f} MB read")
        print(f"  alive: {sorted(found)}")
        if len(found) == 5:
            break
        # If 3+ alive (likely max given pickups), this window is enough
        # to catch all alive spirits. Stop.


if __name__ == "__main__":
    main()
