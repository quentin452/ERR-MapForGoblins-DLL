#!/usr/bin/env python3
"""Resolve singletons via AOB scan (CT-style, version-independent) and
dump their structure. Looking specifically for MsbPointMan which should
manage MSB region runtime state.
"""
import struct, os, sys, ctypes
from ctypes import wintypes
import pymem

os.environ.setdefault("PYTHONIOENCODING", "utf-8")

# AOBs from Hexinton CT
SINGLETONS_AOB = {
    "MsbPointMan":  ("48 8B 0D ???????? 41 B0 01 BA 23000000 E8 ???????? 84 C0",  3, 7),
    "EmkSystem":    ("48 8B 05 ???????? 4C 8B 74 24 ?? 48 8B 7C 24 ?? 48 8B 74 24 ?? 48", 3, 7),
    "WorldSfxMan":  ("48 8B 05 ???????? 48 8D 4D 98 48 89 4C 24 60", 3, 7),
    "WorldGeomMan": ("4C 39 3D ?? ?? ?? ?? 0F 84 ?? ?? ?? ?? 4C 89 60 ?? 41 83 CC FF 4C 89 70 ?? 0F 29 ?? ?? 44 0F 29 ?? ?? F3", 3, 7),
    "FieldArea":    ("48 8B 0D ?? ?? ?? ?? 48 ?? ?? ?? 44 0F B6 61 ?? E8 ?? ?? ?? ?? 48 63 87 ?? ?? ?? ?? 48 ?? ?? ?? 48 85 C0", 3, 7),
    "MsbRegionMan_Maybe": (None, None, None),  # placeholder, search manually
}


def aob_to_pattern(aob_str):
    """Parse CT-style AOB. Tokens can be:
      - '??' or '?'   -> 1 byte wildcard
      - 'xx' (lowercase) -> 1 byte wildcard
      - hex pairs '4A' -> 1 byte literal
      - multi-byte glued 'A1B2C3D4' -> N bytes literal (split per 2 chars)
      - multi-byte wildcards '????????' -> N bytes wildcard
    """
    needle = bytearray()
    mask = bytearray()
    for p in aob_str.split():
        if not p:
            continue
        # Wildcard tokens
        if all(c in "?xX" for c in p):
            n_bytes = max(1, len(p) // 2) if len(p) >= 2 else 1
            for _ in range(n_bytes):
                needle.append(0)
                mask.append(0)
            continue
        # Hex literal — split into byte-pairs
        if len(p) % 2 != 0:
            raise ValueError(f"odd hex token: {p!r}")
        for i in range(0, len(p), 2):
            byte_str = p[i : i + 2]
            if byte_str.lower() in ("??", "xx"):
                needle.append(0)
                mask.append(0)
            else:
                needle.append(int(byte_str, 16))
                mask.append(0xFF)
    return bytes(needle), bytes(mask)


def get_module_size(pm, base):
    """Read PE header at module base to get SizeOfImage."""
    # PE header lookup: base+0x3C = e_lfanew (offset to PE header)
    e_lfanew = struct.unpack("<I", pm.read_bytes(base + 0x3C, 4))[0]
    # PE header signature should be at base + e_lfanew
    sig = pm.read_bytes(base + e_lfanew, 4)
    assert sig == b"PE\0\0", f"bad PE signature: {sig}"
    # COFF header is at +4, optional header at +24
    optional_off = base + e_lfanew + 24
    # SizeOfImage is at +56 in the optional header (PE32+)
    size_of_image = struct.unpack("<I", pm.read_bytes(optional_off + 56, 4))[0]
    return size_of_image


def find_aob_in_module(pm, module_name, aob_str):
    """Regex-based AOB scan over the main module."""
    import re
    needle, mask = aob_to_pattern(aob_str)

    base = pm.base_address  # pymem already has eldenring.exe base
    size = get_module_size(pm, base)
    print(f"  module {module_name}: base 0x{base:X}, size 0x{size:X}, AOB len={len(needle)}")

    # Build regex: literal bytes -> escaped, wildcards -> '.'
    pat = bytearray()
    for b, m in zip(needle, mask):
        if m == 0xFF:
            # Escape regex metachars
            if b in (b".+*?|^$()[]{}\\"):
                pat.extend(b"\\")
            pat.append(b)
        else:
            pat.append(ord("."))
    rx = re.compile(bytes(pat), re.DOTALL)

    # Scan in 16 MiB chunks with overlap
    CHUNK = 16 * 1024 * 1024
    OVERLAP = len(needle)
    matches = []
    off = 0
    while off < size:
        n = min(CHUNK, size - off)
        try:
            data = pm.read_bytes(base + off, n)
        except Exception:
            off += CHUNK
            continue
        for m in rx.finditer(data):
            matches.append(base + off + m.start())
            if len(matches) > 16:
                return matches
        off += CHUNK - OVERLAP if (off + CHUNK < size) else CHUNK
    return matches


def resolve_aob(pm, aob_str, offset, additional):
    """Find AOB, then resolve RIP-relative pointer at addr+offset, advance by additional."""
    matches = find_aob_in_module(pm, "eldenring.exe", aob_str)
    if not matches:
        print("    AOB not found")
        return None
    print(f"    AOB matches: {len(matches)} -> {[hex(a) for a in matches[:3]]}")
    addr = matches[0]
    rel_off_addr = addr + offset
    rel_off_bytes = pm.read_bytes(rel_off_addr, 4)
    rel_off = struct.unpack("<i", rel_off_bytes)[0]
    target_rva_holder = addr + additional + rel_off
    # That's where the singleton POINTER lives. Read pointer from there.
    ptr_bytes = pm.read_bytes(target_rva_holder, 8)
    singleton = struct.unpack("<Q", ptr_bytes)[0]
    return target_rva_holder, singleton


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
    print(f"eldenring base: 0x{base:X}\n")

    for name, (aob, offset, additional) in SINGLETONS_AOB.items():
        if aob is None:
            continue
        print(f"=== {name} ===")
        try:
            result = resolve_aob(pm, aob, offset, additional)
        except Exception as e:
            print(f"    ERROR: {e}")
            continue
        if result is None:
            continue
        slot_addr, singleton = result
        rva = slot_addr - base
        print(f"    slot RVA: 0x{rva:X}  singleton ptr: 0x{singleton:X}")
        if 0x10000 < singleton < 0x7FFFFFFFFFFF:
            # Dump first 0x100 bytes
            try:
                data = pm.read_bytes(singleton, 0x100)
                print(hex_dump(data, base=0))
            except Exception as e:
                print(f"    read failed: {e}")
        print()


if __name__ == "__main__":
    main()
