#!/usr/bin/env python3
"""Deeper BFS from WorldSfxMan singleton, specifically looking for the
radius-coroutine signature (entity_id + 2.0f). If we find it via a
deterministic pointer chain, we never need brute-scan again."""
import struct, os, sys
from collections import deque
from pathlib import Path
from datetime import datetime
import pymem

os.environ.setdefault("PYTHONIOENCODING", "utf-8")

RVA_WORLD_SFX_MAN = 0x3D6F5F8
KINDLING_NORMAL = [1045373501, 1045373502, 1045373503, 1045373504, 1045373505]
TWO_F = b"\x00\x00\x00\x40"


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


def main():
    label = sys.argv[1] if len(sys.argv) > 1 else "deep"
    pm = pymem.Pymem("eldenring.exe")
    base = pm.base_address
    sfx = struct.unpack("<Q", pm.read_bytes(base + RVA_WORLD_SFX_MAN, 8))[0]
    print(f"WorldSfxMan = 0x{sfx:X}")

    visited = {}  # addr -> (depth, path)
    q = deque()
    q.append((sfx, 0, "WorldSfxMan*"))
    visited[sfx] = (0, "WorldSfxMan*")

    matches = []
    nodes = 0
    MAX_NODES = 50000  # bigger budget
    MAX_DEPTH = 8
    BYTES_PER_NODE = 0x800

    while q and nodes < MAX_NODES:
        addr, d, path = q.popleft()
        if d > MAX_DEPTH: continue
        data = safe_read(pm, addr, BYTES_PER_NODE)
        if data is None: continue
        nodes += 1

        # Check for radius-coroutine signature
        radius_hits = {}
        for eid in KINDLING_NORMAL:
            offs = find_radius_offsets(data, eid)
            if offs:
                radius_hits[eid] = offs
        if radius_hits:
            matches.append((path, addr, d, radius_hits))

        # Enqueue children
        for off in range(0, len(data) - 7, 8):
            child = struct.unpack_from("<Q", data, off)[0]
            if not is_valid_ptr(child): continue
            new_d = d + 1
            if new_d > MAX_DEPTH: continue
            if child in visited and visited[child][0] <= new_d: continue
            new_path = f"{path}+0x{off:X}"
            visited[child] = (new_d, new_path)
            q.append((child, new_d, new_path))

    print(f"\nScanned {nodes} nodes, max depth {MAX_DEPTH}")
    print(f"Found {len(matches)} nodes with radius-coroutine signature")
    for path, addr, d, hits in matches[:20]:
        print(f"\n  depth={d}  addr=0x{addr:X}  hits={len(hits)}/5")
        print(f"    path: {path}")
        for eid, offs in sorted(hits.items()):
            print(f"    eid {eid} @ {[hex(o) for o in offs]}")

    if not matches:
        print("\nNO chain to radius coroutines found within depth/budget.")


if __name__ == "__main__":
    main()
