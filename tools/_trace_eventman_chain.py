#!/usr/bin/env python3
"""Walk CSEventMan / CSEventState / CSEmevdRepository singletons via
pointer-chasing to find where the kindling SFX radius-coroutines (or
their parent emevd event instances) live. Goal: a deterministic
traversal from a known global to entity_id 1045373501..505 — so the C++
DLL can stop brute-scanning all RW memory.

Strategy: BFS from each singleton, depth-first to a small depth, log
every node where an entity_id appears as int32 LE. Output the path
(offsets) so we can replay the chain in C++.

Game must be running, in m60_45_37_00 with at least one spirit alive.
"""
import struct
import sys
from collections import deque
from datetime import datetime
from pathlib import Path
import pymem

# Singleton RVAs from libER symbols
SINGLETONS = {
    "CSEventMan":         0x3D69638,
    "CSEventState":       0x3D68FB8,
    "CSEmevdRepository":  0x3D7D328,
    "WorldSfxMan":        0x3D6F5F8,
    "CSSfx":              0x3D7E1F8,
    # Reference: known-good (we already use these from collected.cpp)
    "CSWorldGeomMan":     0x3D69BA8,
}

KINDLING_NORMAL = [1045373501, 1045373502, 1045373503, 1045373504, 1045373505]
KINDLING_X = [1045373511, 1045373512, 1045373513, 1045373514, 1045373515]
ALL_TARGETS = set(KINDLING_NORMAL + KINDLING_X)
TWO_F_BYTES = b"\x00\x00\x00\x40"


def safe_read(pm, addr, size):
    try:
        return pm.read_bytes(int(addr), int(size))
    except Exception:
        return None


def is_valid_ptr(p):
    return p is not None and 0x10000 < p < 0x7FFFFFFFFFFF


def find_int32_offsets(data, target):
    needle = struct.pack("<I", target & 0xFFFFFFFF)
    offsets = []
    start = 0
    while True:
        i = data.find(needle, start)
        if i < 0:
            break
        offsets.append(i)
        start = i + 1
    return offsets


def find_radius_coroutine_offsets(data, target_entity):
    """Find positions where entity_id is followed by 2.0f (radius
    coroutine signature)."""
    needle = struct.pack("<I", target_entity) + TWO_F_BYTES
    offsets = []
    start = 0
    while True:
        i = data.find(needle, start)
        if i < 0:
            break
        offsets.append(i)
        start = i + 1
    return offsets


def trace_from_singleton(pm, base, name, rva, max_depth, max_nodes, max_bytes_per_node, report):
    """BFS pointer-walk from a singleton. Track every node where any
    kindling entity_id appears."""
    slot_addr = base + rva
    root_ptr = struct.unpack("<Q", safe_read(pm, slot_addr, 8) or b"\0" * 8)[0]
    report.append(f"\n{'=' * 70}")
    report.append(f"  {name} @ RVA 0x{rva:X}  ptr=0x{root_ptr:X}")
    report.append(f"{'=' * 70}")

    if not is_valid_ptr(root_ptr):
        report.append(f"  invalid singleton pointer")
        return []

    # Path: list of (parent_addr, parent_offset, child_addr)
    visited = {}  # addr -> (depth, path_str)
    queue = deque()
    queue.append((root_ptr, 0, f"{name}*"))
    visited[root_ptr] = (0, f"{name}*")

    matches = []  # (path, addr, hits_dict)

    nodes_scanned = 0
    while queue and nodes_scanned < max_nodes:
        addr, depth, path = queue.popleft()
        if depth > max_depth:
            continue
        data = safe_read(pm, addr, max_bytes_per_node)
        if data is None:
            continue
        nodes_scanned += 1

        # Check entity_id presence
        hits = {}
        for eid in ALL_TARGETS:
            offs = find_int32_offsets(data, eid)
            if offs:
                hits[eid] = offs
        if hits:
            # Filter for "real" radius coroutine: entity_id followed by 2.0f
            radius_hits = {}
            for eid in KINDLING_NORMAL:
                radius = find_radius_coroutine_offsets(data, eid)
                if radius:
                    radius_hits[eid] = radius
            kind = "RADIUS_COROUTINE" if radius_hits else "ENTITY_ID_ONLY"
            matches.append((path, addr, depth, hits, radius_hits, kind))

        # Enqueue child pointers (every aligned 8-byte slot in the data)
        for off in range(0, len(data) - 7, 8):
            child = struct.unpack_from("<Q", data, off)[0]
            if not is_valid_ptr(child):
                continue
            # Avoid revisiting; bias toward shallow visits
            existing = visited.get(child)
            new_depth = depth + 1
            if new_depth > max_depth:
                continue
            if existing is not None and existing[0] <= new_depth:
                continue
            new_path = f"{path}+0x{off:X}"
            visited[child] = (new_depth, new_path)
            queue.append((child, new_depth, new_path))

    report.append(f"  scanned {nodes_scanned} nodes (max {max_nodes}, depth {max_depth})")
    report.append(f"  found {len(matches)} nodes with kindling entity-ID hits")

    # Sort matches: radius coroutines first, then by depth
    matches.sort(key=lambda m: (m[5] != "RADIUS_COROUTINE", m[2]))
    for path, addr, depth, hits, radius_hits, kind in matches[:20]:
        n_normal = sum(1 for e in hits if e in KINDLING_NORMAL)
        n_x = sum(1 for e in hits if e in KINDLING_X)
        n_radius = len(radius_hits)
        report.append(f"\n  [{kind}] depth={depth}  addr=0x{addr:X}  normal={n_normal}/5  X={n_x}/5  radius={n_radius}")
        report.append(f"    path: {path}")
        if radius_hits:
            for eid, offs in sorted(radius_hits.items()):
                report.append(f"    radius[{eid}] at offsets {[hex(o) for o in offs]}")
        else:
            for eid, offs in sorted(hits.items()):
                report.append(f"    entity[{eid}] at offsets {[hex(o) for o in offs]}")
    if len(matches) > 20:
        report.append(f"    ... +{len(matches) - 20} more")

    return matches


def main():
    label = sys.argv[1] if len(sys.argv) > 1 else "trace"
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    out_dir = Path(__file__).parent.parent / "data" / "sfx_dumps" / f"chain_trace_{ts}_{label}"
    out_dir.mkdir(parents=True, exist_ok=True)

    pm = pymem.Pymem("eldenring.exe")
    base = pm.base_address

    report = []
    report.append(f"=== chain trace ({label}) {ts} ===")
    report.append(f"  eldenring base: 0x{base:X}")
    report.append(f"  targets (normal): {KINDLING_NORMAL}")
    report.append(f"  targets (X-variant): {KINDLING_X}")

    all_matches = {}  # singleton_name -> matches
    for name, rva in SINGLETONS.items():
        matches = trace_from_singleton(
            pm, base, name, rva,
            max_depth=5,         # how many pointer hops to follow
            max_nodes=2000,      # safety cap on BFS
            max_bytes_per_node=0x400,  # bytes to read per node
            report=report,
        )
        all_matches[name] = matches

    # Summary table
    report.append(f"\n{'=' * 70}")
    report.append(f"  SUMMARY")
    report.append(f"{'=' * 70}")
    for name, matches in all_matches.items():
        radius_count = sum(1 for m in matches if m[5] == "RADIUS_COROUTINE")
        any_count = len(matches)
        report.append(f"  {name:<22}: {radius_count} radius-coroutine nodes, {any_count} any-entity nodes")

    # Write
    out_path = out_dir / "report.txt"
    out_path.write_text("\n".join(report) + "\n", encoding="utf-8")
    print("\n".join(report))
    print(f"\nReport: {out_path}")


if __name__ == "__main__":
    main()
