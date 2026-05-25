#!/usr/bin/env python3
"""Cross-check the C++ DLL's vtable-based scan against the original
brute-force (entity_id + 2.0f + vftable) scan. They should agree.
If they disagree, one or both discriminators are wrong.
"""
import struct, os, ctypes
from ctypes import wintypes
import pymem

os.environ.setdefault("PYTHONIOENCODING", "utf-8")

KINDLING_NORMAL = {1045373501, 1045373502, 1045373503, 1045373504, 1045373505}
TWO_F = b"\x00\x00\x00\x40"
RADIUS_COROUTINE_VTABLE_RVA = 0x2A5BB90


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
    vtable_addr = base + RADIUS_COROUTINE_VTABLE_RVA
    print(f"vtable: 0x{vtable_addr:X}\n")

    kernel32 = ctypes.windll.kernel32
    VQ = kernel32.VirtualQueryEx
    VQ.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
    VQ.restype = ctypes.c_size_t

    # Two scans: brute (entity_id+2.0f+vftable@-0x10) and vtable
    # (vtable@+0x00 + entity@+0x30 + liveness@+0x20).
    brute_alive = {}  # eid -> addr_of_entity_id
    vtable_alive = {}  # eid -> struct_start_addr
    vtable_dead = {}

    addr = 0x10000
    while addr < 0x7FFFFFFFFFFF:
        mbi = MBI()
        if not VQ(pm.process_handle, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            break
        if (mbi.State == 0x1000
                and (mbi.Protect & 0xFF) in (0x04, 0x08)
                and not (mbi.Protect & 0x100)
                and mbi.Type != 0x1000000
                and 0x1000 <= mbi.RegionSize <= 32 * 1024 * 1024):
            try:
                data = pm.read_bytes(addr, mbi.RegionSize)
                # Brute: search for entity_id + 2.0f
                for eid in KINDLING_NORMAL:
                    if eid in brute_alive: continue
                    needle = struct.pack("<I", eid) + TWO_F
                    idx = data.find(needle)
                    if idx >= 0x10:
                        v = struct.unpack_from("<Q", data, idx - 0x10)[0]
                        if v >= 0x10000:
                            brute_alive[eid] = addr + idx
                # Vtable: search for vtable_addr u64
                vt_needle = struct.pack("<Q", vtable_addr)
                start = 0
                while True:
                    i = data.find(vt_needle, start)
                    if i < 0: break
                    if i % 8 == 0 and i + 0x40 <= len(data):
                        eid = struct.unpack_from("<I", data, i + 0x30)[0]
                        if eid in KINDLING_NORMAL:
                            liveness = struct.unpack_from("<Q", data, i + 0x20)[0]
                            if liveness >= 0x10000:
                                vtable_alive[eid] = addr + i
                            else:
                                vtable_dead[eid] = (addr + i, liveness)
                    start = i + 1
            except: pass
        addr += mbi.RegionSize

    print("=== Brute scan (entity_id + 2.0f + vftable@-0x10 >= 0x10000) ===")
    for eid in sorted(KINDLING_NORMAL):
        if eid in brute_alive:
            print(f"  ALIVE  eid {eid} @ 0x{brute_alive[eid]:X} (entity_id position)")
        else:
            print(f"  -      eid {eid} not found")

    print("\n=== Vtable scan (vtable@+0x00 + entity@+0x30 + liveness@+0x20 >= 0x10000) ===")
    for eid in sorted(KINDLING_NORMAL):
        if eid in vtable_alive:
            print(f"  ALIVE  eid {eid} @ 0x{vtable_alive[eid]:X} (struct start)")
        elif eid in vtable_dead:
            addr, liveness = vtable_dead[eid]
            print(f"  DEAD   eid {eid} @ 0x{addr:X} liveness=0x{liveness:X}")
        else:
            print(f"  -      eid {eid} not found")

    # Compare
    print("\n=== Diff ===")
    only_brute = set(brute_alive) - set(vtable_alive)
    only_vtable = set(vtable_alive) - set(brute_alive)
    if only_brute:
        print(f"  in brute but NOT vtable: {sorted(only_brute)}")
        for eid in only_brute:
            ent_addr = brute_alive[eid]
            # This is entity_id position. Struct starts -0x30 from here.
            struct_start = ent_addr - 0x30
            try:
                struct_data = pm.read_bytes(struct_start, 0x40)
                vt_at_start = struct.unpack_from("<Q", struct_data, 0)[0]
                print(f"    eid {eid}: struct_start=0x{struct_start:X}, vtable@+0x00=0x{vt_at_start:X} (expected 0x{vtable_addr:X})")
                print(f"      first 0x40 bytes:")
                for off in range(0, 0x40, 16):
                    print(f"        +0x{off:02X}: {' '.join(f'{b:02X}' for b in struct_data[off:off+16])}")
            except Exception as e:
                print(f"    eid {eid} read failed: {e}")
    if only_vtable:
        print(f"  in vtable but NOT brute: {sorted(only_vtable)}")


if __name__ == "__main__":
    main()
