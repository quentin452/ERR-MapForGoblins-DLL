#!/usr/bin/env python3
"""Find the per-instance "active SFX" container that backs Spawn Map SFX
/ Delete Map SFX (EMEVD instructions 2006:1 and 2006:2). Run while ERR is
loaded with the player standing in m60_45_37_00 — all 5 Kindling Spirit
SFX (entity IDs 1045373501..505) should be alive.

The script:
  1. Attaches to eldenring.exe via pymem.
  2. Reads WorldSfxMan @ RVA 0x3D6F5F8 and CSSfxImp @ 0x3D7E1F8 (singleton
     RVAs from libER/symbols/singletons.csv).
  3. Searches each managed region (and the singleton's own bytes) for the
     5 entity IDs as int32 LE — reports every hit + 0x40 surrounding
     context.
  4. Dumps WorldBlockSfx[192] entries, identifies the one keyed on
     block_id 0x3C2D2500 (m60_45_37_00), and dumps that block's full
     header + a wide window for further analysis.
  5. Walks CSSfxImp's three Trees (offsets 0x10/0x28/0xb0). For each
     tree root, prints node count and value bytes from the first 32 nodes
     so we can spot entity-ID-keyed entries.

Output: dumps/sfx_scan_<timestamp>/ with per-region binaries + a text
report. Re-run AFTER picking up one spirit; diff the dumps to confirm
which entry corresponds to which entity_id and where the "alive" bit
lives.

Requires admin (pymem requires PROCESS_VM_READ).
"""

import os
import sys
import struct
import json
from datetime import datetime
from pathlib import Path

import pymem
import pymem.process

# ─── Constants (verified from libER/symbols/singletons.csv) ────────────

RVA_WORLD_SFX_MAN = 0x3D6F5F8   # GLOBAL_WorldSfxMan = 64419320
RVA_CS_SFX        = 0x3D7E1F8   # GLOBAL_CSSfx       = 64502200

# Reference: the existing CSWorldGeomMan/GeomFlag/GeomNonActive RVAs from
# goblin_collected.cpp. Useful as a sanity baseline (the deref of those
# is known to work).
RVA_GEOM_FLAG      = 0x3D69D18
RVA_GEOM_NONACTIVE = 0x3D69D98
RVA_WORLD_GEOM_MAN = 0x3D69BA8

# Kindling Spirit SFX entity IDs in m60_45_37_00.
KINDLING_SPIRITS = [1045373501, 1045373502, 1045373503, 1045373504, 1045373505]
# X-variants (visual "burnt out") — same positions, separate entity IDs.
KINDLING_X_VARIANTS = [1045373511, 1045373512, 1045373513, 1045373514, 1045373515]
ALL_KINDLING_IDS = set(KINDLING_SPIRITS + KINDLING_X_VARIANTS)

# m60_45_37_00 → BlockId encoding (area<<24 | gridX<<16 | gridZ<<8 | idx)
M60_45_37_00_BLOCK_ID = (60 << 24) | (45 << 16) | (37 << 8) | 0   # = 0x3C2D2500


# ─── Helpers ─────────────────────────────────────────────────────────


def safe_read(pm, addr, size):
    try:
        return pm.read_bytes(int(addr), int(size))
    except Exception:
        return None


def read_ptr(pm, addr):
    d = safe_read(pm, addr, 8)
    return struct.unpack("<Q", d)[0] if d else None


def read_u32(pm, addr):
    d = safe_read(pm, addr, 4)
    return struct.unpack("<I", d)[0] if d else None


def is_valid_ptr(p):
    return p is not None and 0x10000 < p < 0x7FFFFFFFFFFF


def hex_dump(data, base=0, width=16):
    out = []
    for i in range(0, len(data), width):
        chunk = data[i : i + width]
        hexs = " ".join(f"{b:02X}" for b in chunk)
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        out.append(f"  +{base + i:06X}: {hexs:<{width * 3}}  {text}")
    return "\n".join(out)


def find_int32_offsets(data, target):
    """Return all byte offsets where data has int32 LE == target."""
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


# ─── Output sink ─────────────────────────────────────────────────────


class Report:
    def __init__(self, root):
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        (self.root / "blocks").mkdir(exist_ok=True)
        self.fp = open(self.root / "report.txt", "w", encoding="utf-8")

    def line(self, s=""):
        print(s)
        self.fp.write(s + "\n")

    def section(self, title):
        bar = "=" * 70
        self.line()
        self.line(bar)
        self.line(f"  {title}")
        self.line(bar)

    def save_bin(self, name, data):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def close(self):
        self.fp.close()


# ─── Investigation passes ────────────────────────────────────────────


def dump_singleton(pm, base, rva, name, dump_size, report):
    """Read the pointer at base+rva, dump dump_size bytes from there."""
    report.section(f"{name} @ RVA 0x{rva:X}")
    slot_addr = base + rva
    ptr_val = read_ptr(pm, slot_addr)
    report.line(f"  slot addr     : 0x{slot_addr:X}")
    report.line(f"  pointer value : 0x{(ptr_val or 0):X}")
    if not is_valid_ptr(ptr_val):
        report.line(f"  INVALID — singleton not initialized?")
        return None, None
    data = safe_read(pm, ptr_val, dump_size)
    if data is None:
        report.line(f"  read failed at 0x{ptr_val:X}")
        return ptr_val, None
    report.line(f"  read {len(data)} bytes from 0x{ptr_val:X}")
    fname = f"singleton_{name.replace(' ', '_')}.bin"
    p = report.save_bin(fname, data)
    report.line(f"  saved to {p.name}")
    return ptr_val, data


def search_entity_ids(data, base_addr, label, report):
    """Search for all kindling entity IDs in the dumped data."""
    report.line()
    report.line(f"--- entity-ID search in {label} ---")
    any_found = False
    for eid in sorted(ALL_KINDLING_IDS):
        offsets = find_int32_offsets(data, eid)
        if not offsets:
            continue
        any_found = True
        report.line(f"  entity {eid} (0x{eid:08X}) — {len(offsets)} hit(s):")
        for off in offsets[:10]:
            abs_addr = base_addr + off
            ctx_start = max(0, off - 16)
            ctx_end = min(len(data), off + 32)
            report.line(f"    +0x{off:X}  abs 0x{abs_addr:X}")
            report.line(hex_dump(data[ctx_start:ctx_end], base=ctx_start))
        if len(offsets) > 10:
            report.line(f"    ... +{len(offsets) - 10} more")
    if not any_found:
        report.line("  no kindling entity IDs found")
    return any_found


def walk_world_block_sfx_list(pm, sfx_man_ptr, report):
    """Walk world_block_sfx_list[192] looking for the m60_45_37_00 block."""
    report.section("WorldSfxMan.world_block_sfx_list[192]")
    if not is_valid_ptr(sfx_man_ptr):
        report.line("  invalid sfx_man_ptr")
        return None

    # Layout (from fromsoftware-rs/cs/world_sfx_man.rs):
    #   +0x18 world_area_sfx_count   u32
    #   +0x20 world_area_sfx_list    ptr
    #   +0x28 world_block_sfx_count  u32   <-- expect 192
    #   +0x30 world_block_sfx_list   ptr
    block_count = read_u32(pm, sfx_man_ptr + 0x28)
    block_list_ptr = read_ptr(pm, sfx_man_ptr + 0x30)
    report.line(f"  world_block_sfx_count  = {block_count}")
    report.line(f"  world_block_sfx_list   = 0x{(block_list_ptr or 0):X}")
    if not is_valid_ptr(block_list_ptr) or not block_count or block_count > 1024:
        report.line("  invalid count/list — bailing")
        return None

    # WorldBlockSfx layout:
    #   +0x00 vftable
    #   +0x08 world_block_info
    #   +0x10 world_area_sfx -> parent
    #   +0x18 block_id (BlockId, 4 bytes? check)
    #   +0x1c [0x40] unmapped
    #   +0x5c total_sfx_count u32
    #   +0x60 unk60 (likely instance list ptr)
    # Need to determine entry stride. Try 0x80 first (matches ER conventions).
    STRIDE_GUESSES = [0x80, 0xA0, 0xC0, 0xE0, 0x100]
    target_block_addr = None
    target_stride = None

    for stride in STRIDE_GUESSES:
        # Probe: assume stride, look at first 4 entries' block_ids
        ids = []
        for i in range(min(8, block_count)):
            bid = read_u32(pm, block_list_ptr + i * stride + 0x18)
            ids.append(bid)
        report.line(f"  stride 0x{stride:X}: first 8 block_ids = {[f'0x{x:08X}' if x else 'NULL' for x in ids]}")

    # Pick the stride that yields the most plausible-looking block_ids
    # (high byte 0x0A..0x3D = area 10..61; ER convention).
    best_stride = None
    best_score = -1
    for stride in STRIDE_GUESSES:
        score = 0
        for i in range(min(64, block_count)):
            bid = read_u32(pm, block_list_ptr + i * stride + 0x18)
            if bid is None:
                continue
            area = (bid >> 24) & 0xFF
            if 0x0A <= area <= 0x3D:
                score += 1
        report.line(f"  stride 0x{stride:X}: plausible-block-id score = {score}/64")
        if score > best_score:
            best_score = score
            best_stride = stride
    report.line(f"  picked stride: 0x{best_stride:X}")
    target_stride = best_stride

    # Find the m60_45_37_00 block
    for i in range(block_count):
        bid = read_u32(pm, block_list_ptr + i * target_stride + 0x18)
        if bid == M60_45_37_00_BLOCK_ID:
            target_block_addr = block_list_ptr + i * target_stride
            report.line(f"  FOUND m60_45_37_00 at index {i}, addr 0x{target_block_addr:X}")
            break

    if target_block_addr is None:
        report.line(f"  m60_45_37_00 (block_id 0x{M60_45_37_00_BLOCK_ID:08X}) not found")
        report.line("  dumping all loaded blocks (block_id != 0):")
        for i in range(block_count):
            bid = read_u32(pm, block_list_ptr + i * target_stride + 0x18)
            if bid:
                report.line(f"    [{i:3d}] block_id = 0x{bid:08X}")
        return None

    # Dump the target block
    block_data = safe_read(pm, target_block_addr, max(0x200, target_stride))
    if block_data:
        p = report.save_bin(f"blocks/m60_45_37_00_block.bin", block_data)
        report.line(f"  saved block data to {p.name}")
        report.line("  block hex dump (first 0x100 bytes):")
        report.line(hex_dump(block_data[:0x100], base=0))

        # Dereference any ptr-looking u64s in the block, and search each
        # for kindling entity IDs.
        report.line()
        report.line("  --- pointer-deref search ---")
        for off in range(0, min(len(block_data), 0x100), 8):
            ptr = struct.unpack("<Q", block_data[off : off + 8])[0]
            if not is_valid_ptr(ptr):
                continue
            sub = safe_read(pm, ptr, 0x800)
            if sub is None:
                continue
            for eid in ALL_KINDLING_IDS:
                if struct.pack("<I", eid) in sub:
                    report.line(f"  +0x{off:X} ptr 0x{ptr:X}: contains entity {eid}!")
                    p2 = report.save_bin(f"blocks/deref_+0x{off:02X}.bin", sub)
                    report.line(f"    saved to {p2.name}")
                    break

    return target_block_addr


def walk_other_lists(pm, sfx_man_ptr, report):
    """Probe world_area_sfx_list (28) and world_grid_area_sfx_list (6)."""
    report.section("WorldSfxMan: area_sfx_list (28) + grid_area_sfx_list (6)")
    if not is_valid_ptr(sfx_man_ptr):
        return

    # +0x18 area_count, +0x20 area_list, +0x38 grid_count, +0x40 grid_list
    area_count = read_u32(pm, sfx_man_ptr + 0x18)
    area_list_ptr = read_ptr(pm, sfx_man_ptr + 0x20)
    grid_count = read_u32(pm, sfx_man_ptr + 0x38)
    grid_list_ptr = read_ptr(pm, sfx_man_ptr + 0x40)

    report.line(f"  area_count={area_count} area_list=0x{(area_list_ptr or 0):X}")
    report.line(f"  grid_count={grid_count} grid_list=0x{(grid_list_ptr or 0):X}")

    for label, ptr, count in (("area", area_list_ptr, area_count), ("grid", grid_list_ptr, grid_count)):
        if not is_valid_ptr(ptr) or not count or count > 256:
            continue
        # Try several strides
        for stride in (0x28, 0x40, 0x60, 0x80, 0xA0):
            data = safe_read(pm, ptr, stride * count)
            if data is None:
                continue
            # Count entries where the first 8 bytes look like a vftable ptr
            vftable_count = 0
            for i in range(count):
                v = struct.unpack_from("<Q", data, i * stride)[0]
                if 0x7FF000000000 < v < 0x7FFFFFFFFFFF:
                    vftable_count += 1
            report.line(f"  {label}-list stride 0x{stride:X}: {vftable_count}/{count} entries with plausible vftable")


def scan_block_for_kindling(pm, block_addr, stride, depth, report):
    """Aggressively probe a block for kindling entity IDs by following pointers."""
    visited = set()
    queue = [(block_addr, 0, "block_root")]
    found_anywhere = False
    while queue and len(visited) < 200:
        addr, d, label = queue.pop(0)
        if addr in visited or d > depth:
            continue
        visited.add(addr)
        size = 0x800 if d == 0 else 0x400
        data = safe_read(pm, addr, size)
        if data is None:
            continue
        for eid in ALL_KINDLING_IDS:
            offs = find_int32_offsets(data, eid)
            for off in offs:
                found_anywhere = True
                report.line(f"    [{label} d={d}] entity {eid} at 0x{addr + off:X} (in 0x{addr:X}+0x{off:X})")
                ctx_start = max(0, off - 16)
                report.line(hex_dump(data[ctx_start : min(len(data), off + 32)], base=ctx_start))
        # Follow ptrs
        for off in range(0, len(data), 8):
            v = struct.unpack_from("<Q", data, off)[0]
            if is_valid_ptr(v) and v not in visited:
                queue.append((v, d + 1, f"{label}+0x{off:X}"))
    return found_anywhere


def walk_cssfx_trees(pm, cssfx_ptr, report):
    """Walk the three Tree<()> members of CSSfxImp at offsets 0x10/0x28/0xb0
    looking for kindling entity IDs in node values."""
    report.section("CSSfxImp Trees (0x10, 0x28, 0xb0)")
    if not is_valid_ptr(cssfx_ptr):
        report.line("  invalid cssfx_ptr")
        return

    for tree_off in (0x10, 0x28, 0xB0):
        # Microsoft STL std::map / red-black tree typical layout:
        #   tree_obj +0x00 alloc/cmp
        #   tree_obj +0x08 head_node ptr
        #   tree_obj +0x10 size u64
        # Each node:
        #   +0x00 left   +0x08 parent  +0x10 right
        #   +0x18 colour/leaf flags
        #   +0x20 value (key+payload, layout depends on map type)
        head_node = read_ptr(pm, cssfx_ptr + tree_off + 0x08)
        size = read_ptr(pm, cssfx_ptr + tree_off + 0x10)
        report.line()
        report.line(f"  tree @ +0x{tree_off:X}: head=0x{(head_node or 0):X} size={size if size is not None else '?'}")
        if not is_valid_ptr(head_node) or not size or size > 1_000_000:
            continue

        # Walk by following parent of head = root, then DFS up to N nodes.
        root = read_ptr(pm, head_node + 0x08)
        if not is_valid_ptr(root):
            report.line("    no root")
            continue

        visited = set()
        stack = [root]
        n = 0
        report.line(f"    walking up to 64 nodes from root 0x{root:X}:")
        while stack and n < 64:
            node = stack.pop()
            if node in visited:
                continue
            visited.add(node)
            n += 1
            # Read the value blob at +0x20
            value_data = safe_read(pm, node + 0x20, 0x40)
            if value_data is None:
                continue
            # Look for kindling entity IDs anywhere in the value blob
            hits = []
            for eid in ALL_KINDLING_IDS:
                offs = find_int32_offsets(value_data, eid)
                for o in offs:
                    hits.append((eid, o))
            if hits:
                report.line(f"    node 0x{node:X} matches:")
                for eid, off in hits:
                    report.line(f"      entity {eid} at value+0x{off:X}")
                report.line(f"      value bytes:")
                report.line(hex_dump(value_data, base=0))
            # Push children (simple DFS, leaks bounded by visited check)
            for child_off in (0x00, 0x10):  # left, right
                child = read_ptr(pm, node + child_off)
                if is_valid_ptr(child) and child != head_node:
                    stack.append(child)


# ─── Entry point ─────────────────────────────────────────────────────


def main():
    label = sys.argv[1] if len(sys.argv) > 1 else "scan"
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    out_dir = Path(__file__).parent.parent / "data" / "sfx_dumps" / f"sfx_scan_{ts}_{label}"
    report = Report(out_dir)

    report.section("Setup")
    report.line(f"  output dir: {out_dir}")
    try:
        pm = pymem.Pymem("eldenring.exe")
    except pymem.exception.ProcessNotFound:
        report.line("ERROR: eldenring.exe not running")
        return 1
    base = pm.base_address
    report.line(f"  eldenring.exe base: 0x{base:X}")
    report.line(f"  process handle    : 0x{pm.process_handle:X}")
    report.line(f"  kindling entity IDs (probe targets): {sorted(ALL_KINDLING_IDS)}")
    report.line(f"  m60_45_37_00 block_id           : 0x{M60_45_37_00_BLOCK_ID:08X}")

    # Pass 1: WorldSfxMan
    sfx_man_ptr, sfx_man_data = dump_singleton(
        pm, base, RVA_WORLD_SFX_MAN, "WorldSfxMan", 0x6300, report
    )
    if sfx_man_data:
        search_entity_ids(sfx_man_data, sfx_man_ptr, "WorldSfxMan singleton", report)

    # Pass 2: CSSfxImp
    cssfx_ptr, cssfx_data = dump_singleton(
        pm, base, RVA_CS_SFX, "CSSfxImp", 0x300, report
    )
    if cssfx_data:
        search_entity_ids(cssfx_data, cssfx_ptr, "CSSfxImp singleton", report)

    # Pass 3: walk world_block_sfx_list, find m60_45_37_00 block, deref ptrs
    if is_valid_ptr(sfx_man_ptr):
        walk_world_block_sfx_list(pm, sfx_man_ptr, report)
        walk_other_lists(pm, sfx_man_ptr, report)

    # Pass 4: walk CSSfxImp trees
    if is_valid_ptr(cssfx_ptr):
        walk_cssfx_trees(pm, cssfx_ptr, report)

    # Pass 5: brute-force pointer-walk both singletons looking for entity IDs
    report.section("Brute-force pointer-walk (depth 4)")
    for ptr, name in ((sfx_man_ptr, "WorldSfxMan"), (cssfx_ptr, "CSSfxImp")):
        if is_valid_ptr(ptr):
            report.line(f"--- {name} pointer-walk ---")
            found = scan_block_for_kindling(pm, ptr, 0x80, 4, report)
            if not found:
                report.line(f"  no kindling IDs found within 4 levels of pointer indirection")

    # Pass 6: process-wide search for entity ID 1045373501 to confirm it
    # exists in RAM at all (even if not where we expected).
    report.section("Process-wide scan for entity 1045373501")
    report.line("  scanning all committed memory regions for int32 LE 0x3E4F223D...")
    needle = struct.pack("<I", 1045373501)
    hits = []
    import ctypes
    from ctypes import wintypes
    kernel32 = ctypes.windll.kernel32
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
    VirtualQueryEx = kernel32.VirtualQueryEx
    VirtualQueryEx.argtypes = [
        wintypes.HANDLE, ctypes.c_void_p,
        ctypes.POINTER(MBI), ctypes.c_size_t
    ]
    VirtualQueryEx.restype = ctypes.c_size_t
    addr = 0x10000
    region_count = 0
    bytes_scanned = 0
    while addr < 0x7FFFFFFFFFFF and len(hits) < 200:
        mbi = MBI()
        rc = VirtualQueryEx(pm.process_handle, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi))
        if rc == 0:
            break
        region_size = mbi.RegionSize
        if mbi.State == 0x1000 and (mbi.Protect & 0xFF) not in (0x01, 0x100):
            data = safe_read(pm, addr, min(region_size, 64 * 1024 * 1024))
            if data:
                bytes_scanned += len(data)
                start = 0
                while True:
                    i = data.find(needle, start)
                    if i < 0:
                        break
                    hits.append(addr + i)
                    if len(hits) >= 200:
                        break
                    start = i + 1
                region_count += 1
        addr += region_size
    report.line(f"  scanned {region_count} regions, {bytes_scanned/1e6:.1f} MB")
    report.line(f"  found {len(hits)} hits for 1045373501 (truncated to 200)")
    for i, h in enumerate(hits[:30]):
        # Read 64-byte context
        data = safe_read(pm, h - 16, 64) or b""
        report.line(f"  hit {i + 1}: 0x{h:X}")
        report.line(hex_dump(data, base=-16))

    report.section("DONE")
    report.line(f"  results in: {out_dir}")
    report.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
