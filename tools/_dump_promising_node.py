#!/usr/bin/env python3
"""Re-walk the WorldSfxMan*+0x40+0x28+0x10+0xA0 path to the promising
node and dump it in detail. Also try deeper traversal for any radius
coroutine signature."""
import struct, sys
from datetime import datetime
from pathlib import Path
import pymem

RVA_WORLD_SFX_MAN = 0x3D6F5F8
KINDLING = [1045373501, 1045373502, 1045373503, 1045373504, 1045373505]
TWO_F = b"\x00\x00\x00\x40"


def safe_read(pm, addr, size):
    try: return pm.read_bytes(int(addr), int(size))
    except: return None


def is_valid_ptr(p):
    return p is not None and 0x10000 < p < 0x7FFFFFFFFFFF


def read_ptr(pm, addr):
    d = safe_read(pm, addr, 8)
    return struct.unpack("<Q", d)[0] if d else None


def hex_dump(data, base=0, width=16):
    out = []
    for i in range(0, len(data), width):
        chunk = data[i : i + width]
        hexs = " ".join(f"{b:02X}" for b in chunk)
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        out.append(f"  +0x{base + i:04X}: {hexs:<{width * 3}}  {text}")
    return "\n".join(out)


def main():
    pm = pymem.Pymem("eldenring.exe")
    base = pm.base_address
    print(f"base = 0x{base:X}")

    # Walk: WorldSfxMan* + 0x40 + 0x28 + 0x10 + 0xA0
    sfx_man = read_ptr(pm, base + RVA_WORLD_SFX_MAN)
    print(f"WorldSfxMan = 0x{sfx_man:X}")
    if not is_valid_ptr(sfx_man):
        return

    p1 = read_ptr(pm, sfx_man + 0x40);  print(f"  +0x40 → 0x{(p1 or 0):X}")
    if not is_valid_ptr(p1): return

    p2 = read_ptr(pm, p1 + 0x28);       print(f"  +0x28 → 0x{(p2 or 0):X}")
    if not is_valid_ptr(p2): return

    p3 = read_ptr(pm, p2 + 0x10);       print(f"  +0x10 → 0x{(p3 or 0):X}")
    if not is_valid_ptr(p3): return

    p4 = read_ptr(pm, p3 + 0xA0);       print(f"  +0xA0 → 0x{(p4 or 0):X}")
    if not is_valid_ptr(p4): return

    # Read 0x600 bytes (covers all 5 entries at stride 0xD0)
    data = safe_read(pm, p4, 0x600)
    if data is None:
        print("read failed at p4")
        return

    print(f"\nNode @ 0x{p4:X}, dumping 0x600 bytes:")
    print(hex_dump(data, base=0))

    # Locate kindling entity IDs and the radius signature
    print(f"\n=== Entity ID locations ===")
    for eid in KINDLING:
        needle = struct.pack("<I", eid)
        pos = []
        start = 0
        while True:
            i = data.find(needle, start)
            if i < 0: break
            # check if followed by 2.0f
            radius = i + 4 < len(data) and data[i+4:i+8] == TWO_F
            pos.append((i, radius))
            start = i + 1
        if pos:
            for off, is_radius in pos:
                marker = " [RADIUS!]" if is_radius else ""
                print(f"  eid {eid}: at +0x{off:X}{marker}")

    # If stride 0xD0, dump entries
    print(f"\n=== Decoding as 0xD0-stride array ===")
    for slot in range(8):
        entry_off = 0x100 + slot * 0xD0  # rough heuristic, first entry near +0x100
        if entry_off + 0xD0 > len(data): break
        entry = data[entry_off : entry_off + 0xD0]
        # First int32 of entry
        entry_int = struct.unpack_from("<I", entry, 0)[0]
        # Hex
        print(f"\n  entry[{slot}] @ +0x{entry_off:X}:")
        print(hex_dump(entry[:0x60], base=entry_off))


if __name__ == "__main__":
    main()
