#!/usr/bin/env python3
"""Dump 0x80 bytes around each known kindling radius-coroutine address
so we can diff before/after a pickup and find the actual liveness bit
(if the struct gets freed) or status word (if it's recycled).
"""
import struct
import sys
from pathlib import Path
import pymem

KNOWN_ADDRS = {
    1: 0x29672A90870,
    2: 0x29672A90B30,
    3: 0x29672A90F70,
    4: 0x29672A905B0,
    5: 0x29672A90BB0,
}


def hex_dump(data, base_offset, width=16):
    lines = []
    for i in range(0, len(data), width):
        chunk = data[i : i + width]
        hexs = " ".join(f"{b:02X}" for b in chunk)
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"  +{base_offset + i:+05X}: {hexs:<{width * 3}}  {text}")
    return "\n".join(lines)


def main():
    label = sys.argv[1] if len(sys.argv) > 1 else "dump"
    pm = pymem.Pymem("eldenring.exe")

    out_dir = Path(__file__).parent.parent / "data" / "sfx_dumps"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / f"kindling_addrs_{label}.txt"
    bin_path = out_dir / f"kindling_addrs_{label}.bin"

    txt = []
    raw = bytearray()
    txt.append(f"=== kindling addrs dump: {label} ===")
    for slot in sorted(KNOWN_ADDRS):
        addr = KNOWN_ADDRS[slot]
        try:
            # Read 0x40 BEFORE entity_id and 0x60 AFTER → 0xA0 total
            data = pm.read_bytes(addr - 0x40, 0xA0)
        except Exception as e:
            txt.append(f"\nslot {slot} 0x{addr:X}: READ FAILED ({e})")
            continue
        eid_at_0 = struct.unpack_from("<I", data, 0x40)[0]
        f_at_4 = struct.unpack_from("<f", data, 0x44)[0]
        # Status bytes at offsets that looked interesting in baseline
        status_8 = struct.unpack_from("<Q", data, 0x40 - 0x08)[0]  # +0x18..+0x1F of struct
        vftable = struct.unpack_from("<Q", data, 0x40 - 0x10)[0]
        txt.append(f"\nslot {slot} (expected eid 0x{(1045373500 + slot):08X}={1045373500 + slot}) addr 0x{addr:X}:")
        txt.append(f"  vftable@-0x10 = 0x{vftable:016X}")
        txt.append(f"  status@-0x08  = 0x{status_8:016X}")
        txt.append(f"  bytes@+0x00   = eid 0x{eid_at_0:08X}={eid_at_0}, +0x04 = float {f_at_4}")
        txt.append(hex_dump(data, base_offset=-0x40))
        raw += data
        raw += b"\xFF" * 16

    out_path.write_text("\n".join(txt) + "\n", encoding="utf-8")
    bin_path.write_bytes(bytes(raw))
    print("\n".join(txt))
    print(f"\nWritten: {out_path}")


if __name__ == "__main__":
    main()
