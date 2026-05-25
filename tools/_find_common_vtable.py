#!/usr/bin/env python3
"""For each alive radius coroutine, dump the surrounding bytes and look
for a COMMON pointer (vtable, type tag) that all share. If found, we
can enumerate all coroutines via back-refs to that pointer instead of
brute-scanning all RW memory.
"""
import struct, os, ctypes
from ctypes import wintypes
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
    kernel32 = ctypes.windll.kernel32
    VQ = kernel32.VirtualQueryEx
    VQ.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
    VQ.restype = ctypes.c_size_t
    addrs = {}
    addr = 0x10000
    while addr < 0x7FFFFFFFFFFF:
        mbi = MBI()
        if not VQ(pm.process_handle, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            break
        if (mbi.State == 0x1000 and (mbi.Protect & 0xFF) in (0x04, 0x08) and not (mbi.Protect & 0x100)):
            try:
                data = pm.read_bytes(addr, min(mbi.RegionSize, 64*1024*1024))
                for eid in KINDLING_NORMAL:
                    if eid in addrs: continue
                    needle = struct.pack("<I", eid) + TWO_F
                    idx = data.find(needle)
                    if idx >= 0x10:
                        v = struct.unpack_from("<Q", data, idx - 0x10)[0]
                        if v >= 0x10000:
                            addrs[eid] = addr + idx
            except: pass
        addr += mbi.RegionSize
    return addrs


def hex_dump(data, base=0, width=16):
    out = []
    for i in range(0, len(data), width):
        chunk = data[i:i+width]
        hexs = " ".join(f"{b:02X}" for b in chunk)
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        out.append(f"  +{base+i:+05d}: {hexs:<{width*3}}  {text}")
    return "\n".join(out)


def main():
    pm = pymem.Pymem("eldenring.exe")
    print(f"base: 0x{pm.base_address:X}\n")
    coros = find_coroutines(pm)
    if not coros:
        print("No live coroutines found")
        return

    print(f"Found {len(coros)} live coroutines\n")
    # Read 0x80 bytes BEFORE each entity_id position
    bufs = {}
    for eid, addr in sorted(coros.items()):
        try:
            data = pm.read_bytes(addr - 0x80, 0x100)  # ±0x80 around entity_id
            bufs[eid] = data
        except Exception as e:
            print(f"  read failed for {eid}: {e}")
            continue

    # For each pre-entity_id offset (-0x80 .. -0x08, 8-byte stride), check
    # if the value is the SAME across all coroutines.
    print("=== Common ptrs in pre-entity_id area ===")
    print("(offset relative to entity_id position; same value across all 3-5 coroutines = candidate vtable)")
    print()
    for off in range(-0x80, 0, 8):
        idx_in_buf = 0x80 + off
        vals = []
        for eid in sorted(bufs):
            v = struct.unpack_from("<Q", bufs[eid], idx_in_buf)[0]
            vals.append(v)
        unique = set(vals)
        if len(unique) == 1 and 0x10000 < vals[0] < 0x7FFFFFFFFFFF:
            print(f"  ★ -0x{abs(off):X}: ALL = 0x{vals[0]:X}  (POTENTIAL COMMON POINTER)")
        elif len(unique) <= 2 and any(0x10000 < v < 0x7FFFFFFFFFFF for v in vals):
            v_str = ", ".join(f"0x{v:X}" for v in vals)
            print(f"  ~ -0x{abs(off):X}: mostly common: {v_str}")
        # else: noisy, skip

    # Also dump full -0x80 hex for one coroutine for visualization
    print("\n=== Full dump of first coroutine context ===")
    eid = sorted(bufs.keys())[0]
    print(f"  eid {eid} @ 0x{coros[eid]:X}")
    print(hex_dump(bufs[eid][:0x80], base=-0x80))


if __name__ == "__main__":
    main()
