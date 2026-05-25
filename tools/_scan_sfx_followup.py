#!/usr/bin/env python3
"""Follow-up scanner: find all 5 kindling SFX runtime structures and walk
back to a common manager. Run after _scan_sfx_manager.py reveals the
existence of the radius-check structures. Game must still be running.
"""
import os, sys, struct
from datetime import datetime
from pathlib import Path
import ctypes
from ctypes import wintypes
import pymem

KINDLING_NORMAL = [1045373501, 1045373502, 1045373503, 1045373504, 1045373505]
KINDLING_X = [1045373511, 1045373512, 1045373513, 1045373514, 1045373515]
ALL = set(KINDLING_NORMAL + KINDLING_X)


def safe_read(pm, addr, size):
    try:
        return pm.read_bytes(int(addr), int(size))
    except Exception:
        return None


def is_valid_ptr(p):
    return p is not None and 0x10000 < p < 0x7FFFFFFFFFFF


def hex_dump(data, base=0, width=16):
    out = []
    for i in range(0, len(data), width):
        chunk = data[i : i + width]
        hexs = " ".join(f"{b:02X}" for b in chunk)
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        out.append(f"  +{base + i:+06X}: {hexs:<{width * 3}}  {text}")
    return "\n".join(out)


class MBI(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_void_p),
        ("AllocationBase", ctypes.c_void_p),
        ("AllocationProtect", wintypes.DWORD),
        ("RegionSize", ctypes.c_size_t),
        ("State", wintypes.DWORD),
        ("Protect", wintypes.DWORD),
        ("Type", wintypes.DWORD),
    ]


def scan_all_kindling(pm):
    """Process-wide scan for all kindling entity IDs. Returns dict eid → [addrs]."""
    kernel32 = ctypes.windll.kernel32
    VirtualQueryEx = kernel32.VirtualQueryEx
    VirtualQueryEx.argtypes = [
        wintypes.HANDLE, ctypes.c_void_p,
        ctypes.POINTER(MBI), ctypes.c_size_t,
    ]
    VirtualQueryEx.restype = ctypes.c_size_t

    needles = {eid: struct.pack("<I", eid) for eid in ALL}
    hits = {eid: [] for eid in ALL}
    addr = 0x10000
    region_count = 0
    while addr < 0x7FFFFFFFFFFF:
        mbi = MBI()
        rc = VirtualQueryEx(pm.process_handle, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi))
        if rc == 0:
            break
        rsize = mbi.RegionSize
        if mbi.State == 0x1000 and (mbi.Protect & 0xFF) not in (0x01, 0x100):
            data = safe_read(pm, addr, min(rsize, 64 * 1024 * 1024))
            if data:
                region_count += 1
                for eid, needle in needles.items():
                    start = 0
                    while True:
                        i = data.find(needle, start)
                        if i < 0:
                            break
                        hits[eid].append(addr + i)
                        start = i + 1
        addr += rsize
    return hits, region_count


def main():
    label = sys.argv[1] if len(sys.argv) > 1 else "followup"
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    out_dir = Path(__file__).parent.parent / "data" / "sfx_dumps" / f"sfx_followup_{ts}_{label}"
    out_dir.mkdir(parents=True, exist_ok=True)
    log_path = out_dir / "report.txt"
    log = open(log_path, "w", encoding="utf-8")

    def out(s=""):
        print(s)
        log.write(s + "\n")

    out(f"=== {ts} {label} ===")
    pm = pymem.Pymem("eldenring.exe")
    base = pm.base_address
    out(f"base: 0x{base:X}")

    out("\n=== Process-wide scan: all 10 kindling entity IDs ===")
    hits, region_count = scan_all_kindling(pm)
    for eid in sorted(ALL):
        out(f"  {eid}: {len(hits[eid])} hit(s)")
    out(f"  scanned {region_count} memory regions")

    # Save raw hit list
    with open(out_dir / "all_hits.json", "w") as f:
        import json
        json.dump({str(eid): [hex(a) for a in addrs] for eid, addrs in hits.items()}, f, indent=2)

    # ── Categorise hits by surrounding pattern ──
    # Pattern A (emevd InitializeEvent table): 4 prev bytes = 1045372500 (agg)
    #   or 1045373500 (template) immediately before
    # Pattern B (radius coroutine): preceded by ptr to eldenring.exe text +
    #   followed by float 2.0 (40000000)
    # Pattern C (something else): record context for analysis

    out("\n=== Per-hit context dump (32 bytes window) ===")
    PATTERN_RADIUS = struct.pack("<f", 2.0)  # b'\x00\x00\x00\x40'
    PATTERN_RADIUS_PRE_OFFSETS = [4, 8, 12]  # how far after entity_id

    candidates = {}  # eid → list of (addr, kind) where kind ∈ {'radius', 'static_table', 'other'}
    for eid in sorted(ALL):
        out(f"\n--- entity {eid} ({len(hits[eid])} hits) ---")
        candidates[eid] = []
        for addr in hits[eid]:
            data = safe_read(pm, addr - 32, 96)
            if data is None:
                out(f"  0x{addr:X}: read failed")
                continue
            # Classify
            window = data[32 : 32 + 16]  # bytes after entity_id (entity is at offset 32 in our window)
            pre_window = data[28 - 4 : 28]  # 4 bytes immediately BEFORE entity_id
            kind = "other"
            # Static emevd table marker: prev 4 bytes is event-template ID 1045373500
            if pre_window == struct.pack("<I", 1045373500) or pre_window == struct.pack("<I", 1045372500):
                kind = "static_table"
            # Radius coroutine: entity_id followed by 2.0 then 1.0
            elif data[32 + 4 : 32 + 8] == PATTERN_RADIUS:
                kind = "radius_coroutine"
            candidates[eid].append((addr, kind))
            out(f"  0x{addr:X} [{kind}]")
            out(hex_dump(data, base=-32))

    # ── Aggregate radius-coroutine hits ──
    out("\n=== Radius-coroutine summary ===")
    radius_addrs = []
    for eid in KINDLING_NORMAL:
        for addr, kind in candidates.get(eid, []):
            if kind == "radius_coroutine":
                radius_addrs.append((eid, addr))
                out(f"  entity {eid}: 0x{addr:X}")
    if not radius_addrs:
        out("  none found — pattern miss?")
    else:
        out(f"  {len(radius_addrs)} radius-coroutine instance(s)")
        # Walk back to find a common manager: dump the ptr at +0x10 of each
        # (we saw it points at the same back-ref).
        out("\n=== Backref pointers (at entity+0x10) ===")
        for eid, addr in radius_addrs:
            ptr = struct.unpack("<Q", safe_read(pm, addr + 0x10, 8) or b"\0" * 8)[0]
            out(f"  entity {eid} @ 0x{addr:X} -> backref 0x{ptr:X}")

    # ── If 5 normal entities each have a radius hit AND they share a backref,
    # that backref structure is our manager. Walk it.
    out("\n=== Walk shared backref structures ===")
    backrefs = {}
    for eid, addr in radius_addrs:
        ptr = struct.unpack("<Q", safe_read(pm, addr + 0x10, 8) or b"\0" * 8)[0]
        backrefs.setdefault(ptr, []).append((eid, addr))
    for bp, kids in backrefs.items():
        if not is_valid_ptr(bp):
            continue
        out(f"\n  backref 0x{bp:X} shared by {len(kids)} entities: {[k[0] for k in kids]}")
        # Dump 0x100 bytes from backref
        bdata = safe_read(pm, bp, 0x200)
        if bdata:
            out(f"  backref dump (first 0x100 bytes):")
            out(hex_dump(bdata[:0x100], base=0))
            # Look for kindling entities in backref data
            for eid in ALL:
                if struct.pack("<I", eid) in bdata:
                    out(f"  >>> backref also contains entity {eid}!")

    # Also walk a level back: read pointer that points TO each radius_addr
    # (find by scanning small window of memory near each — overkill for now,
    # leave as TODO).

    # Save MBI ranges for the addresses involved (which heap they belong to)
    out("\n=== Heap region for each radius hit ===")
    kernel32 = ctypes.windll.kernel32
    VirtualQueryEx = kernel32.VirtualQueryEx
    VirtualQueryEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
    VirtualQueryEx.restype = ctypes.c_size_t
    for eid, addr in radius_addrs:
        mbi = MBI()
        VirtualQueryEx(pm.process_handle, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi))
        out(f"  0x{addr:X}: alloc_base=0x{mbi.AllocationBase or 0:X} region_size=0x{mbi.RegionSize:X} protect=0x{mbi.Protect:X}")

    log.close()
    out_path = out_dir / "report.txt"
    print(f"\nReport: {out_path}")


if __name__ == "__main__":
    main()
