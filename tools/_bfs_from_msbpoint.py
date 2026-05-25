#!/usr/bin/env python3
"""BFS from MsbPointMan / EmkSystem looking for radius-coroutine
signature (entity_id + 2.0f). If we find a deterministic chain, the C++
DLL can replace its brute scan."""
import struct, os, sys
from collections import deque
from datetime import datetime
import pymem

os.environ.setdefault("PYTHONIOENCODING", "utf-8")

# Singleton pointer-slot RVAs (resolved earlier via AOB)
SLOT_RVAS = {
    "MsbPointMan":  0x3D7D478,
    "EmkSystem":    0x3D67BD0,
    "WorldSfxMan":  0x3D6F5F8,
    "FieldArea":    0x3D691D8,
}

KINDLING_NORMAL = [1045373501, 1045373502, 1045373503, 1045373504, 1045373505]
TWO_F = b"\x00\x00\x00\x40"


def read_ptr(pm, a):
    try: return struct.unpack("<Q", pm.read_bytes(int(a), 8))[0]
    except: return None


def safe_read(pm, a, n):
    try: return pm.read_bytes(int(a), int(n))
    except: return None


def is_valid_ptr(p):
    return p is not None and 0x10000 < p < 0x7FFFFFFFFFFF


def find_radius_offsets(data, eid):
    needle = struct.pack("<I", eid) + TWO_F
    offsets, start = [], 0
    while True:
        i = data.find(needle, start)
        if i < 0: break
        offsets.append(i)
        start = i + 1
    return offsets


def trace(pm, root, root_label, max_depth=10, max_nodes=100000, bytes_per_node=0x800):
    visited = {root: (0, root_label)}
    queue = deque([(root, 0, root_label)])
    matches = []
    nodes = 0
    while queue and nodes < max_nodes:
        addr, d, path = queue.popleft()
        if d > max_depth: continue
        data = safe_read(pm, addr, bytes_per_node)
        if data is None: continue
        nodes += 1

        radius_hits = {}
        for eid in KINDLING_NORMAL:
            offs = find_radius_offsets(data, eid)
            if offs:
                radius_hits[eid] = offs
        if radius_hits:
            matches.append((path, addr, d, radius_hits))

        for off in range(0, len(data) - 7, 8):
            child = struct.unpack_from("<Q", data, off)[0]
            if not is_valid_ptr(child): continue
            new_d = d + 1
            if new_d > max_depth: continue
            existing = visited.get(child)
            if existing is not None and existing[0] <= new_d: continue
            new_path = f"{path}+0x{off:X}"
            visited[child] = (new_d, new_path)
            queue.append((child, new_d, new_path))

    return matches, nodes


def main():
    pm = pymem.Pymem("eldenring.exe")
    base = pm.base_address
    print(f"base: 0x{base:X}\n")

    for name, rva in SLOT_RVAS.items():
        ptr = read_ptr(pm, base + rva)
        print(f"=== BFS from {name} (rva 0x{rva:X}, ptr 0x{(ptr or 0):X}) ===")
        if not is_valid_ptr(ptr):
            print("  invalid singleton ptr")
            continue
        matches, nodes_visited = trace(pm, ptr, f"{name}*")
        print(f"  scanned {nodes_visited} nodes, found {len(matches)} radius nodes")
        # Sort: more spirits per node = better, then shallower depth
        matches.sort(key=lambda m: (-len(m[3]), m[2]))
        for path, addr, d, hits in matches[:10]:
            print(f"\n  depth={d}  addr=0x{addr:X}  spirits found: {sorted(hits.keys())}")
            print(f"    path: {path}")


if __name__ == "__main__":
    main()
