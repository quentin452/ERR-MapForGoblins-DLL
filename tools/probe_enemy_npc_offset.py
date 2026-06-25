#!/usr/bin/env python3
"""Pin the MSBE Enemy PART-entry NPCParamID offset (+ the Enemy part-type @+0x0c)
for the disk-loot ENEMY pass.

For every Enemy part across the ERR _00 maps this:
  (a) reads the AUTHORITATIVE NPCParamID via SoulsFormats, then
  (b) replicates msbe_parser.parse_msb's raw section walk over the decompressed
      blob, locates the same PART entry by name, reads partType@+0x0c, and
      brute-forces which offset (inline u32, or via an entry-relative u64
      sub-struct pointer) holds NPCParamID — the byte offset the C++ parser
      needs to derive enemy loot (NPCParamID -> NpcParam.itemLotId_enemy/_map).

Run: PYTHONUTF8=1 PYTHONPATH="C:/Users/iamacat/AppData/Local/Temp/mfg_aux" \
     MFG_PROFILE=err py -3.14 tools/probe_enemy_npc_offset.py
"""
import sys, io, os, tempfile, struct
from collections import Counter, defaultdict
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pathlib import Path
from pythonnet import load
load('coreclr')
import clr
clr.AddReference(str(config.SOULSFORMATS_DLL))
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
import SoulsFormats

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
_str = SysType.GetType('System.String')
_msbe_read = asm.GetType('SoulsFormats.MSBE').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str]), None)

def rd32(b, o): return struct.unpack_from('<I', b, o)[0]
def rd64(b, o): return struct.unpack_from('<Q', b, o)[0]

SEC_PARTS = 5

def walk_sections(buf):
    secs = []
    po = rd32(buf, 0x08)
    for _ in range(6):
        offset_count = rd32(buf, po + 4)
        entries = offset_count - 1
        entry_arr = po + 0x10
        secs.append((entries, entry_arr))
        po = rd64(buf, entry_arr + entries * 8)
    return secs

def raw_part_entries(buf):
    """name -> (entry_off, partType)."""
    secs = walk_sections(buf)
    pt_entries, pt_arr = secs[SEC_PARTS]
    out = {}
    for i in range(pt_entries):
        pe = rd64(buf, pt_arr + i * 8)
        nm_off = pe + rd64(buf, pe + 0x00)
        s = bytearray()
        o = nm_off
        while o + 1 < len(buf):
            c = buf[o] | (buf[o + 1] << 8)
            if c == 0: break
            s.append(c if c < 0x80 else ord('?'))
            o += 2
        out[s.decode('ascii', 'replace')] = (pe, rd32(buf, pe + 0x0c))
    return out

def decompress(path):
    return bytes(SoulsFormats.DCX.Decompress(str(path)).ToArray())

def read_msb(path):
    data = decompress(path)
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_en.msb')
    SysFile.WriteAllBytes(tmp, data)
    msb = _msbe_read.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    return msb, data

def probe_offset(buf, pe, target):
    """Where does `target` (u32) live relative to part entry pe?
    Returns set of ('inline', off) and ('subptr+0xXX', subptr_off, inner_off)."""
    hits = []
    for off in range(0, 0x100, 4):              # inline u32
        if pe + off + 4 > len(buf): break
        if rd32(buf, pe + off) == target:
            hits.append(('inline', off, 0))
    for off in range(0x08, 0xa0, 8):            # entry-rel u64 sub-struct pointers
        if pe + off + 8 > len(buf): break
        rel = rd64(buf, pe + off)
        if rel == 0 or rel > 0x200000: continue
        sub = pe + rel
        for io2 in range(0, 0x80, 4):
            if sub + io2 + 4 > len(buf): break
            if rd32(buf, sub + io2) == target:
                hits.append(('subptr@+0x%02x' % off, off, io2))
    return hits

def main():
    msb_dir = config.require_err_mod_dir() / 'map' / 'MapStudio'
    print(f"MSB dir: {msb_dir}")
    part_type_counts = Counter()        # which @+0x0c value Enemy parts carry
    # tally of consistent NPCParamID locations
    inline_off = Counter()
    subptr_loc = Counter()              # (subptr_off, inner_off)
    total_enemies = 0
    matched = 0
    examples = []
    for path in sorted(msb_dir.glob('*_00.msb.dcx')):
        try:
            msb, buf = read_msb(path)
        except Exception:
            continue
        enemies = list(getattr(msb.Parts, 'Enemies', []) or [])
        if not enemies:
            continue
        raw = raw_part_entries(buf)
        for p in enemies:
            name = str(p.Name)
            npc = int(getattr(p, 'NPCParamID', 0) or 0)
            total_enemies += 1
            if name not in raw:
                continue
            pe, ptype = raw[name]
            part_type_counts[ptype] += 1
            if npc <= 0:
                continue
            hits = probe_offset(buf, pe, npc)
            if hits:
                matched += 1
                for mode, a, b in hits:
                    if mode == 'inline':
                        inline_off[a] += 1
                    else:
                        subptr_loc[(a, b)] += 1
                if len(examples) < 8:
                    examples.append((path.name, name, npc, ptype, hits))

    print(f"\nEnemy parts seen: {total_enemies}; with NPCParamID>0 and a raw match: {matched}")
    print("\n--- Enemy partType @+0x0c histogram ---")
    for v, n in part_type_counts.most_common():
        print(f"  type {v}: {n}")
    print("\n--- NPCParamID INLINE u32 offset histogram (off -> count) ---")
    for off, n in inline_off.most_common(12):
        print(f"  +0x{off:02x}: {n}")
    print("\n--- NPCParamID via SUBPTR (subptr_off, inner_off) histogram ---")
    for (a, b), n in subptr_loc.most_common(12):
        print(f"  subptr@+0x{a:02x} -> sub+0x{b:02x}: {n}")
    print("\n--- examples ---")
    for fn, name, npc, ptype, hits in examples:
        print(f"  {fn} {name} npc={npc} type={ptype}")
        for h in hits:
            print(f"      {h}")

if __name__ == '__main__':
    main()
