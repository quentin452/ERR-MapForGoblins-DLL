#!/usr/bin/env python3
"""Dump the struct at WorldSfxMan*+0x40+0x28+0x10 (parent of the MSB region
table). Look for fields that might list ACTIVE SFX runtime instances —
ideally something that distinguishes picked-up vs alive."""
import struct, os, sys
import pymem

os.environ.setdefault("PYTHONIOENCODING", "utf-8")

RVA_WORLD_SFX_MAN = 0x3D6F5F8


def safe_read(pm, a, n):
    try: return pm.read_bytes(int(a), int(n))
    except: return None


def read_ptr(pm, a):
    d = safe_read(pm, a, 8); return struct.unpack("<Q", d)[0] if d else None


def is_valid_ptr(p):
    return p is not None and 0x10000 < p < 0x7FFFFFFFFFFF


def hex_dump(data, base=0, width=16):
    out = []
    for i in range(0, len(data), width):
        chunk = data[i:i+width]
        hexs = " ".join(f"{b:02X}" for b in chunk)
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        out.append(f"  +0x{base+i:04X}: {hexs:<{width*3}}  {text}")
    return "\n".join(out)


def main():
    pm = pymem.Pymem("eldenring.exe")
    base = pm.base_address
    sfx = read_ptr(pm, base + RVA_WORLD_SFX_MAN)
    p1 = read_ptr(pm, sfx + 0x40)
    p2 = read_ptr(pm, p1 + 0x28)
    p3 = read_ptr(pm, p2 + 0x10)
    print(f"WorldSfxMan = 0x{sfx:X}")
    print(f"+0x40       = 0x{p1:X}")
    print(f"+0x28       = 0x{p2:X}")
    print(f"+0x10       = 0x{p3:X}  <-- parent of MSB region table")

    # Dump 0x200 bytes of p3
    data = safe_read(pm, p3, 0x200)
    print(f"\nNode @ 0x{p3:X}, dumping 0x200 bytes:")
    print(hex_dump(data, base=0))

    print("\n=== Plausible pointers at each 8-byte offset ===")
    for off in range(0, len(data), 8):
        v = struct.unpack_from("<Q", data, off)[0]
        if not is_valid_ptr(v):
            continue
        # try to read 16 bytes from there to see what's there
        d2 = safe_read(pm, v, 16)
        if d2 is None: continue
        ascii_repr = "".join(chr(b) if 32 <= b < 127 else "." for b in d2)
        # also report as ints
        ints = struct.unpack("<II", d2[:8])
        print(f"  +0x{off:03X} = 0x{v:X} -> first 16 bytes: {' '.join(f'{b:02X}' for b in d2)}  '{ascii_repr}'")

    # Check if there's a vector/list near our node — look for
    # (begin_ptr, end_ptr, capacity_ptr) STL-vector layout (3 consecutive ptrs)
    print("\n=== STL-vector candidates (3 consecutive pointers, end-begin > 0) ===")
    for off in range(0, len(data) - 24, 8):
        a = struct.unpack_from("<Q", data, off)[0]
        b = struct.unpack_from("<Q", data, off + 8)[0]
        c = struct.unpack_from("<Q", data, off + 16)[0]
        if not (is_valid_ptr(a) and is_valid_ptr(b) and is_valid_ptr(c)):
            continue
        if not (a <= b <= c):
            continue
        size_bytes = b - a
        cap_bytes = c - a
        if size_bytes == 0 or cap_bytes == 0:
            continue
        if cap_bytes > 0x100000:  # > 1 MB — probably not a vector
            continue
        # Try to read the first element
        elem = safe_read(pm, a, 16)
        if elem is None:
            continue
        print(f"  +0x{off:03X}: begin=0x{a:X} end=0x{b:X} cap=0x{c:X}")
        print(f"    size={size_bytes} bytes, cap={cap_bytes}")
        # If stride looks reasonable, infer count
        for stride in (8, 16, 24, 32, 48, 64, 80, 96, 128, 160, 192, 208, 256):
            if size_bytes % stride == 0:
                count = size_bytes // stride
                if 1 <= count <= 1000:
                    print(f"    if stride={stride}: {count} elements")


if __name__ == "__main__":
    main()
